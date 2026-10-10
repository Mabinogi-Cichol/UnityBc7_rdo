// Compile the pinned, unchanged upstream translation unit with local adapters.
// Its two transaction node types use nothrow allocation; API wrappers record
// failures that upstream Commit/Abort otherwise deliberately ignore.
#define DETOURS_INTERNAL
#include <windows.h>
#include <detours.h>
#include <new>
#include "detours_checked_api.h"
#define new new (std::nothrow)
#define GetThreadContext rgbm_engine::checked_get_context
#define SetThreadContext rgbm_engine::checked_set_context
#define VirtualProtect rgbm_engine::checked_protect
#define FlushInstructionCache rgbm_engine::checked_flush
#define ResumeThread rgbm_engine::checked_resume
#include "../vendor/detours/src/detours.cpp"
#undef new
#undef GetThreadContext
#undef SetThreadContext
#undef VirtualProtect
#undef FlushInstructionCache
#undef ResumeThread
