#include "MinHookRuntime.h"

namespace {
    using InitializeFn = MH_STATUS(WINAPI*)();
    using CreateHookFn = MH_STATUS(WINAPI*)(LPVOID, LPVOID, LPVOID*);
    using TargetHookFn = MH_STATUS(WINAPI*)(LPVOID);

    struct MinHookApi {
        InitializeFn initialize = nullptr;
        CreateHookFn createHook = nullptr;
        TargetHookFn enableHook = nullptr;
        TargetHookFn disableHook = nullptr;
        TargetHookFn removeHook = nullptr;
    };

    MinHookApi g_api{};
    bool g_apiResolved = false;

    HMODULE LoadFromGameDirectory(const wchar_t* dllName) noexcept {
        // GTA SA is a legacy Win32 game and its usual installation path is
        // comfortably within MAX_PATH. Fail safely rather than truncate.
        wchar_t executablePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(
            nullptr, executablePath, static_cast<DWORD>(MAX_PATH));
        if (length == 0 || length >= MAX_PATH)
            return nullptr;

        // Keep the directory prefix, replacing only the executable filename.
        DWORD writePosition = length;
        while (writePosition > 0 &&
            executablePath[writePosition - 1] != L'\\' &&
            executablePath[writePosition - 1] != L'/') {
            --writePosition;
        }
        if (writePosition == 0)
            return nullptr;

        for (DWORD i = 0; dllName[i] != L'\0'; ++i) {
            if (writePosition + 1 >= MAX_PATH)
                return nullptr;
            executablePath[writePosition++] = dllName[i];
        }
        executablePath[writePosition] = L'\0';
        return LoadLibraryW(executablePath);
    }

    HMODULE AcquireMinHookModule(const wchar_t* dllName) noexcept {
        HMODULE module = nullptr;

        // GetModuleHandleEx with flags=0 increments the module reference
        // count. We intentionally never release it while our hooks exist.
        if (GetModuleHandleExW(0, dllName, &module))
            return module;

        // Use an absolute path rooted at gta_sa.exe. Do not rely on the
        // process current directory or an unrestricted DLL search path.
        return LoadFromGameDirectory(dllName);
    }

    template <typename Fn>
    Fn ResolveFunction(HMODULE module, const char* name) noexcept {
        return reinterpret_cast<Fn>(GetProcAddress(module, name));
    }

    bool ResolveApi() noexcept {
        if (g_apiResolved)
            return g_api.initialize && g_api.createHook && g_api.enableHook &&
                g_api.disableHook && g_api.removeHook;

        // FLA's import library names MinHook.x86.dll. The other names are
        // compatibility fallbacks for game/plugin distributions that renamed
        // the same x86 MinHook binary.
        constexpr const wchar_t* kDllNames[] = {
            L"MinHook.x86.dll",
            L"MinHookx86.dll",
            L"MinHook.86.dll"
        };

        for (const wchar_t* dllName : kDllNames) {
            HMODULE module = AcquireMinHookModule(dllName);
            if (!module)
                continue;

            MinHookApi candidate{};
            candidate.initialize = ResolveFunction<InitializeFn>(module, "MH_Initialize");
            candidate.createHook = ResolveFunction<CreateHookFn>(module, "MH_CreateHook");
            candidate.enableHook = ResolveFunction<TargetHookFn>(module, "MH_EnableHook");
            candidate.disableHook = ResolveFunction<TargetHookFn>(module, "MH_DisableHook");
            candidate.removeHook = ResolveFunction<TargetHookFn>(module, "MH_RemoveHook");

            if (candidate.initialize && candidate.createHook &&
                candidate.enableHook && candidate.disableHook && candidate.removeHook) {
                g_api = candidate;
                g_apiResolved = true;
                return true;
            }

            // We acquired one reference for this candidate. Release it when
            // the binary does not expose the compatible MinHook API.
            FreeLibrary(module);
        }

        g_apiResolved = true;
        return false;
    }
}

namespace MinHookRuntime {
    bool Initialize() noexcept {
        if (!ResolveApi())
            return false;

        const MH_STATUS status = g_api.initialize();
        // ALREADY_INITIALIZED is expected when FLA initialized the shared
        // MinHook instance before this ASI. Never uninitialize shared state.
        return status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED;
    }

    MH_STATUS CreateHook(void* target, void* detour, void** original) noexcept {
        if (!ResolveApi())
            return MH_ERROR_NOT_INITIALIZED;
        return g_api.createHook(target, detour, original);
    }

    MH_STATUS EnableHook(void* target) noexcept {
        if (!ResolveApi())
            return MH_ERROR_NOT_INITIALIZED;
        return g_api.enableHook(target);
    }

    MH_STATUS DisableHook(void* target) noexcept {
        if (!ResolveApi())
            return MH_ERROR_NOT_INITIALIZED;
        return g_api.disableHook(target);
    }

    MH_STATUS RemoveHook(void* target) noexcept {
        if (!ResolveApi())
            return MH_ERROR_NOT_INITIALIZED;
        return g_api.removeHook(target);
    }
}
