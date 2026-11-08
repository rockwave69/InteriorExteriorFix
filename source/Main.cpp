#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "plugin.h"
#include "Fix.h"

namespace {
    // 0 = not initialized, 1 = initialization in progress, 2 = attempted.
    volatile LONG g_bootstrapState = 0;

    void EnsurePluginInitialized() {
        if (InterlockedCompareExchange(&g_bootstrapState, 1, 0) != 0)
            return;

        FixInstallHooks();
        InterlockedExchange(&g_bootstrapState, 2);
    }

    void OnRwInitialized() {
        EnsurePluginInitialized();
    }

    void OnGameProcess() {
        if (InterlockedCompareExchange(&g_bootstrapState, 0, 0) == 0)
            EnsurePluginInitialized();
    }

    struct PluginBootstrap {
        PluginBootstrap() {
            // Install only after the game has passed Plugin-SDK initialization;
            // never access game state from DllMain / loader-lock context.
            plugin::Events::initRwEvent += OnRwInitialized;
            plugin::Events::gameProcessEvent += OnGameProcess;
        }
    } g_pluginBootstrap;
}
