// tenchuz - fresh-profile fix for XUserReadProfileSettings.
//
// ReXGlue reports profile settings that were never written with source
// XSOURCE_NO_VALUE (0) and no data. Tenchu Z only treats XSOURCE_DEFAULT (1) as
// "nothing saved"; for anything else it copies 20 bytes from the setting's data
// pointer. With a fresh profile that pointer is NULL, so the game crashed as
// soon as Start was pressed on the title screen (sub_82282F20 reading
// XPROFILE_TITLE_SPECIFIC1). Report unset settings as XSOURCE_DEFAULT so the
// game falls back to its built-in defaults, as it does on a console.

#include "generated/default/tenchuz_pch.h"

#include <rex/hook.h>

namespace {

constexpr uint32_t kXSourceNoValue = 0;
constexpr uint32_t kXSourceDefault = 1;
constexpr uint32_t kSettingSize = 40;  // sizeof(XUSER_PROFILE_SETTING)
constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorIoPending = 997;

}  // namespace

REX_EXTERN(__imp__sub_8255EAB0);

// XUserReadProfileSettings(title_id, user_index, count, setting_ids,
//                          size_ptr, results, overlapped)
REX_HOOK_RAW(sub_8255EAB0) {
  const uint32_t results = ctx.r8.u32;
  __imp__sub_8255EAB0(ctx, base);

  // ReXGlue completes the overlapped read immediately, so the results are
  // already filled in when IO_PENDING comes back.
  const uint32_t status = ctx.r3.u32;
  if (!results || (status != kErrorSuccess && status != kErrorIoPending)) {
    return;
  }
  const uint32_t count = REX_LOAD_U32(results);
  const uint32_t settings = REX_LOAD_U32(results + 4);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t setting = settings + i * kSettingSize;
    if (REX_LOAD_U32(setting) == kXSourceNoValue) {
      REX_STORE_U32(setting, kXSourceDefault);
    }
  }
}
