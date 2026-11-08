# SA Interior Exterior Fix

Fixes outdoor static geometry disappearing when entering interiors located in the main game world.

![Demo](screens/screen%20(1).jpg)

## How it works

The plugin temporarily applies the current Area Code to eligible outdoor static entities while the game's Renderer and Streaming checks run, then restores their original Area Code. It also requests outdoor IPLs and processes the exterior model ring beyond the game's native streaming radius.

`CGame::currArea` is not modified. Area Codes of dynamic entities, pedestrians, and vehicles are not overridden. The game's global Far Clip / Draw Distance is not changed.

## Configuration

```ini
[Streaming]
ExteriorStreamingRadius=80.0
```

The radius is specified in game units. Valid range: `40.0`–`400.0`. Default: `80.0`. This value sets the outer limit of the additional streaming pass and does not reduce the game's native streaming distance. Changes take effect after restarting the game.

## Hook addresses

Absolute addresses for GTA SA 1.0 US:

| Function | Address |
|---|---:|
| `CRenderer::ScanSectorList_ListModels` | `0x005535D0` |
| `CRenderer::ScanSectorList_ListModelsVisible` | `0x00553650` |
| `CRenderer::SetupEntityVisibility` | `0x00554230` |
| `CRenderer::SetupBigBuildingVisibility` | `0x00554650` |
| `CRenderer::ShouldModelBeStreamed` | `0x00554EB0` |
| `CStreaming::InstanceLoadedModels` | `0x004084F0` |
| `CStreaming::ProcessEntitiesInSectorList` (bounded) | `0x0040C270` |
| `CStreaming::ProcessEntitiesInSectorList` (list) | `0x0040C450` |
| `CStreaming::AddModelsToRequestList` | `0x0040D3F0` |
| `CIplStore::SetIplsRequired` | `0x00404700` |
| `CIplStore::LoadIpls` | `0x00405170` |

## Build

Build for **Win32** with Plugin-SDK. MinHook API functions are resolved at runtime from the shared `MinHook.x86.dll` same as used by Fastman92 Limit Adjuster. The plugin manages only its own hooks and does not call `MH_Uninitialize`.
