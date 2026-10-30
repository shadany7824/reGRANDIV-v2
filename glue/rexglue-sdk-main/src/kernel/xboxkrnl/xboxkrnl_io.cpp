/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <mutex>
#include <unordered_map>
#include <rex/filesystem/device.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/info/file.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/system/xiocompletion.h>
#include <rex/system/xsymboliclink.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/thread/mutex.h>

#include <new>
#include <atomic>
#include <chrono>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace {
// TEMP: streaming read telemetry (perf investigation).
struct ReadTelemetry {
  std::atomic<uint64_t> reads{0}, async_reads{0}, bytes{0}, service_ns{0};
  std::atomic<int64_t> in_flight{0};
  std::atomic<int64_t> max_in_flight{0};
  std::atomic<int64_t> last_report_ns{0};
};
ReadTelemetry g_read_telemetry;
int64_t TelemetryNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}
void TelemetryBegin() {
  const int64_t now_in_flight = g_read_telemetry.in_flight.fetch_add(1) + 1;
  int64_t seen = g_read_telemetry.max_in_flight.load();
  while (now_in_flight > seen && !g_read_telemetry.max_in_flight.compare_exchange_weak(seen, now_in_flight)) {}
}
void TelemetryEnd(int64_t begin_ns, uint32_t bytes, bool async) {
  auto& t = g_read_telemetry;
  t.in_flight.fetch_sub(1);
  t.reads.fetch_add(1);
  if (async) t.async_reads.fetch_add(1);
  t.bytes.fetch_add(bytes);
  const int64_t now = TelemetryNowNs();
  t.service_ns.fetch_add(uint64_t(now - begin_ns));
  int64_t last = t.last_report_ns.load();
  if (now - last > 2000000000ll && t.last_report_ns.compare_exchange_strong(last, now)) {
    const uint64_t reads = t.reads.exchange(0), async_reads = t.async_reads.exchange(0);
    const uint64_t total = t.bytes.exchange(0), service = t.service_ns.exchange(0);
    const int64_t peak = t.max_in_flight.exchange(0);
#if defined(__ANDROID__)
    const double seconds = last ? double(now - last) / 1e9 : 2.0;
    __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
                        "io: %.0f reads/s (%llu async) %.1f MB/s avg %.0f KB service %.2f ms peak-in-flight %lld",
                        reads / seconds, (unsigned long long)async_reads, total / 1e6 / seconds,
                        reads ? total / 1024.0 / reads : 0.0, reads ? service / 1e6 / reads : 0.0,
                        (long long)peak);
#elif REX_PLATFORM_WEB
    const double seconds = last ? double(now - last) / 1e9 : 2.0;
    REXKRNL_INFO("web io: {:.0f} reads/s ({} async) {:.1f} MB/s avg {:.0f} KB service {:.2f} ms peak-in-flight {}",
                 reads / seconds, async_reads, total / 1e6 / seconds, reads ? total / 1024.0 / reads : 0.0,
                 reads ? service / 1e6 / reads : 0.0, peak);
#endif
  }
}
}  // namespace

namespace rex::kernel::xboxkrnl {
using namespace rex::system;

struct CreateOptions {
  // https://processhacker.sourceforge.io/doc/ntioapi_8h.html
  static const uint32_t FILE_DIRECTORY_FILE = 0x00000001;
  // Optimization - files access will be sequential, not random.
  static const uint32_t FILE_SEQUENTIAL_ONLY = 0x00000004;
  static const uint32_t FILE_SYNCHRONOUS_IO_ALERT = 0x00000010;
  static const uint32_t FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020;
  static const uint32_t FILE_NON_DIRECTORY_FILE = 0x00000040;
  // Optimization - file access will be random, not sequential.
  static const uint32_t FILE_RANDOM_ACCESS = 0x00000800;
};

static bool IsValidPath(const std::string_view s, bool is_pattern) {
  // TODO(gibbed): validate path components individually
  bool got_asterisk = false;
  for (const auto& c : s) {
    if (c <= 31 || c >= 127) {
      return false;
    }
    if (got_asterisk) {
      // * must be followed by a . (*.)
      //
      // 4D530819 has a bug in its game code where it attempts to
      // FindFirstFile() with filters of "Game:\\*_X3.rkv", "Game:\\m*_X3.rkv",
      // and "Game:\\w*_X3.rkv" and will infinite loop if the path filter is
      // allowed.
      if (c != '.') {
        return false;
      }
      got_asterisk = false;
    }
    switch (c) {
      case '"':
      // case '*':
      case '+':
      case ',':
      // case ':':
      // case ';':
      case '<':
      // case '=':
      case '>':
      // case '?':
      case '|': {
        return false;
      }
      case '*': {
        // Pattern-specific (for NtQueryDirectoryFile)
        if (!is_pattern) {
          return false;
        }
        got_asterisk = true;
        break;
      }
      case '?': {
        // Pattern-specific (for NtQueryDirectoryFile)
        if (!is_pattern) {
          return false;
        }
        break;
      }
      default: {
        break;
      }
    }
  }
  return true;
}

struct FileReadCompletion {
  object_ref<XFile> file;
  object_ref<XEvent> supplied_event;
  object_ref<XThread> requesting_thread;
  uint32_t io_status_block = 0;
  uint32_t apc_routine = 0;
  uint32_t apc_context = 0;

  void PrepareWaitObject() const {
    // A supplied per-request event replaces FileHandle as the wait object.
    // Resetting both makes overlapping requests destroy unrelated file-object
    // completion signals.
    if (file_io::SelectReadWaitObject(!!supplied_event) ==
        file_io::ReadWaitObject::kSuppliedEvent) {
      supplied_event->Reset();
    } else {
      file->ResetWaitEvent();
    }
  }

  void Complete(X_STATUS status, uint32_t bytes_transferred) const {
    // Completion ordering is observable by the guest. The transfer has already
    // made data visible before this function is entered.
    if (io_status_block) {
      auto* iosb = file->memory()->TranslateVirtual<X_IO_STATUS_BLOCK*>(io_status_block);
      iosb->status = status;
      iosb->information = bytes_transferred;
    }
    file->NotifyIOCompletion(status, bytes_transferred, apc_context);
    if (apc_routine && requesting_thread) {
      // A null APC context is valid. Accepted requests deliver their APC for
      // success, EOF and transfer errors alike. Publish the APC before waking
      // an alertable wait so the event can't win and strand the APC until an
      // unrelated later alertable wait.
      requesting_thread->EnqueueApc(apc_routine, apc_context, io_status_block, 0);
    }
    if (file_io::SelectReadWaitObject(!!supplied_event) ==
        file_io::ReadWaitObject::kSuppliedEvent) {
      supplied_event->Set(0, false);
    } else {
      file->SignalWaitEvent();
    }
    REXKRNL_DEBUG("[AsyncIO] complete iosb={:08X} status={:08X} bytes={} wait={} apc={:08X}",
                  io_status_block, status, bytes_transferred, supplied_event ? "event" : "file",
                  apc_routine);
  }
};

static X_STATUS ValidateReadRequest(const object_ref<XFile>& file, uint32_t io_status_block_address,
                                    uint32_t buffer_guest_address, uint32_t buffer_length) {
  if (io_status_block_address &&
      !file->ValidateGuestRange(io_status_block_address, sizeof(X_IO_STATUS_BLOCK), true)) {
    return X_STATUS_ACCESS_VIOLATION;
  }
  if (!file->ValidateGuestRange(buffer_guest_address, buffer_length, true)) {
    return X_STATUS_ACCESS_VIOLATION;
  }
  return X_STATUS_SUCCESS;
}

static void StoreImmediateIoStatus(const object_ref<XFile>& file, uint32_t io_status_block_address,
                                   X_STATUS status, uint32_t information) {
  if (!io_status_block_address ||
      !file->ValidateGuestRange(io_status_block_address, sizeof(X_IO_STATUS_BLOCK), true)) {
    return;
  }
  auto* iosb = file->memory()->TranslateVirtual<X_IO_STATUS_BLOCK*>(io_status_block_address);
  iosb->status = status;
  iosb->information = information;
}

u32 NtCreateFile_entry(mapped_u32 handle_out, u32 desired_access,
                       ppc_ptr_t<X_OBJECT_ATTRIBUTES> object_attrs,
                       ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, mapped_u64 allocation_size_ptr,
                       u32 file_attributes, u32 share_access, u32 creation_disposition,
                       u32 create_options) {
  // note used. maybe later
  // uint64_t allocation_size = 0;  // is this correct???
  // if (allocation_size_ptr) {
  //  allocation_size = *allocation_size_ptr;
  //}

  if (!object_attrs) {
    // ..? Some games do this. This parameter is not optional.
    return X_STATUS_INVALID_PARAMETER;
  }
  assert_not_null(handle_out);

  auto object_name = REX_KERNEL_MEMORY()->TranslateVirtual<X_ANSI_STRING*>(object_attrs->name_ptr);

  rex::filesystem::Entry* root_entry = nullptr;

  // Compute path, possibly attrs relative.
  auto target_path = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);
  REXKRNL_IMPORT_TRACE(
      "NtCreateFile", "path={} access={:#x} attrs={:#x} share={:#x} disp={:#x} options={:#x}",
      target_path, (uint32_t)desired_access, (uint32_t)file_attributes, (uint32_t)share_access,
      (uint32_t)creation_disposition, (uint32_t)create_options);

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  if (object_attrs->root_directory != 0xFFFFFFFD &&  // ObDosDevices
      object_attrs->root_directory != 0) {
    auto root_file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(object_attrs->root_directory);
    assert_not_null(root_file);
    assert_true(root_file->type() == XObject::Type::File);

    root_entry = root_file->entry();
  }

  // Attempt open (or create).
  rex::filesystem::File* vfs_file;
  rex::filesystem::FileAction file_action;
  X_STATUS result = REX_KERNEL_FS()->OpenFile(
      root_entry, target_path, rex::filesystem::FileDisposition((uint32_t)creation_disposition),
      desired_access, (create_options & CreateOptions::FILE_DIRECTORY_FILE) != 0,
      (create_options & CreateOptions::FILE_NON_DIRECTORY_FILE) != 0, &vfs_file, &file_action);
  object_ref<XFile> file = nullptr;

  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  if (XSUCCEEDED(result)) {
    // If true, desired_access SYNCHRONIZE flag must be set.
    bool synchronous = (create_options & CreateOptions::FILE_SYNCHRONOUS_IO_ALERT) ||
                       (create_options & CreateOptions::FILE_SYNCHRONOUS_IO_NONALERT);
    file = object_ref<XFile>(new XFile(REX_KERNEL_STATE(), vfs_file, synchronous));

    // Handle ref is incremented, so return that.
    handle = file->handle();
  }

  if (io_status_block) {
    io_status_block->status = result;
    io_status_block->information = (uint32_t)file_action;
  }

  *handle_out = handle;
  if (XFAILED(result)) {
    REXKRNL_IMPORT_FAIL("NtCreateFile", "path='{}' -> {:#x}", target_path, result);
  } else {
    REXKRNL_IMPORT_RESULT("NtCreateFile", "{:#x} handle={:#x}", result, handle);
  }
  return result;
}

u32 NtOpenFile_entry(mapped_u32 handle_out, u32 desired_access,
                     ppc_ptr_t<X_OBJECT_ATTRIBUTES> object_attributes,
                     ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, u32 open_options) {
  return NtCreateFile_entry(handle_out, desired_access, object_attributes, io_status_block, nullptr,
                            0, 0, static_cast<uint32_t>(rex::filesystem::FileDisposition::kOpen),
                            open_options);
}

#if REX_PLATFORM_WEB
// [web port] Reads of streamed media (audio\sfx archives, movies): the first
// few per file with their result, then every 500th.
static void WebMediaReadTrace(XFile* file, uint64_t offset, uint32_t length, X_STATUS status,
                              uint32_t bytes, bool async, uint32_t event = 0, uint32_t apc = 0) {
  const std::string& path = file->path();
  if (path.find("sfx") == std::string::npos && path.find("movies") == std::string::npos &&
      path.find(".bik") == std::string::npos) {
    return;
  }
  static std::mutex mutex;
  static std::unordered_map<std::string, uint32_t> counts;
  std::lock_guard lock(mutex);
  const uint32_t n = ++counts[path];
  if (n <= 3 || n % 500 == 0 || XFAILED(status) || bytes < length) {
    REXKRNL_INFO("web media read #{} {} offset {} len {} -> {:#x} bytes {}{} event {:08X} apc {:08X} sync {} port {}",
                 n, path, offset, length, uint32_t(status), bytes, async ? " (async)" : "", event, apc,
                 file->is_synchronous(), file->HasIOCompletionPorts());
  }
}
#endif

u32 NtReadFile_entry(u32 file_handle, u32 event_handle, mapped_void apc_routine_ptr,
                     mapped_void apc_context, ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                     mapped_void buffer, u32 buffer_length, mapped_u64 byte_offset_ptr) {
  const uint64_t byte_offset =
      byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : file_io::kUseCurrentPosition;
  const uint32_t apc_routine = static_cast<uint32_t>(apc_routine_ptr) & ~1u;
  REXKRNL_IMPORT_TRACE(
      "NtReadFile",
      "handle={:#x} event={:#x} apc={:#x} apc_ctx={:#x} iosb={:#x} buf={:#x} len={:#x} offset={}",
      (uint32_t)file_handle, (uint32_t)event_handle, apc_routine_ptr.guest_address(),
      apc_context.guest_address(), io_status_block.guest_address(), buffer.guest_address(),
      (uint32_t)buffer_length, byte_offset_ptr ? (int64_t)byte_offset : -1);

  auto file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(file_handle);
  if (!file) {
    if (io_status_block) {
      io_status_block->status = X_STATUS_INVALID_HANDLE;
      io_status_block->information = 0;
    }
    return X_STATUS_INVALID_HANDLE;
  }
  auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
  if (event_handle && !ev) {
    StoreImmediateIoStatus(file, io_status_block.guest_address(), X_STATUS_INVALID_HANDLE, 0);
    return X_STATUS_INVALID_HANDLE;
  }

  X_STATUS result = ValidateReadRequest(file, io_status_block.guest_address(),
                                        buffer.guest_address(), buffer_length);
  if (XFAILED(result)) {
    StoreImmediateIoStatus(file, io_status_block.guest_address(), result, 0);
    return result;
  }

  XThread* current_thread = XThread::GetCurrentThread();
  FileReadCompletion completion{file,
                                ev,
                                retain_object(current_thread),
                                io_status_block.guest_address(),
                                apc_routine,
                                apc_context.guest_address()};

  const bool has_completion_port = file->HasIOCompletionPorts();
  const bool inline_completion = file_io::IsInlineCompletionEligible(
      file->is_synchronous(), !!ev, apc_routine != 0, has_completion_port);
  const int64_t telemetry_begin = TelemetryNowNs();
  TelemetryBegin();
  if (inline_completion) {
    completion.PrepareWaitObject();
    uint32_t bytes_read = 0;
    result = file->ReadTransfer(buffer.guest_address(), buffer_length, byte_offset, &bytes_read);
#if REX_PLATFORM_WEB
    WebMediaReadTrace(file.get(), byte_offset, buffer_length, result, bytes_read, false,
                      uint32_t(event_handle), apc_routine);
#endif
    TelemetryEnd(telemetry_begin, bytes_read, false);
    completion.Complete(result, bytes_read);
    REXKRNL_IMPORT_RESULT(
        "NtReadFile", "{:#x} inline=1 synchronous={} completion_port={} wait={} transferred={}",
        result, file->is_synchronous(), has_completion_port, ev ? "event" : "file", bytes_read);
    return result;
  }

  HostTaskAdmissionResult admission = HostTaskAdmissionResult::kNoMemory;
  try {
    admission = REX_KERNEL_STATE()->QueueHostIoTask(
        reinterpret_cast<uintptr_t>(file.get()),
        [completion, buffer_address = buffer.guest_address(),
         length = static_cast<uint32_t>(buffer_length), byte_offset, telemetry_begin]() {
          REXKRNL_DEBUG("[AsyncIO] worker start iosb={:08X} kind=read", completion.io_status_block);
          uint32_t bytes_read = 0;
          const X_STATUS status =
              completion.file->ReadTransfer(buffer_address, length, byte_offset, &bytes_read);
#if REX_PLATFORM_WEB
          WebMediaReadTrace(completion.file.get(), byte_offset, length, status, bytes_read, true);
#endif
          TelemetryEnd(telemetry_begin, bytes_read, true);
          completion.Complete(status, bytes_read);
        },
        [completion]() {
          completion.PrepareWaitObject();
          StoreImmediateIoStatus(completion.file, completion.io_status_block, X_STATUS_PENDING, 0);
          REXKRNL_DEBUG("[AsyncIO] admitted iosb={:08X} wait={} apc={:08X}",
                        completion.io_status_block, completion.supplied_event ? "event" : "file",
                        completion.apc_routine);
        });
  } catch (const std::bad_alloc&) {
    admission = HostTaskAdmissionResult::kNoMemory;
  }
  if (admission != HostTaskAdmissionResult::kAccepted) {
    const X_STATUS failure = admission == HostTaskAdmissionResult::kNoMemory
                                 ? X_STATUS_NO_MEMORY
                                 : X_STATUS_THREAD_IS_TERMINATING;
    StoreImmediateIoStatus(file, io_status_block.guest_address(), failure, 0);
    REXKRNL_IMPORT_RESULT("NtReadFile", "{:#x} async admission rejected", failure);
    return failure;
  }

  REXKRNL_IMPORT_RESULT("NtReadFile", "{:#x} inline=0 wait={} completion_port={}", X_STATUS_PENDING,
                        ev ? "event" : "file", has_completion_port);
  return X_STATUS_PENDING;
}

u32 NtReadFileScatter_entry(u32 file_handle, u32 event_handle, mapped_void apc_routine_ptr,
                            mapped_void apc_context, ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                            mapped_u32 segment_array, u32 length, mapped_u64 byte_offset_ptr) {
  auto file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(file_handle);
  if (!file) {
    if (io_status_block) {
      io_status_block->status = X_STATUS_INVALID_HANDLE;
      io_status_block->information = 0;
    }
    return X_STATUS_INVALID_HANDLE;
  }
  auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
  if (event_handle && !ev) {
    StoreImmediateIoStatus(file, io_status_block.guest_address(), X_STATUS_INVALID_HANDLE, 0);
    return X_STATUS_INVALID_HANDLE;
  }

  if (io_status_block.guest_address() &&
      !file->ValidateGuestRange(io_status_block.guest_address(), sizeof(X_IO_STATUS_BLOCK), true)) {
    return X_STATUS_ACCESS_VIOLATION;
  }

  std::vector<uint32_t> segments;
  X_STATUS result = file->SnapshotReadScatter(segment_array.guest_address(), length, &segments);
  if (XFAILED(result)) {
    StoreImmediateIoStatus(file, io_status_block.guest_address(), result, 0);
    return result;
  }

  const uint64_t byte_offset =
      byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : file_io::kUseCurrentPosition;
  const uint32_t apc_routine = static_cast<uint32_t>(apc_routine_ptr) & ~1u;
  FileReadCompletion completion{file,
                                ev,
                                retain_object(XThread::GetCurrentThread()),
                                io_status_block.guest_address(),
                                apc_routine,
                                apc_context.guest_address()};

  const bool has_completion_port = file->HasIOCompletionPorts();
  const bool inline_completion = file_io::IsInlineCompletionEligible(
      file->is_synchronous(), !!ev, apc_routine != 0, has_completion_port);
  if (inline_completion) {
    completion.PrepareWaitObject();
    uint32_t bytes_read = 0;
    result = file->ReadScatterTransfer(segments, length, byte_offset, &bytes_read);
    completion.Complete(result, bytes_read);
    REXKRNL_IMPORT_RESULT("NtReadFileScatter",
                          "{:#x} inline=1 synchronous={} completion_port={} wait={} transferred={}",
                          result, file->is_synchronous(), has_completion_port,
                          ev ? "event" : "file", bytes_read);
    return result;
  }

  HostTaskAdmissionResult admission = HostTaskAdmissionResult::kNoMemory;
  try {
    admission = REX_KERNEL_STATE()->QueueHostIoTask(
        reinterpret_cast<uintptr_t>(file.get()),
        [completion, segments = std::move(segments), length = static_cast<uint32_t>(length),
         byte_offset]() {
          REXKRNL_DEBUG("[AsyncIO] worker start iosb={:08X} kind=scatter",
                        completion.io_status_block);
          uint32_t bytes_read = 0;
          const X_STATUS status =
              completion.file->ReadScatterTransfer(segments, length, byte_offset, &bytes_read);
          completion.Complete(status, bytes_read);
        },
        [completion]() {
          completion.PrepareWaitObject();
          StoreImmediateIoStatus(completion.file, completion.io_status_block, X_STATUS_PENDING, 0);
          REXKRNL_DEBUG("[AsyncIO] admitted iosb={:08X} wait={} apc={:08X} kind=scatter",
                        completion.io_status_block, completion.supplied_event ? "event" : "file",
                        completion.apc_routine);
        });
  } catch (const std::bad_alloc&) {
    admission = HostTaskAdmissionResult::kNoMemory;
  }
  if (admission != HostTaskAdmissionResult::kAccepted) {
    const X_STATUS failure = admission == HostTaskAdmissionResult::kNoMemory
                                 ? X_STATUS_NO_MEMORY
                                 : X_STATUS_THREAD_IS_TERMINATING;
    StoreImmediateIoStatus(file, io_status_block.guest_address(), failure, 0);
    return failure;
  }

  REXKRNL_IMPORT_RESULT("NtReadFileScatter", "{:#x} inline=0 wait={} completion_port={}",
                        X_STATUS_PENDING, ev ? "event" : "file", has_completion_port);
  return X_STATUS_PENDING;
}

u32 NtWriteFile_entry(u32 file_handle, u32 event_handle, u32 apc_routine, mapped_void apc_context,
                      ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, mapped_void buffer,
                      u32 buffer_length, mapped_u64 byte_offset_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  // Grab event to signal.
  bool signal_event = false;
  auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  // Grab file.
  auto file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  }

  // Execute write.
  if (XSUCCEEDED(result)) {
    // TODO(benvanik): async path.
    if (true || file->is_synchronous()) {
      // Synchronous request.
      uint32_t bytes_written = 0;
      result = file->Write(buffer.guest_address(), buffer_length,
                           byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : -1,
                           &bytes_written, apc_context.guest_address());

      if (io_status_block) {
        io_status_block->status = result;
        io_status_block->information = static_cast<uint32_t>(bytes_written);
      }

      // Queue the APC callback. It must be delivered via the APC mechanism even
      // though were are completing immediately.
      // Low bit probably means do not queue to IO ports.
      if ((uint32_t)apc_routine & ~1) {
        if (apc_context) {
          auto thread = XThread::GetCurrentThread();
          thread->EnqueueApc(static_cast<uint32_t>(apc_routine) & ~1u, apc_context.guest_address(),
                             io_status_block.guest_address(), 0);
        }
      }

      if (!file->is_synchronous()) {
        result = X_STATUS_PENDING;
      }

      // Mark that we should signal the event now. We do this after
      // we have written the info out.
      signal_event = true;
    } else {
      // X_STATUS_PENDING if not returning immediately.
      result = X_STATUS_PENDING;

      if (io_status_block) {
        io_status_block->status = X_STATUS_PENDING;
        io_status_block->information = 0;
      }
    }
  }

  if (XFAILED(result) && io_status_block) {
    io_status_block->status = result;
    io_status_block->information = 0;
  }

  if (ev && signal_event) {
    ev->Set(0, false);
  }

  return result;
}

u32 NtCreateIoCompletion_entry(mapped_u32 out_handle, u32 desired_access,
                               mapped_void object_attribs, u32 num_concurrent_threads) {
  auto completion = new XIOCompletion(REX_KERNEL_STATE());
  if (out_handle) {
    *out_handle = completion->handle();
  }

  return X_STATUS_SUCCESS;
}

u32 NtSetIoCompletion_entry(u32 handle, u32 key_context, u32 apc_context, u32 completion_status,
                            u32 num_bytes) {
  auto port = REX_KERNEL_OBJECTS()->LookupObject<XIOCompletion>(handle);
  if (!port) {
    return X_STATUS_INVALID_HANDLE;
  }

  XIOCompletion::IONotification notification;
  notification.key_context = key_context;
  notification.apc_context = apc_context;
  notification.num_bytes = num_bytes;
  notification.status = completion_status;

  port->QueueNotification(notification);
  return X_STATUS_SUCCESS;
}

// Dequeues a packet from the completion port.
u32 NtRemoveIoCompletion_entry(u32 handle, mapped_u32 key_context, mapped_u32 apc_context,
                               ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, mapped_u64 timeout) {
  X_STATUS status = X_STATUS_SUCCESS;
  // uint32_t info = 0;

  auto port = REX_KERNEL_OBJECTS()->LookupObject<XIOCompletion>(handle);
  if (!port) {
    status = X_STATUS_INVALID_HANDLE;
  }

  uint64_t timeout_ticks = timeout ? static_cast<uint32_t>(*timeout)
                                   : static_cast<uint64_t>(std::numeric_limits<int64_t>::min());
  XIOCompletion::IONotification notification;
  if (port->WaitForNotification(timeout_ticks, &notification)) {
    if (key_context) {
      *key_context = notification.key_context;
    }
    if (apc_context) {
      *apc_context = notification.apc_context;
    }

    if (io_status_block) {
      io_status_block->status = notification.status;
      io_status_block->information = notification.num_bytes;
    }
  } else {
    status = X_STATUS_TIMEOUT;
  }

  return status;
}

u32 NtQueryFullAttributesFile_entry(ppc_ptr_t<X_OBJECT_ATTRIBUTES> obj_attribs,
                                    ppc_ptr_t<X_FILE_NETWORK_OPEN_INFORMATION> file_info) {
  auto object_name = REX_KERNEL_MEMORY()->TranslateVirtual<X_ANSI_STRING*>(obj_attribs->name_ptr);
  auto path_str = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);
  REXKRNL_IMPORT_TRACE("NtQueryFullAttributesFile", "path={}", path_str);

  object_ref<XFile> root_file;
  if (obj_attribs->root_directory != 0xFFFFFFFD &&  // ObDosDevices
      obj_attribs->root_directory != 0) {
    root_file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(obj_attribs->root_directory);
    assert_not_null(root_file);
    assert_true(root_file->type() == XObject::Type::File);
    assert_always();
  }

  auto target_path = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  // Resolve the file using the virtual file system.
  auto entry = REX_KERNEL_FS()->ResolvePath(target_path);
  if (entry) {
    // Found.
    file_info->creation_time = entry->create_timestamp();
    file_info->last_access_time = entry->access_timestamp();
    file_info->last_write_time = entry->write_timestamp();
    file_info->change_time = entry->write_timestamp();
    file_info->allocation_size = entry->allocation_size();
    file_info->end_of_file = entry->size();
    file_info->attributes = entry->attributes();

    REXKRNL_IMPORT_RESULT("NtQueryFullAttributesFile", "0x0 size={}", (uint64_t)entry->size());
    return X_STATUS_SUCCESS;
  }

  REXKRNL_IMPORT_RESULT("NtQueryFullAttributesFile", "X_STATUS_NO_SUCH_FILE");
  return X_STATUS_NO_SUCH_FILE;
}

u32 NtQueryDirectoryFile_entry(u32 file_handle, u32 event_handle, u32 apc_routine,
                               mapped_void apc_context,
                               ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                               ppc_ptr_t<X_FILE_DIRECTORY_INFORMATION> file_info_ptr, u32 length,
                               ppc_ptr_t<X_ANSI_STRING> file_name, u32 restart_scan) {
  if (length < 72) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  X_STATUS result = X_STATUS_UNSUCCESSFUL;
  uint32_t info = 0;

  auto file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(file_handle);
  auto name = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), file_name);

  // Enforce that the path is ASCII.
  if (!IsValidPath(name, true)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  if (file) {
    // X_FILE_DIRECTORY_INFORMATION dir_info = {0};
    result = file->QueryDirectory(file_info_ptr, length, name, restart_scan != 0);
    if (XSUCCEEDED(result)) {
      info = length;
    }
  } else {
    result = X_STATUS_NO_SUCH_FILE;
  }

  if (XFAILED(result)) {
    info = 0;
  }

  if (io_status_block) {
    io_status_block->status = result;
    io_status_block->information = info;
  }

  return result;
}

u32 NtFlushBuffersFile_entry(u32 file_handle, ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block_ptr) {
  auto result = X_STATUS_SUCCESS;

  if (io_status_block_ptr) {
    io_status_block_ptr->status = result;
    io_status_block_ptr->information = 0;
  }

  return result;
}

// https://docs.microsoft.com/en-us/windows/win32/devnotes/ntopensymboliclinkobject
u32 NtOpenSymbolicLinkObject_entry(mapped_u32 handle_out,
                                   ppc_ptr_t<X_OBJECT_ATTRIBUTES> object_attrs) {
  if (!object_attrs) {
    return X_STATUS_INVALID_PARAMETER;
  }
  assert_not_null(handle_out);

  assert_true(object_attrs->attributes == 64);  // case insensitive

  auto object_name = REX_KERNEL_MEMORY()->TranslateVirtual<X_ANSI_STRING*>(object_attrs->name_ptr);

  auto target_path = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  if (object_attrs->root_directory != 0) {
    assert_always();
  }

  if (rex::string::utf8_starts_with(target_path, "\\??\\")) {
    target_path = target_path.substr(4);  // Strip the full qualifier
  }

  std::string link_path;
  if (!REX_KERNEL_FS()->FindSymbolicLink(target_path, link_path)) {
    return X_STATUS_NO_SUCH_FILE;
  }

  object_ref<XSymbolicLink> symlink(new XSymbolicLink(REX_KERNEL_STATE()));
  symlink->Initialize(target_path, link_path);

  *handle_out = symlink->handle();

  return X_STATUS_SUCCESS;
}

// https://docs.microsoft.com/en-us/windows/win32/devnotes/ntquerysymboliclinkobject
u32 NtQuerySymbolicLinkObject_entry(u32 handle, ppc_ptr_t<X_ANSI_STRING> target) {
  auto symlink = REX_KERNEL_OBJECTS()->LookupObject<XSymbolicLink>(handle);
  if (!symlink) {
    return X_STATUS_NO_SUCH_FILE;
  }
  auto length = std::min(static_cast<size_t>(target->maximum_length), symlink->target().size());
  if (length > 0) {
    auto target_buf = REX_KERNEL_MEMORY()->TranslateVirtual(target->pointer);
    std::memcpy(target_buf, symlink->target().c_str(), length);
  }
  target->length = static_cast<uint16_t>(length);
  return X_STATUS_SUCCESS;
}

u32 FscGetCacheElementCount_entry(u32 r3) {
  return 0;
}

u32 FscSetCacheElementCount_entry(u32 unk_0, u32 unk_1) {
  // unk_0 = 0
  // unk_1 looks like a count? in what units? 256 is a common value
  return X_STATUS_SUCCESS;
}

u32 NtDeviceIoControlFile_entry(u32 handle, u32 event_handle, u32 apc_routine, u32 apc_context,
                                u32 io_status_block, u32 io_control_code, mapped_void input_buffer,
                                u32 input_buffer_len, mapped_void output_buffer,
                                u32 output_buffer_len) {
  // Called by XMountUtilityDrive cache-mounting code
  // (checks if the returned values look valid, values below seem to pass the
  // checks)
  const uint32_t cache_size = 0xFF000;

  const uint32_t X_IOCTL_DISK_GET_DRIVE_GEOMETRY = 0x70000;
  const uint32_t X_IOCTL_DISK_GET_PARTITION_INFO = 0x74004;

  if (io_control_code == X_IOCTL_DISK_GET_DRIVE_GEOMETRY) {
    if (output_buffer_len < 0x8) {
      assert_always();
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    memory::store_and_swap<uint32_t>(output_buffer, cache_size / 512);
    memory::store_and_swap<uint32_t>(output_buffer + 4, 512);
  } else if (io_control_code == X_IOCTL_DISK_GET_PARTITION_INFO) {
    if (output_buffer_len < 0x10) {
      assert_always();
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    memory::store_and_swap<uint64_t>(output_buffer, 0);
    memory::store_and_swap<uint64_t>(output_buffer + 8, cache_size);
  } else {
    REXKRNL_DEBUG("NtDeviceIoControlFile(0x{:X}) - unhandled IOCTL!", uint32_t(io_control_code));
    assert_always();
    return X_STATUS_INVALID_PARAMETER;
  }

  return X_STATUS_SUCCESS;
}

u32 IoCreateDevice_entry(u32 device_struct, u32 r4, u32 r5, u32 r6, u32 r7, mapped_u32 out_struct) {
  // Called from XMountUtilityDrive XAM-task code
  // That code tries writing things to a pointer at out_struct+0x18
  // We'll alloc some scratch space for it so it doesn't cause any exceptions

  // 0x24 is guessed size from accesses to out_struct - likely incorrect
  auto out_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x24);

  auto out = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(out_guest);
  memset(out, 0, 0x24);

  // XMountUtilityDrive writes some kind of header here
  // 0x1000 bytes should be enough to store it
  auto out_guest2 = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x1000);
  memory::store_and_swap(out + 0x18, out_guest2);

  *out_struct = out_guest;
  return X_STATUS_SUCCESS;
}

u32 IoDismountVolumeByFileHandle_entry(u32 handle) {
  REXKRNL_DEBUG("IoDismountVolumeByFileHandle({:#x}) ignored for host-backed VFS",
                (uint32_t)handle);
  return X_STATUS_SUCCESS;
}

u32 IoDismountVolumeByName_entry(ppc_ptr_t<X_ANSI_STRING> name) {
  REXKRNL_DEBUG("IoDismountVolumeByName ignored for host-backed VFS");
  return X_STATUS_SUCCESS;
}

u32 IoSynchronousDeviceIoControlRequest_entry(u32 ioctl, mapped_void device_object,
                                              mapped_void input_buffer, u32 input_length,
                                              mapped_void output_buffer, u32 output_length,
                                              mapped_u32 returned_length, u32 unk) {
  REXKRNL_WARN("IoSynchronousDeviceIoControlRequest({:#x}) - stub", (uint32_t)ioctl);
  return X_STATUS_SUCCESS;
}

u32 StfsCreateDevice_entry(mapped_void device_object, u32 flags, mapped_u32 out_device) {
  REXKRNL_WARN("StfsCreateDevice - stub");
  // if (out_device) *out_device = 0;
  return X_STATUS_SUCCESS;
}

u32 StfsControlDevice_entry(mapped_void device_object, u32 ioctl, mapped_void input_buffer,
                            u32 input_length, mapped_void output_buffer, u32 output_length) {
  REXKRNL_WARN("StfsControlDevice({:#x}) - stub", (uint32_t)ioctl);
  return X_STATUS_SUCCESS;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__NtCreateFile, rex::kernel::xboxkrnl::NtCreateFile_entry)
REX_EXPORT(__imp__NtOpenFile, rex::kernel::xboxkrnl::NtOpenFile_entry)
REX_EXPORT(__imp__NtReadFile, rex::kernel::xboxkrnl::NtReadFile_entry)
REX_EXPORT(__imp__NtReadFileScatter, rex::kernel::xboxkrnl::NtReadFileScatter_entry)
REX_EXPORT(__imp__NtWriteFile, rex::kernel::xboxkrnl::NtWriteFile_entry)
REX_EXPORT(__imp__NtCreateIoCompletion, rex::kernel::xboxkrnl::NtCreateIoCompletion_entry)
REX_EXPORT(__imp__NtSetIoCompletion, rex::kernel::xboxkrnl::NtSetIoCompletion_entry)
REX_EXPORT(__imp__NtRemoveIoCompletion, rex::kernel::xboxkrnl::NtRemoveIoCompletion_entry)
REX_EXPORT(__imp__NtQueryFullAttributesFile, rex::kernel::xboxkrnl::NtQueryFullAttributesFile_entry)
REX_EXPORT(__imp__NtQueryDirectoryFile, rex::kernel::xboxkrnl::NtQueryDirectoryFile_entry)
REX_EXPORT(__imp__NtFlushBuffersFile, rex::kernel::xboxkrnl::NtFlushBuffersFile_entry)
REX_EXPORT(__imp__NtOpenSymbolicLinkObject, rex::kernel::xboxkrnl::NtOpenSymbolicLinkObject_entry)
REX_EXPORT(__imp__NtQuerySymbolicLinkObject, rex::kernel::xboxkrnl::NtQuerySymbolicLinkObject_entry)
REX_EXPORT(__imp__FscGetCacheElementCount, rex::kernel::xboxkrnl::FscGetCacheElementCount_entry)
REX_EXPORT(__imp__FscSetCacheElementCount, rex::kernel::xboxkrnl::FscSetCacheElementCount_entry)
REX_EXPORT(__imp__NtDeviceIoControlFile, rex::kernel::xboxkrnl::NtDeviceIoControlFile_entry)
REX_EXPORT(__imp__IoCreateDevice, rex::kernel::xboxkrnl::IoCreateDevice_entry)
REX_EXPORT(__imp__IoDismountVolumeByFileHandle,
           rex::kernel::xboxkrnl::IoDismountVolumeByFileHandle_entry)
REX_EXPORT(__imp__IoDismountVolumeByName, rex::kernel::xboxkrnl::IoDismountVolumeByName_entry)
REX_EXPORT(__imp__IoSynchronousDeviceIoControlRequest,
           rex::kernel::xboxkrnl::IoSynchronousDeviceIoControlRequest_entry)
REX_EXPORT(__imp__StfsCreateDevice, rex::kernel::xboxkrnl::StfsCreateDevice_entry)
REX_EXPORT(__imp__StfsControlDevice, rex::kernel::xboxkrnl::StfsControlDevice_entry)

REX_EXPORT_STUB(__imp__IoAcquireDeviceObjectLock);
REX_EXPORT_STUB(__imp__IoAllocateIrp);
REX_EXPORT_STUB(__imp__IoBuildAsynchronousFsdRequest);
REX_EXPORT_STUB(__imp__IoBuildDeviceIoControlRequest);
REX_EXPORT_STUB(__imp__IoBuildSynchronousFsdRequest);
REX_EXPORT_STUB(__imp__IoCallDriver);
REX_EXPORT_STUB(__imp__IoCheckShareAccess);
REX_EXPORT_STUB(__imp__IoCompleteRequest);
REX_EXPORT_STUB(__imp__IoCreateFile);
REX_EXPORT_STUB(__imp__IoDeleteDevice);
REX_EXPORT_STUB(__imp__IoDismountVolume);
REX_EXPORT_STUB(__imp__IoFreeIrp);
REX_EXPORT_STUB(__imp__IoInitializeIrp);
REX_EXPORT_STUB(__imp__IoInvalidDeviceRequest);
REX_EXPORT_STUB(__imp__IoQueueThreadIrp);
REX_EXPORT_STUB(__imp__IoReleaseDeviceObjectLock);
REX_EXPORT_STUB(__imp__IoRemoveShareAccess);
REX_EXPORT_STUB(__imp__IoSetIoCompletion);
REX_EXPORT_STUB(__imp__IoSetShareAccess);
REX_EXPORT_STUB(__imp__IoStartNextPacket);
REX_EXPORT_STUB(__imp__IoStartNextPacketByKey);
REX_EXPORT_STUB(__imp__IoStartPacket);
REX_EXPORT_STUB(__imp__IoSynchronousFsdRequest);
REX_EXPORT_STUB(__imp__NtDeleteFile);
REX_EXPORT_STUB(__imp__NtSetSystemTime);
REX_EXPORT_STUB(__imp__NtWriteFileGather);
REX_EXPORT_STUB(__imp__IoAcquireCancelSpinLock);
REX_EXPORT_STUB(__imp__IoReleaseCancelSpinLock);
REX_EXPORT_STUB(__imp__NtCancelIoFile);
REX_EXPORT_STUB(__imp__NtCancelIoFileEx);
REX_EXPORT_STUB(__imp__SvodCreateDevice);

REX_EXPORT_STUB(__imp__SataCdRomRecordReset);
