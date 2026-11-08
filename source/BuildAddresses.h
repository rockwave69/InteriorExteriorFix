#pragma once
#include <cstdint>

// Absolute addresses taken from the supplied GTA SA reverse source.
// The plugin resolves them as RVAs relative to the preferred 0x400000 image base.
// Target: GTA San Andreas PC 1.0 US, x86 only.
namespace SA10US {
    constexpr std::uintptr_t kImageBase = 0x400000u;

    constexpr std::uintptr_t kIplStoreAddIplsNeededAtPosn   = 0x4045B0u;
    constexpr std::uintptr_t kIplStoreClearIplsNeededAtPosn = 0x4045E0u;
    constexpr std::uintptr_t kIplStoreSetIplsRequired       = 0x404700u;
    constexpr std::uintptr_t kIplStoreRemoveIpl             = 0x404B20u;
    constexpr std::uintptr_t kIplStoreLoadIpls              = 0x405170u;

    constexpr std::uintptr_t kStreamingInstanceLoadedModels = 0x4084F0u;
    constexpr std::uintptr_t kStreamingRemoveBigBuildings   = 0x4093B0u;
    constexpr std::uintptr_t kStreamingRemoveBuildingsArea  = 0x4094B0u;
    constexpr std::uintptr_t kStreamingProcessListRange     = 0x40C270u;
    constexpr std::uintptr_t kStreamingProcessList           = 0x40C450u;
    constexpr std::uintptr_t kStreamingAddModelsToRequestList= 0x40D3F0u;

    constexpr std::uintptr_t kRendererScanSectorListModels   = 0x5535D0u;
    constexpr std::uintptr_t kRendererScanSectorModelsVisible= 0x553650u;

    constexpr std::uintptr_t kEntryExitPreserveVisible       = 0x43ECF0u;
    constexpr std::uintptr_t kEntryExitRestoreVisible        = 0x43ED80u;

    constexpr std::uintptr_t kRendererSetupEntityVisibility  = 0x554230u;
    constexpr std::uintptr_t kRendererSetupBigVisibility     = 0x554650u;
    constexpr std::uintptr_t kRendererShouldStream           = 0x554EB0u;
    constexpr std::uintptr_t kRendererRequestObjectsDirection = 0x555CB0u;
}
