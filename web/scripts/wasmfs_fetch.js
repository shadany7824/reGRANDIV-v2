/**
 * [web port] WasmFS's fetch backend, for a page served over the internet
 * (link.sh: --js-library; replaces Emscripten's libwasmfs_fetch.js function).
 *
 * Emscripten's own reads one chunk at a time, keeps every chunk it ever read
 * (the whole 6.6 GB install, in JavaScript memory), asks for the size with a
 * HEAD request first and gives up on the first failed request. Over a home
 * connection seen from elsewhere (one stream: well under 1 MB/s, a dropped
 * request now and then) the install took hours and stopped at the first error.
 * This one:
 * - reads 1 MiB blocks; the first request is a range GET whose Content-Range
 *   gives the size (no HEAD); a server that ignores ranges sends the whole file;
 * - reads ahead while a file is read in order (the installer copies files
 *   front to back), up to READ_AHEAD bytes, in requests of up to RUN blocks,
 *   several at once (LANES): the speed of one stream stops mattering;
 * - keeps at most CACHE bytes, least recently used first out;
 * - retries a failed or cut request (TRIES times, waiting longer each time);
 * - posts the bytes it fetched on the 'gta4-net' BroadcastChannel twice a
 *   second, for the loading screen's speed.
 * The backend's URL is a file's, or a folder's that files are created in
 * (web_startup.cpp: the URL is the folder's plus the file's path in it).
 */

addToLibrary({
  _wasmfs_create_fetch_backend_js__deps: [
    '$wasmFS$backends',
    '_wasmfs_fetch_get_file_url',
  ],
  _wasmfs_create_fetch_backend_js: async function(backend) {
    const BLOCK = 1 << 20, RUN = 4, READ_AHEAD = 32 << 20, LANES = 8, CACHE = 320 << 20, TRIES = 8;
    const files = new Map();      // file pointer -> {url, size, blocks: Map(index -> Uint8Array), pending: Map(index -> Promise), cursors}
    const lru = new Map();        // `${file}:${index}` -> [state, index], oldest first
    let cached = 0, active = 0, fetched = 0;
    const waiting = [];           // requests waiting for a free lane
    const channel = typeof BroadcastChannel == 'function' ? new BroadcastChannel('gta4-net') : null;
    if (channel) setInterval(() => { if (fetched) { channel.postMessage({ dl: fetched }); fetched = 0; } }, 500);

    const urlOf = (file) => {
      const raw = UTF8ToString(__wasmfs_fetch_get_file_url(file));
      // (a file in a fetched folder: the folder's URL, '/', then its path, which starts with '/')
      return new URL(raw.replace(/\/{2,}/g, '/'), self.location.origin).toString();
    };
    const lane = () => active < LANES ? (active++, Promise.resolve()) : new Promise((go) => waiting.push(go));
    const free = () => { const go = waiting.shift(); if (go) go(); else active--; };

    // Bytes [start, end] of the file (end: -1 = from start to its end), retried; resolves to
    // {bytes, size} (size: the file's, from Content-Range, or the body's when the server sent it whole).
    async function get(url, start, end, priority) {
      await lane();
      try {
        for (let attempt = 1; ; attempt++) {
          try {
            const response = await fetch(url, { headers: { Range: `bytes=${start}-${end < 0 ? '' : end}` }, priority });
            if (response.status == 404) throw { status: 404 };
            if (!response.ok) throw new Error('HTTP ' + response.status);
            const bytes = new Uint8Array(await response.arrayBuffer());
            fetched += bytes.length;
            if (response.status == 200) return { bytes, size: bytes.length, whole: true };
            const range = /\/(\d+)\s*$/.exec(response.headers.get('Content-Range') || '');
            if (end >= 0 && bytes.length != end - start + 1 && (!range || start + bytes.length != +range[1])) throw new Error('short body');
            return { bytes, size: range ? +range[1] : start + bytes.length, whole: false };
          } catch (e) {
            if (e && e.status == 404) throw e;
            if (attempt >= TRIES) { err(`web fetch: ${url} bytes ${start}-${end} failed ${attempt} times: ${e}`); throw { status: 0 }; }
            if (attempt == 2) err(`web fetch: ${url} bytes ${start}-${end}: ${e}; retrying`);
            await new Promise((r) => setTimeout(r, Math.min(15000, 500 * 2 ** attempt)));
          }
        }
      } finally { free(); }
    }

    function remember(state, index, bytes) {
      if (state.whole) return;
      state.blocks.set(index, bytes);
      const key = state.id + ':' + index;
      lru.delete(key);
      lru.set(key, [state, index]);
      cached += bytes.length;
      for (const [k, [s, i]] of lru) {
        if (cached <= CACHE) break;
        if (s.pinned && s.pinned.has(i)) continue;
        const b = s.blocks.get(i);
        if (b) { cached -= b.length; s.blocks.delete(i); }
        lru.delete(k);
      }
    }
    function touch(state, index) {
      const key = state.id + ':' + index, entry = lru.get(key);
      if (entry) { lru.delete(key); lru.set(key, entry); }
    }

    // The file's state, with its size known (the first block is read for it).
    async function open(file) {
      let state = files.get(file);
      if (state && state.ready) { await state.ready; return state; }
      state = { id: file, url: urlOf(file), size: 0, blocks: new Map(), pending: new Map(), cursors: [], whole: null, pinned: new Set() };
      files.set(file, state);
      state.ready = (async () => {
        const first = await get(state.url, 0, BLOCK - 1, 'high');
        state.size = first.size;
        if (first.whole) state.whole = first.bytes;
        else remember(state, 0, first.bytes);
      })();
      try { await state.ready; } catch (e) { files.delete(file); throw e; }
      return state;
    }

    // Starts fetching blocks [a, b] (those not there or on their way), in runs of at most RUN.
    function want(state, a, b, priority) {
      const last = Math.ceil(state.size / BLOCK) - 1;
      b = Math.min(b, last);
      for (let i = a; i <= b; ) {
        if (state.blocks.has(i) || state.pending.has(i)) { i++; continue; }
        let j = i;
        while (j + 1 <= b && j + 1 - i < RUN && !state.blocks.has(j + 1) && !state.pending.has(j + 1)) j++;
        const from = i, to = j;
        const run = get(state.url, from * BLOCK, Math.min(state.size, (to + 1) * BLOCK) - 1, priority).then(({ bytes }) => {
          for (let k = from; k <= to; k++) remember(state, k, bytes.subarray((k - from) * BLOCK, (k - from + 1) * BLOCK));
        }).finally(() => { for (let k = from; k <= to; k++) state.pending.delete(k); });
        for (let k = from; k <= to; k++) state.pending.set(k, run);
        run.catch(() => {});
        i = j + 1;
      }
    }

    async function blocks(state, a, b) {
      for (let tries = 0; ; tries++) {
        want(state, a, b, 'high');
        const missing = [];
        for (let i = a; i <= b; i++) if (!state.blocks.has(i)) missing.push(state.pending.get(i));
        if (!missing.length) return;
        await Promise.all(missing);
        if (tries > 3) throw { status: 0 };   // (evicted while waiting, again and again)
      }
    }

    wasmFS$backends[backend] = {
      allocFile: async (file) => {},
      freeFile: async (file) => {
        const state = files.get(file);
        if (!state) return;
        for (const [i, b] of state.blocks) { cached -= b.length; lru.delete(file + ':' + i); }
        files.delete(file);
      },
      write: async (file, buffer, length, offset) => -{{{ cDefs.EBADF }}},
      read: async (file, buffer, length, offset) => {
        if (offset < 0 || length <= 0) return 0;
        let state;
        try { state = await open(file); } catch (e) { return e && e.status == 404 ? -{{{ cDefs.ENOENT }}} : -{{{ cDefs.EIO }}}; }
        length = Math.min(length, state.size - offset);
        if (length <= 0) return 0;
        if (state.whole) {
          HEAPU8.set(state.whole.subarray(offset, offset + length), buffer);
          return length;
        }
        const a = Math.floor(offset / BLOCK), b = Math.floor((offset + length - 1) / BLOCK);
        // (in order: a read starts where an earlier one ended, or in its last block. The installer
        // copies several files of the disc at once, so a few such cursors are followed per file.)
        let cursor = state.cursors.find((c) => offset == c.next || a == Math.floor((c.next - 1) / BLOCK) || a == Math.floor(c.next / BLOCK));
        if (cursor) state.cursors.splice(state.cursors.indexOf(cursor), 1);
        else cursor = { next: 0, streak: -1 };
        cursor.streak++;
        cursor.next = offset + length;
        state.cursors.unshift(cursor);
        state.cursors.length = Math.min(state.cursors.length, 8);
        if (cursor.streak >= 2) want(state, b + 1, b + Math.max(1, Math.round(READ_AHEAD * Math.min(1, cursor.streak / 16) / BLOCK)), 'low');
        for (let i = a; i <= b; i++) state.pinned.add(i);
        try { await blocks(state, a, b); }
        catch (e) { return -{{{ cDefs.EIO }}}; }
        finally { for (let i = a; i <= b; i++) state.pinned.delete(i); }
        for (let i = a; i <= b; i++) {
          const block = state.blocks.get(i), start = Math.max(i * BLOCK, offset), end = Math.min((i + 1) * BLOCK, offset + length);
          HEAPU8.set(block.subarray(start - i * BLOCK, end - i * BLOCK), buffer + (start - offset));
          // (a block read to its end in order isn't read again: let it go, not the read-ahead
          // blocks after it)
          if (cursor.streak >= 2 && end == (i + 1) * BLOCK) {
            state.blocks.delete(i);
            cached -= block.length;
            lru.delete(state.id + ':' + i);
          } else touch(state, i);
        }
        return length;
      },
      getSize: async (file) => {
        try { return (await open(file)).size; } catch (e) { return 0; }
      },
    };
  },
});
