#!/usr/bin/env python3
"""Local test server for the GTA IV web build.

Serves a directory with the cross-origin isolation headers shared-memory
WebAssembly needs (COOP/COEP/CORP), supports single byte-range requests, and
maps /iso/gta4.iso, /iso/tu8 and /iso/aes_key to the disc image, Title
Update 8 and the RPF key on disk, and /app0/hub and /app0/fonts to the PS5
hub's assets (GTA4_PS5_DIR, default the repository's ps5/).
The hub's install folder is a real folder on this computer (GTA4_WEB_FILES,
default the repository's web/files): listed at
/host/list/install, read at /host/file/install/<path>. (Xbox 360 saves aren't
imported from here: each player picks theirs in the browser.)
Listens on 127.0.0.1 only, unless GTA4_WEB_LAN=1: then it serves the Wi-Fi
too, over HTTPS (a browser on another device only runs the game from a secure
page), with a certificate from a local certificate authority (lan-cert.sh,
kept in GTA4_WEB_FILES/lan-cert). A plain HTTP page on <port + 1> hands the
iPad or phone that authority's certificate to install and trust, once. The
upload route is off then.

Usage: serve.py <site dir> <iso path> <title update path> <rpf key path> [port]
"""

import gzip
import http.server
import os
import re
import hashlib
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import urllib.parse

SITE = os.path.abspath(sys.argv[1])
ISO = os.path.abspath(sys.argv[2])
TU8 = os.path.abspath(sys.argv[3])
KEY = os.path.abspath(sys.argv[4])
PORT = int(sys.argv[5]) if len(sys.argv) > 5 else 8765
LAN = os.environ.get("GTA4_WEB_LAN") == "1"
# (the repository: two folders up from this script, in web/scripts or web/build)
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
PS5 = os.path.abspath(os.environ.get("GTA4_PS5_DIR", os.path.join(ROOT, "ps5")))
APP0 = {"/app0/hub/": os.path.join(PS5, "package", "hub"), "/app0/fonts/": os.path.join(PS5, "assets", "fonts")}

FILES = os.path.abspath(os.environ.get("GTA4_WEB_FILES", os.path.join(ROOT, "web", "files")))
HOST = {name: os.path.join(FILES, name) for name in ("install",)}
for folder in HOST.values():
    os.makedirs(folder, exist_ok=True)


def shown(path):
    home = os.path.expanduser("~")
    return ("~" + path[len(home):] if path.startswith(home + os.sep) else path) + "/"


def listing(name):
    """Every file in a host folder: "<size>\t<path>" lines (no hidden ones)."""
    root = HOST[name]
    lines = []
    for folder, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for f in sorted(files):
            if f.startswith("."):
                continue
            full = os.path.join(folder, f)
            if os.path.isfile(full):
                lines.append(f"{os.path.getsize(full)}\t{os.path.relpath(full, root)}")
    return "\n".join(lines) + "\n"


TYPES = {".wasm": "application/wasm", ".js": "text/javascript", ".html": "text/html"}
# Sent gzipped to a browser that takes it (the engine: 66 -> 14 MB, a lot over
# a home connection seen from elsewhere), compressed once per file version.
COMPRESSED = {".wasm", ".js", ".html", ".wgsl", ".json", ".txt"}
gzipped = {}
gzip_lock = threading.Lock()


def gzipped_copy(full, stat):
    key = (full, stat.st_mtime_ns, stat.st_size)
    with gzip_lock:
        if key not in gzipped:
            with open(full, "rb") as f:
                gzipped[key] = gzip.compress(f.read(), 6)
        return gzipped[key]


class Handler(http.server.BaseHTTPRequestHandler):
    # (keep-alive: each engine thread, shader and disc range doesn't open a connection of its own)
    protocol_version = "HTTP/1.1"

    def resolve(self):
        path = urllib.parse.unquote(self.path.split("?", 1)[0])
        for prefix, root in APP0.items():
            if path.startswith(prefix):
                full = os.path.abspath(os.path.join(root, path[len(prefix):]))
                return full if full.startswith(root + os.sep) and os.path.isfile(full) else None
        match = re.match(r"/host/file/(install)/(.+)$", path)
        if match:
            root = HOST[match.group(1)]
            full = os.path.abspath(os.path.join(root, match.group(2)))
            return full if full.startswith(root + os.sep) and os.path.isfile(full) else None
        if path == "/iso/gta4.iso":
            return ISO
        if path == "/iso/tu8":
            return TU8
        if path == "/iso/aes_key":
            return KEY
        if path == "/":
            path = "/index.html"
        full = os.path.abspath(os.path.join(SITE, path.lstrip("/")))
        return full if full.startswith(SITE + os.sep) and os.path.isfile(full) else None

    def common_headers(self, ctype, length, cache="no-store"):
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", cache)

    def serve(self, body):
        full = self.resolve()
        if not full:
            self.send_error(404)
            return
        stat = os.stat(full)
        size = stat.st_size
        ext = os.path.splitext(full)[1]
        ctype = TYPES.get(ext, "application/octet-stream")
        # The site's own files: kept by the browser, asked again each time
        # (unchanged: an empty 304), so each engine thread's script and a
        # second visit's engine don't come over the network again.
        site = full.startswith(SITE + os.sep)
        cache = "no-cache" if site else "no-store"
        etag = f'"{stat.st_mtime_ns:x}-{size:x}"'
        if site and self.headers.get("If-None-Match") == etag:
            self.send_response(304)
            self.send_header("ETag", etag)
            self.send_header("Cache-Control", cache)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if (body and site and ext in COMPRESSED and not self.headers.get("Range")
                and "gzip" in self.headers.get("Accept-Encoding", "")):
            data = gzipped_copy(full, stat)
            self.send_response(200)
            self.common_headers(ctype, len(data), cache)
            self.send_header("Content-Encoding", "gzip")
            self.send_header("Vary", "Accept-Encoding")
            self.send_header("X-Uncompressed-Length", str(size))
            self.send_header("ETag", etag)
            self.end_headers()
            self.wfile.write(data)
            return
        start, end = 0, size - 1
        match = re.match(r"bytes=(\d*)-(\d*)$", self.headers.get("Range", ""))
        if match and (match.group(1) or match.group(2)):
            if match.group(1):
                start = int(match.group(1))
                end = int(match.group(2)) if match.group(2) else size - 1
            else:
                start = max(0, size - int(match.group(2)))
            end = min(end, size - 1)
            if start > end:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        else:
            self.send_response(200)
        self.common_headers(ctype, end - start + 1, cache)
        if site:
            self.send_header("ETag", etag)
        self.end_headers()
        if not body:
            return
        with open(full, "rb") as f:
            f.seek(start)
            remaining = end - start + 1
            while remaining > 0:
                chunk = f.read(min(1 << 20, remaining))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)

    def text(self, body):
        data = body.encode()
        self.send_response(200)
        self.common_headers("text/plain; charset=utf-8", len(data))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urllib.parse.unquote(self.path.split("?", 1)[0])
        if path == "/host/info":
            return self.text(f"install\t{shown(HOST['install'])}\n")
        if path == "/host/free":
            return self.text(f"{shutil.disk_usage(FILES).free}\n")
        match = re.match(r"/host/list/(install)$", path)
        if match:
            return self.text(listing(match.group(1)))
        try:
            self.serve(True)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        # The page uploads captures (audio recordings) here: saved beside the
        # site as uploads/<name>, names limited to [A-Za-z0-9._-]. (Not from
        # the Wi-Fi.)
        if LAN:
            self.send_error(403)
            return
        match = re.match(r"/upload/([A-Za-z0-9._-]{1,64})$", self.path)
        length = int(self.headers.get("Content-Length", "0"))
        if not match or length <= 0 or length > 64 << 20:
            self.send_error(400)
            return
        folder = os.path.join(SITE, "uploads")
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, match.group(1)), "wb") as f:
            remaining = length
            while remaining:
                chunk = self.rfile.read(min(1 << 20, remaining))
                if not chunk:
                    break
                f.write(chunk)
                remaining -= len(chunk)
        self.send_response(204)
        self.common_headers("text/plain", 0)
        self.end_headers()

    def do_HEAD(self):
        self.serve(False)

    def log_message(self, fmt, *args):
        sys.stderr.write("%s\n" % (fmt % args))


def lan_address():
    """This computer's address on the Wi-Fi (the one its default route uses)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("192.0.2.1", 9))  # (no packet is sent)
        return s.getsockname()[0]


SETUP_PAGE = """<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>GTA IV on this network</title>
<style>body{{font:17px -apple-system,system-ui,sans-serif;max-width:640px;margin:24px auto;padding:0 16px;
background:#111;color:#ddd}}a{{color:#f0b030}}li{{margin:8px 0}}code{{color:#fff;word-break:break-all}}</style>
<h2>Play GTA IV from {name}</h2>
<p>Once per iPad or phone:</p>
<ol>
<li><a href="/gta4-local-ca.crt">Download this computer's certificate</a> and allow it.</li>
<li>iPad / iPhone: Settings &rarr; General &rarr; VPN &amp; Device Management &rarr; install
<i>GTA IV Web local CA</i>.</li>
<li>Then Settings &rarr; General &rarr; About &rarr; Certificate Trust Settings &rarr; turn it on.</li>
</ol>
<p>Then: <a href="{check}">check this device</a> &middot; <a href="{game}">play</a></p>
<p>Certificate fingerprint (SHA-256): <code>{fingerprint}</code></p>
"""


def setup_server(port, ca_path, name, origin):
    """Plain HTTP on the Wi-Fi: the setup page and the authority's certificate, nothing else."""
    der = ssl.PEM_cert_to_DER_cert(open(ca_path).read())
    fingerprint = ":".join(f"{b:02X}" for b in hashlib.sha256(der).digest())
    page = SETUP_PAGE.format(name=name, check=origin + "/check.html", game=origin + "/",
                             fingerprint=fingerprint).encode()

    class Setup(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == "/gta4-local-ca.crt":
                body, ctype = der, "application/x-x509-ca-cert"
            elif self.path in ("/", "/index.html"):
                body, ctype = page, "text/html; charset=utf-8"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, fmt, *args):
            sys.stderr.write("setup: %s\n" % (fmt % args))

    http.server.ThreadingHTTPServer(("0.0.0.0", port), Setup).serve_forever()


if __name__ == "__main__":
    if not LAN:
        server = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
        print(f"serving {SITE} on http://127.0.0.1:{PORT} (iso: {ISO})", flush=True)
        server.serve_forever()
    ip = lan_address()
    certs = os.path.join(FILES, "lan-cert")
    subprocess.run(["bash", os.path.join(os.path.dirname(os.path.abspath(__file__)), "lan-cert.sh"), certs, ip],
                   check=True)
    name = subprocess.run(["scutil", "--get", "LocalHostName"], capture_output=True, text=True).stdout.strip()
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(os.path.join(certs, "server.crt"), os.path.join(certs, "server.key"))
    server = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    origin = f"https://{ip}:{PORT}"
    threading.Thread(target=setup_server, args=(PORT + 1, os.path.join(certs, "ca.crt"), name or ip, origin),
                     daemon=True).start()
    print(f"serving {SITE} on {origin} (iso: {ISO})", flush=True)
    print(f"on the iPad or phone, first open http://{ip}:{PORT + 1}/ to install the certificate", flush=True)
    server.serve_forever()
