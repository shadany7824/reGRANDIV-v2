/**
 * @file        core/memory_web.cpp
 * @brief       [web port] WebAssembly memory backend
 *
 * WebAssembly has one linear memory: no reservations at chosen addresses, no
 * page protection and no way to map pages twice. This backend therefore:
 *
 * - satisfies anonymous allocations (no base address) from the heap, aligned
 *   to the allocation granularity, and remembers them so they can be freed;
 * - treats a request at an address it already owns (inside a backing block or
 *   an earlier allocation) as a commit or decommit of memory that is already
 *   there: commit leaves it alone, decommit zeroes it;
 * - backs each file mapping with one heap block and returns views as
 *   pointers into it, wherever the caller asked for them
 *   (rex/memory/web_layout.h explains how the guest copes without aliases);
 * - accepts every protection change and reports read-write. Nothing faults,
 *   so write watches and guard pages are not enforced.
 *
 * Fresh linear memory is zero-filled and the browser commits pages as they
 * are touched, so a 4.5 GiB backing block costs only what the guest uses.
 *
 * @license     BSD 3-Clause License
 */

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include <unistd.h>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>

namespace rex {
namespace memory {

namespace {

constexpr size_t kWebPageSize = 0x1000;
constexpr size_t kWebAllocationGranularity = 0x10000;

struct Block {
  size_t length;
  bool owned;  // allocated here, freed on release
};

std::mutex blocks_mutex;
// Start address -> block, for allocations and file mapping backings.
std::map<uintptr_t, Block> blocks;

// Returns the block containing [address, address + length), if any.
const Block* FindBlock(uintptr_t address, size_t length, uintptr_t* out_start) {
  auto it = blocks.upper_bound(address);
  if (it == blocks.begin()) {
    return nullptr;
  }
  --it;
  if (address + length > it->first + it->second.length) {
    return nullptr;
  }
  *out_start = it->first;
  return &it->second;
}

void* AllocateBlock(size_t length) {
  length = (length + kWebAllocationGranularity - 1) & ~(kWebAllocationGranularity - 1);
  // Memory past the heap's end has never been handed out (Emscripten's
  // malloc never gives memory back, MORECORE_CANNOT_TRIM), so it is still
  // zero and not yet committed: clearing only the part below the old end
  // keeps a 4.5 GiB backing block from costing 4.5 GiB of RAM up front.
  const uintptr_t fresh = reinterpret_cast<uintptr_t>(sbrk(0));
  void* result = std::aligned_alloc(kWebAllocationGranularity, length);
  if (!result) {
    REXLOG_ERROR("web memory: could not allocate {:#x} bytes", length);
    return nullptr;
  }
  const uintptr_t start = reinterpret_cast<uintptr_t>(result);
  if (start < fresh) std::memset(result, 0, std::min<size_t>(length, fresh - start));
  std::lock_guard lock(blocks_mutex);
  blocks[reinterpret_cast<uintptr_t>(result)] = {length, true};
  return result;
}

}  // namespace

size_t page_size() {
  return kWebPageSize;
}

size_t allocation_granularity() {
  return kWebAllocationGranularity;
}

bool IsWritableExecutableMemorySupported() {
  // There is no executable memory to write: code lives outside linear memory.
  return false;
}

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  (void)allocation_type;
  (void)access;
  if (!base_address) {
    return AllocateBlock(length);
  }
  std::lock_guard lock(blocks_mutex);
  uintptr_t start;
  if (!FindBlock(reinterpret_cast<uintptr_t>(base_address), length, &start)) {
    // Linear memory cannot be reserved at an address of the caller's choice.
    return nullptr;
  }
  return base_address;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  auto address = reinterpret_cast<uintptr_t>(base_address);
  std::lock_guard lock(blocks_mutex);
  uintptr_t start;
  const Block* block = FindBlock(address, length ? length : 1, &start);
  if (!block) {
    return false;
  }
  if (deallocation_type == DeallocationType::kDecommit) {
    std::memset(base_address, 0, length);
    return true;
  }
  if (start != address || !block->owned) {
    // Releasing part of a block (or of a mapping's backing): just clear it.
    std::memset(base_address, 0, length);
    return true;
  }
  blocks.erase(start);
  std::free(base_address);
  return true;
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  (void)base_address;
  (void)length;
  (void)access;
  if (out_old_access) {
    *out_old_access = PageAccess::kReadWrite;
  }
  return true;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  (void)base_address;
  (void)length;
  access_out = PageAccess::kReadWrite;
  return true;
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
  (void)path;
  (void)access;
  (void)commit;
  void* backing = AllocateBlock(length);
  if (!backing) {
    return kFileMappingHandleInvalid;
  }
  return reinterpret_cast<FileMappingHandle>(backing);
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  (void)path;
  if (handle != kFileMappingHandleInvalid) {
    DeallocFixed(reinterpret_cast<void*>(handle), 0, DeallocationType::kRelease);
  }
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  // Views are pointers into the backing; a requested address is ignored, as
  // nothing can be mapped there.
  (void)base_address;
  (void)access;
  if (handle == kFileMappingHandleInvalid) {
    return nullptr;
  }
  auto backing = reinterpret_cast<uint8_t*>(handle);
  std::lock_guard lock(blocks_mutex);
  uintptr_t start;
  if (!FindBlock(reinterpret_cast<uintptr_t>(backing + file_offset), length ? length : 1, &start)) {
    return nullptr;
  }
  return backing + file_offset;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  // The backing stays until the mapping handle is closed.
  (void)handle;
  (void)base_address;
  (void)length;
  return true;
}

}  // namespace memory
}  // namespace rex
