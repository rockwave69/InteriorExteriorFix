#pragma once

#include <Windows.h>

// Minimal ABI declarations matching the MinHook API used by FLA.
// The implementation resolves these functions from the game's shared
// MinHook.x86.dll at runtime; no MinHook sources or import library are linked.
enum MH_STATUS {
    MH_UNKNOWN = -1,
    MH_OK = 0,
    MH_ERROR_ALREADY_INITIALIZED,
    MH_ERROR_NOT_INITIALIZED,
    MH_ERROR_ALREADY_CREATED,
    MH_ERROR_NOT_CREATED,
    MH_ERROR_ENABLED,
    MH_ERROR_DISABLED,
    MH_ERROR_NOT_EXECUTABLE,
    MH_ERROR_UNSUPPORTED_FUNCTION,
    MH_ERROR_MEMORY_ALLOC,
    MH_ERROR_MEMORY_PROTECT,
    MH_ERROR_MODULE_NOT_FOUND,
    MH_ERROR_FUNCTION_NOT_FOUND
};

namespace MinHookRuntime {
    // Uses the already-loaded FLA MinHook module when present. If it is not
    // loaded yet, tries to load a compatible DLL from the GTA SA directory.
    // The module reference is intentionally kept for the process lifetime.
    bool Initialize() noexcept;

    MH_STATUS CreateHook(void* target, void* detour, void** original) noexcept;
    MH_STATUS EnableHook(void* target) noexcept;
    MH_STATUS DisableHook(void* target) noexcept;
    MH_STATUS RemoveHook(void* target) noexcept;
}
