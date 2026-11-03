// [web port] The Android build's stale-object guards (android/
// android_stale_object_guard.cpp, the same code with the port's logging),
// plus one more site the browser build hit: the title frees objects on one
// thread while another still holds them, and the faster renderer exposes
// races the console's timing hid. The generated code calls
// GTA4_StaleChildGuard (tools/apply_generated_patches.py).
// sub_8296E480(owner r3, out r4) copies the world matrix of the object at
// owner+56 into out, following object->[4]->[12] to a type check and a
// skeleton query; with no object it writes an identity matrix instead. The
// title frees such objects on another thread while this one still holds the
// owner, a race the console's timing hid: the faster renderer exposes it as
// a crash on an unmapped page inside this function. When the object's
// pointer chain no longer leads to mapped memory, take the title's own
// no-object path rather than fault.

#include <atomic>
#include <cstdio>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include "gta4_init.h"

REXCVAR_DEFINE_BOOL(gta4_stale_object_guard, true, "GTA IV/Stability",
                    "Treat an object whose pointers lead to unmapped memory as absent in "
                    "sub_8296E480, sub_8296E310, sub_8297CE78 and sub_82477E30 instead of crashing, "
                    "and reset sub_8296E638's entry count when it is garbage and skip sub_8294EC18's unlink of an "
                    "entry its chain doesn't hold, instead of looping");

namespace {

std::atomic<uint64_t> g_stale_objects{0};

template <class... A>
void StaleLog(const char* format, A... args) {
  char text[256];
  std::snprintf(text, sizeof(text), format, args...);
  REXLOG_WARN("{}", text);
}

// Recently confirmed readable 4 KB pages, valid while their heap's access
// epoch is unchanged (it reads 0 during a mutation and moves after it).
struct ReadablePage {
  const void* heap = nullptr;
  uint64_t epoch = 0;
  uint32_t page = UINT32_MAX;
};
thread_local ReadablePage t_readable_pages[16];
thread_local uint32_t t_readable_cursor = 0;

bool PageReadable(rex::memory::Memory* memory, uint32_t address) {
  auto* heap = memory->LookupHeap(address);
  if (!heap) return false;
  const uint64_t epoch = heap->access_epoch();
  const uint32_t page = address >> 12;
  for (const ReadablePage& cached : t_readable_pages) {
    if (cached.page == page && cached.heap == heap && cached.epoch == epoch && epoch) return true;
  }
  const uint32_t first = page << 12;
  using rex::memory::PageAccess;
  const auto access = heap->QueryRangeAccess(first, first + 0xFFF);
  const bool readable = access == PageAccess::kReadOnly || access == PageAccess::kReadWrite ||
                        access == PageAccess::kExecuteReadOnly ||
                        access == PageAccess::kExecuteReadWrite;
  if (readable && epoch) {
    t_readable_pages[t_readable_cursor++ % 16] = {heap, epoch, page};
  }
  return readable;
}

bool Readable(uint32_t address, uint32_t bytes) {
  if (!address || !bytes || uint64_t(address) + bytes > uint64_t(UINT32_MAX) + 1) return false;
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) return false;
  const uint32_t end = uint32_t(uint64_t(address) + bytes - 1);
  if (!PageReadable(memory, address)) return false;
  return (end >> 12) == (address >> 12) || PageReadable(memory, end);
}

}  // namespace

// Both copy routines share one layout: owner+object_offset -> object (matrix
// at +16, link at +4), link+12 -> typed object (type byte at +4), owner+
// table_offset read for the skeleton query. Returns false (after writing the
// title's no-object identity) when that chain no longer leads to mapped
// memory; true when the original should run.
static bool GuardedMatrixCopy(PPCContext& ctx, uint8_t* base, uint32_t object_offset,
                       uint32_t table_offset, const char* name) {
  if (!REXCVAR_GET(gta4_stale_object_guard)) return true;
  const uint32_t owner = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  bool valid = Readable(owner + object_offset, 4) && Readable(owner + table_offset, 4) &&
               Readable(out, 64);
  const uint32_t object = valid ? REX_LOAD_U32(owner + object_offset) : 0;
  if (valid && object) {
    valid = Readable(object, 80);
    const uint32_t link = valid ? REX_LOAD_U32(object + 4) : 0;
    valid = valid && Readable(link + 12, 4);
    const uint32_t typed = valid ? REX_LOAD_U32(link + 12) : 0;
    valid = valid && Readable(typed + 4, 1);
  }
  if (valid) return true;
  const uint64_t count = g_stale_objects.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count <= 8 || !(count % 1024)) {
    StaleLog("stale-object-guard: #%llu %s owner=%08X object=%08X out=%08X",
                        static_cast<unsigned long long>(count), name, owner, object, out);
  }
  if (Readable(out, 64)) {
    // The title's no-object path: identity rotation, zero translation, with
    // its 1.0/0.0 constants read from the same addresses (lis -32256).
    const uint32_t one = REX_LOAD_U32(0x82000000u + 3400);
    const uint32_t zero = REX_LOAD_U32(0x82000000u + 2612);
    const uint32_t layout[9][2] = {{0, one},  {4, zero},  {8, zero},  {16, zero}, {20, one},
                                   {24, zero}, {32, zero}, {36, zero}, {40, one}};
    for (const auto& [offset, bits] : layout) REX_STORE_U32(out + offset, bits);
    for (uint32_t offset = 48; offset < 64; offset += 4) REX_STORE_U32(out + offset, 0);
  }
  return false;
}

// sub_8297CE78 walks an object's child table (object+48+648+i*4) and reads the
// child's words at +100 and +108 straight away: the same freed-object race, a
// crash with the game standing still (2026-10-09). The edit that
// tools/apply_generated_patches.py makes in the generated code asks here and,
// for a child that no longer leads to mapped memory, takes the title's
// empty-table path.
extern "C" bool GTA4_StaleChildGuard(uint32_t child) {
  if (!REXCVAR_GET(gta4_stale_object_guard) || Readable(child + 100, 12)) return true;
  const uint64_t count = g_stale_objects.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count <= 8 || !(count % 1024)) {
    StaleLog("stale-object-guard: #%llu sub_8297CE78 child=%08X",
                        static_cast<unsigned long long>(count), child);
  }
  return false;
}

// sub_8296E638 (an object's physics update) first walks its entry table: the
// count at +1184 of 176-byte entries held in the object itself (+128 up to
// +1184: six at most), then works through them. A freed object's garbage
// count made it loop for ever - the game frozen, one thread at 100% - after a
// car landed on a rooftop (2026-11-03). A count no table that size can hold
// takes the title's own reset (loc_8296E7C0: count 0, +1188 -1) before the
// call, as for a fresh object.
extern "C" void sub_8296E638(PPCContext& ctx, uint8_t* base) {
  const uint32_t object = ctx.r3.u32;
  if (REXCVAR_GET(gta4_stale_object_guard) && Readable(object + 1184, 8)) {
    const uint32_t count = REX_LOAD_U32(object + 1184);
    if (count > 16) {
      const uint64_t n = g_stale_objects.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 8 || !(n % 1024)) {
        StaleLog("stale-object-guard: #%llu sub_8296E638 object=%08X count=%08X", static_cast<unsigned long long>(n),
                 object, count);
      }
      REX_STORE_U32(object + 1184, 0);
      REX_STORE_U32(object + 1188, 0xFFFFFFFFu);
    }
  }
  __imp__sub_8296E638(ctx, base);
}

// sub_8294EC18(pool r3, bucket r4, entry r5) unlinks entry r5 from a bucket's
// chain: it follows the 16-bit links (the first at pool->[140] + bucket*64 +
// 26, then table pool->[112] + link*8) until it meets r5, with no end test.
// A freed object's entry is in no chain, and the walk then circles for ever:
// the game frozen, one thread at 100%, after a car landed on a rooftop
// (2026-11-03). The chain is walked first, bounded; an entry it doesn't reach
// is left alone (nothing to unlink).
extern "C" void sub_8294EC18(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(gta4_stale_object_guard)) {
    const uint32_t pool = ctx.r3.u32, bucket = ctx.r4.u32, entry = ctx.r5.u32 & 0xFFFF;
    bool found = false;
    if (Readable(pool + 112, 4) && Readable(pool + 140, 4)) {
      const uint32_t table = REX_LOAD_U32(pool + 112);
      uint32_t link = REX_LOAD_U32(pool + 140) + (bucket << 6) + 26;
      // (a chain holds at most the pool's 65536 entries)
      for (uint32_t steps = 0; steps <= 0x10000 && Readable(link, 2); steps++) {
        const uint32_t next = REX_LOAD_U16(link);
        if (next == entry) { found = true; break; }
        link = table + (next << 3);
      }
    }
    if (!found) {
      const uint64_t n = g_stale_objects.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 8 || !(n % 1024))
        StaleLog("stale-object-guard: #%llu sub_8294EC18 pool=%08X bucket=%u entry=%04X not in its chain",
                 static_cast<unsigned long long>(n), pool, bucket, entry);
      return;
    }
  }
  __imp__sub_8294EC18(ctx, base);
}

extern "C" void sub_8296E480(PPCContext& ctx, uint8_t* base) {
  if (GuardedMatrixCopy(ctx, base, 56, 112, "sub_8296E480")) __imp__sub_8296E480(ctx, base);
}

// Same routine for the object at owner+52 (table at owner+104).
extern "C" void sub_8296E310(PPCContext& ctx, uint8_t* base) {
  if (GuardedMatrixCopy(ctx, base, 52, 104, "sub_8296E310")) __imp__sub_8296E310(ctx, base);
}

// sub_82477EF0 walks the title's global entity list every frame and calls
// sub_82477E30 for each entity's children (entity+652+i*4), the table
// sub_8297CE78 reads. A collision left small integers in it (child=00000B32,
// then 00000002, 2026-10-10): sub_8297CE78's guard took its empty path, and
// sub_82477E30 then faulted reading the child's +52. A child that does not
// lead to mapped memory is skipped; the next frame walks the table again.
extern "C" void sub_82477E30(PPCContext& ctx, uint8_t* base) {
  const uint32_t child = ctx.r3.u32;
  if (!REXCVAR_GET(gta4_stale_object_guard) || Readable(child + 52, 8)) {
    __imp__sub_82477E30(ctx, base);
    return;
  }
  const uint64_t count = g_stale_objects.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count <= 8 || !(count % 1024)) {
    StaleLog("stale-object-guard: #%llu sub_82477E30 child=%08X",
                        static_cast<unsigned long long>(count), child);
  }
}

// sub_829530F8 calls the virtual at +96 of the object at r3+4, skipping the
// call when that is null. In the browser it once held 0xDC000000, a freed
// object whose table led to 0x191CFFFF (2026-10-28, while driving): an object
// whose function at +96 isn't the title's code is treated as absent, the
// title's own null path.
extern "C" void sub_829530F8(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(gta4_stale_object_guard)) {
    const uint32_t holder = ctx.r3.u32;
    bool valid = Readable(holder + 4, 4);
    const uint32_t object = valid ? REX_LOAD_U32(holder + 4) : 0;
    if (valid && object) {
      valid = Readable(object, 4);
      const uint32_t table = valid ? REX_LOAD_U32(object) : 0;
      valid = valid && Readable(table + 96, 4);
      const uint32_t target = valid ? REX_LOAD_U32(table + 96) : 0;
      // (the title's code: its own dispatch table, lock-free, as
      // REX_CALL_INDIRECT_FUNC looks it up; anything else, the dispatcher)
      if (valid) {
        const bool in_title = uint32_t(target - REX_CODE_BASE) < REX_CODE_SIZE + REX_THUNK_RESERVE_SIZE;
        if (!(in_title && REX_LOOKUP_FUNC(base, target))) {
          auto* runtime = rex::Runtime::instance();
          valid = runtime && runtime->function_dispatcher() &&
                  runtime->function_dispatcher()->GetFunction(target) != nullptr;
        }
      }
      if (!valid) {
        const uint64_t count = g_stale_objects.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 8 || !(count % 1024))
          StaleLog("stale-object-guard: #%llu sub_829530F8 object=%08X", static_cast<unsigned long long>(count), object);
        ctx.r3.u64 = 0;  // (as the title's null path leaves it)
        return;
      }
    }
  }
  __imp__sub_829530F8(ctx, base);
}
