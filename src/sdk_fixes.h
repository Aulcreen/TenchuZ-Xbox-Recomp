// tenchuz - fixes to the prebuilt ReXGlue SDK runtime that don't need an SDK
// rebuild (sdk_fixes.cpp).

#pragma once

namespace tenchuz::sdk_fixes {

// Once at startup, before the runtime starts its threads' work.
void Apply();

}  // namespace tenchuz::sdk_fixes
