// spiderman3 - Save system fix: synchronous SUCCESS + async XN_SYS_UI lifecycle
//
// Device selector: returns SUCCESS with device_id=1 (HDD) synchronously,
//   broadcasts XN_SYS_UI=true immediately, spawns a detached thread that
//   broadcasts XN_SYS_UI=false after 200ms (for the late-created listener).
//
// Runtime fixes (in custom rexruntime.dll):
//   - xeXamEnumerate async path returns X_ERROR_IO_PENDING (not SUCCESS)
//   - WriteItems returns X_ERROR_SUCCESS for 0 items (not ERROR_NO_MORE_FILES)
//   - xeXamDispatchHeadless broadcasts XN_SYS_UI true/false via deferred path

#include "generated/default/spiderman3_init.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/thread.h>
#include <thread>
#include <chrono>

REX_HOOK_RAW(XMPGetStatus_Wrapper) {
  uint32_t status_ptr = ctx.r3.u32;
  REX_STORE_U32(status_ptr, 0);
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(XamContentGetDeviceState_Wrapper) {
  ctx.r3.u64 = 0;
}

// r3=user_index, r4=content_type, r5=content_flags, r6=total_requested, r7=device_id_ptr, r8=overlapped_ptr
REX_HOOK_RAW(XamShowDeviceSelectorUI_Wrapper) {
  uint32_t device_id_ptr = ctx.r7.u32;
  uint32_t overlapped_ptr = ctx.r8.u32;

  // Write device_id = 1 (HDD) and overlapped result = SUCCESS
  if (device_id_ptr != 0) {
    REX_STORE_U32(device_id_ptr, 1);
  }
  if (overlapped_ptr != 0) {
    REX_STORE_U32(overlapped_ptr + 0, 0);
  }

  // Broadcast XN_SYS_UI = true (UI showing) — existing listeners receive it
  REX_KERNEL_STATE()->BroadcastNotification(0x9, true);

  // Spawn detached thread to broadcast XN_SYS_UI = false after 200ms.
  // Game returns from hook, creates F8000090 listener, starts polling.
  // 200ms later, false is broadcast and F8000090 receives it.
  std::thread([]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  }).detach();

  ctx.r3.u64 = 0;  // return SUCCESS
}
