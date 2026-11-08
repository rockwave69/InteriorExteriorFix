#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Fix.h"
#include "Config.h"
#include "BuildAddresses.h"

#include <Windows.h>
#include "MinHookRuntime.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstddef>

#include "CGame.h"
#include "CEntity.h"
#include "CVector.h"
#include "CRenderer.h"
#include "CStreaming.h"
#include "CWorld.h"
#include "CSector.h"
#include "CRepeatSector.h"
#include "CPtrList.h"
#include "CRect.h"

namespace {
    constexpr unsigned char kAreaMainMap = 0;
    // Use the Plugin-SDK enum values: NOTHING=0, BUILDING=1, VEHICLE=2,
    // PED=3, OBJECT=4, DUMMY=5. Do not infer these from list positions.
    constexpr int kEntityTypeBuilding = ENTITY_TYPE_BUILDING;
    constexpr int kEntityTypeObject = ENTITY_TYPE_OBJECT;
    constexpr int kEntityTypeDummy = ENTITY_TYPE_DUMMY;
    constexpr float kNativeInteriorStreamingRadius = 40.0f;
    constexpr float kNativeWorldStreamingRadius = 80.0f;
    constexpr float kWorldSpaceZCeiling = 900.0f;
    constexpr std::size_t kBatchCapacity = 1024;
    constexpr std::size_t kMaxPtrListTraversal = 65536;
    constexpr std::uintptr_t kHighestHookAddress = SA10US::kRendererShouldStream;

    // ABI-only view of GTA SA CPtrList/CPtrNode. The reverse-engineered x86
    // layout is: CPtrList { CPtrNode* m_pHead; } and CPtrNode
    // { void* m_pVoid; CPtrNode* m_pNext; }. We intentionally do not access
    // SDK C++ members/accessors here because Plugin-SDK distributions expose
    // different declarations (and may make the fields protected).
    struct CPtrNodeAbi {
        void* pEntity;
        CPtrNodeAbi* pNext;
    };

    struct CPtrListAbi {
        CPtrNodeAbi* pHead;
    };

    static_assert(sizeof(void*) == 4, "SAInteriorExteriorFix requires Win32/x86");
    static_assert(sizeof(CPtrNodeAbi) == 8, "Unexpected GTA SA CPtrNode x86 layout");
    static_assert(sizeof(CPtrListAbi) == 4, "Unexpected GTA SA CPtrList x86 layout");
    static_assert(offsetof(CPtrNodeAbi, pEntity) == 0, "Unexpected CPtrNode entity offset");
    static_assert(offsetof(CPtrNodeAbi, pNext) == 4, "Unexpected CPtrNode next offset");
    static_assert(offsetof(CPtrListAbi, pHead) == 0, "Unexpected CPtrList head offset");

    volatile LONG g_lastExpandedRequestRunTick = 0;
    thread_local int g_loadIplsDepth = 0;
    thread_local bool g_iplFocusAppliedInCycle = false;
    volatile LONG g_lastExpandedInstanceRunTick = 0;
    bool g_hooksInstalled = false;
    constexpr std::size_t kMaximumOwnedHooks = 16;
    std::array<void*, kMaximumOwnedHooks> g_createdHookTargets{};
    std::array<void*, kMaximumOwnedHooks> g_enabledHookTargets{};
    std::size_t g_createdHookCount = 0;
    std::size_t g_enabledHookCount = 0;

    // AddModelsToRequestList calls ProcessEntitiesInSectorList synchronously.
    // The central list overload has no per-entity XY/radius test, and repeat-sector
    // buckets can contain hash aliases. Keep the Area Code bypass local to the native
    // streaming focus so we do not request unrelated outdoor models from other regions.
    thread_local int g_streamRequestDepth = 0;
    thread_local CVector g_streamRequestPosition{};
    thread_local float g_streamRequestInnerRadius = 0.0f;
    // STREAMING_LOADING_SCENE = 0x20 in GTA SA (1 << 5).
    constexpr unsigned int kStreamingLoadingSceneFlag = 1u << 5;

    using SetupEntityVisibilityFn = int(__cdecl*)(CEntity*, float&);
    using ScanSectorFn = void(__cdecl*)(int, int);
    using SetupBigBuildingVisibilityFn = int(__cdecl*)(CEntity*, float&);
    using ShouldModelBeStreamedFn = bool(__cdecl*)(CEntity*, const CVector&, float);
    using ProcessListRangeFn = void(__cdecl*)(CPtrList*, float, float, float, float, float, float, float, unsigned int);
    using ProcessListFn = void(__cdecl*)(CPtrList*, unsigned int);
    using InstanceLoadedModelsFn = void(__cdecl*)(const CVector*);
    using AddModelsToRequestListFn = void(__cdecl*)(const CVector*, unsigned int);
    using SetIplsRequiredFn = void(__cdecl*)(const CVector*, int);
    using LoadIplsFn = void(__cdecl*)(CVector, bool);

    SetupEntityVisibilityFn g_originalSetupEntityVisibility = nullptr;
    ScanSectorFn g_originalScanSectorListModels = nullptr;
    ScanSectorFn g_originalScanSectorListModelsVisible = nullptr;
    SetupBigBuildingVisibilityFn g_originalSetupBigBuildingVisibility = nullptr;
    ShouldModelBeStreamedFn g_originalShouldModelBeStreamed = nullptr;
    ProcessListRangeFn g_originalProcessListRange = nullptr;
    ProcessListFn g_originalProcessList = nullptr;
    InstanceLoadedModelsFn g_originalInstanceLoadedModels = nullptr;
    AddModelsToRequestListFn g_originalAddModelsToRequestList = nullptr;
    SetIplsRequiredFn g_originalSetIplsRequired = nullptr;
    LoadIplsFn g_originalLoadIpls = nullptr;

    void* ResolveGameAddress(std::uintptr_t absoluteAddress) {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
        if (absoluteAddress < SA10US::kImageBase || base == 0)
            return nullptr;
        return reinterpret_cast<void*>(base + (absoluteAddress - SA10US::kImageBase));
    }

    bool IsExecutableAddress(void* address) {
        if (!address)
            return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        if (mbi.State != MEM_COMMIT)
            return false;
        const DWORD p = mbi.Protect & 0xFF;
        return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
               p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
    }

    bool ValidateTargetImage() {
        HMODULE module = GetModuleHandleA(nullptr);
        if (!module)
            return false;

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        if (dos->e_lfanew <= 0 || static_cast<std::uintptr_t>(dos->e_lfanew) > 0x1000u)
            return false;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            reinterpret_cast<const unsigned char*>(module) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32) ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            nt->OptionalHeader.ImageBase != SA10US::kImageBase)
            return false;

        // Verify that the executable image contains every function address used
        // by the functional hook set.
        const std::uintptr_t maxRva = kHighestHookAddress - SA10US::kImageBase + 0x20;
        if (nt->OptionalHeader.SizeOfImage <= maxRva)
            return false;

        return true;
    }

    bool IsFiniteVector(const CVector& position) {
        return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
    }

    struct SectorRange {
        int left;
        int right;
        int bottom;
        int top;
    };

    bool GetClampedSectorRange(const CVector& position, float radius, SectorRange& range) {
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(radius) || radius < 0.0f)
            return false;

        const float rawMinX = position.x - radius;
        const float rawMaxX = position.x + radius;
        const float rawMinY = position.y - radius;
        const float rawMaxY = position.y + radius;

        // The world maximum is exclusive. Reject ranges that do not overlap the
        // map before converting coordinates to integer sector indices.
        if (rawMaxX < WORLD_MIN_X || rawMinX >= WORLD_MAX_X ||
            rawMaxY < WORLD_MIN_Y || rawMinY >= WORLD_MAX_Y)
            return false;

        const float minX = (std::max)(rawMinX, static_cast<float>(WORLD_MIN_X));
        const float maxX = (std::min)(rawMaxX, static_cast<float>(WORLD_MAX_X));
        const float minY = (std::max)(rawMinY, static_cast<float>(WORLD_MIN_Y));
        const float maxY = (std::min)(rawMaxY, static_cast<float>(WORLD_MAX_Y));

        range.left = (std::max)(0, (std::min)(NUMSECTORS_X - 1, CWorld::GetSectorIndexX(minX)));
        range.right = (std::max)(0, (std::min)(NUMSECTORS_X - 1, CWorld::GetSectorIndexX(maxX)));
        range.bottom = (std::max)(0, (std::min)(NUMSECTORS_Y - 1, CWorld::GetSectorIndexY(minY)));
        range.top = (std::max)(0, (std::min)(NUMSECTORS_Y - 1, CWorld::GetSectorIndexY(maxY)));
        return range.left <= range.right && range.bottom <= range.top;
    }

    float GetNativeStreamingRadius() {
        // Match both CStreaming::AddModelsToRequestList and InstanceLoadedModels.
        return CGame::CanSeeOutSideFromCurrArea() ? kNativeWorldStreamingRadius : kNativeInteriorStreamingRadius;
    }

    bool IsWorldSpaceInteriorActive(const CVector* referencePosition = nullptr) {
        if (CGame::currArea == kAreaMainMap || CGame::currArea < 1 || CGame::currArea > 255)
            return false;

        // Streaming/ IPL hooks receive the authoritative scene or player position.
        // During the first load/door transition ms_vecCameraPosition may still be
        // (0,0,0) or may belong to the previous scene. Do not reject a valid low-Z
        // world-space interior merely because the camera matrix has not caught up.
        const CVector& position = referencePosition
            ? *referencePosition
            : CRenderer::ms_vecCameraPosition;
        return IsFiniteVector(position) && position.z < kWorldSpaceZCeiling;
    }

    bool IsEligibleExteriorStatic(CEntity* entity) {
        if (!entity || entity->m_nAreaCode != kAreaMainMap)
            return false;

        const int type = static_cast<int>(entity->m_nType);
        if (type == kEntityTypeBuilding || type == kEntityTypeDummy)
            return true;
        if (type == kEntityTypeObject)
            return entity->bIsStatic;
        return false;
    }

    class ScopedEntityAreaOverride {
    public:
        explicit ScopedEntityAreaOverride(CEntity* entity,
            const CVector* referencePosition = nullptr) : m_entity(entity) {
            if (!IsWorldSpaceInteriorActive(referencePosition) || !IsEligibleExteriorStatic(entity)) {
                m_entity = nullptr;
                return;
            }
            m_oldArea = entity->m_nAreaCode;
            entity->m_nAreaCode = static_cast<unsigned char>(CGame::currArea);
        }

        ~ScopedEntityAreaOverride() {
            if (m_entity)
                m_entity->m_nAreaCode = m_oldArea;
        }

    private:
        CEntity* m_entity = nullptr;
        unsigned char m_oldArea = 0;
    };

    class ScopedAreaBatch {
    public:
        explicit ScopedAreaBatch(const CVector* referencePosition = nullptr)
            : m_referencePosition(referencePosition) {}
        ScopedAreaBatch(const ScopedAreaBatch&) = delete;
        ScopedAreaBatch& operator=(const ScopedAreaBatch&) = delete;

        ~ScopedAreaBatch() {
            for (std::size_t i = m_count; i > 0; --i)
                m_entries[i - 1].entity->m_nAreaCode = m_entries[i - 1].oldArea;
        }

        void AddList(CPtrList* list) {
            if (!list || !IsWorldSpaceInteriorActive(m_referencePosition))
                return;
            ForEach(list, [this](CEntity* entity) { AddEntity(entity); });
        }

        // Mirror the bounded ProcessEntitiesInSectorList overload. Its native
        // predicate is rectangular clipping followed by a circular radius test.
        void AddListWithinBounds(CPtrList* list, float centerX, float centerY,
            float minX, float minY, float maxX, float maxY, float radius, float innerRadius = 0.0f) {
            if (!list || !IsWorldSpaceInteriorActive(m_referencePosition) ||
                !std::isfinite(centerX) || !std::isfinite(centerY) || !std::isfinite(radius) ||
                !std::isfinite(innerRadius) || radius <= 0.0f || innerRadius < 0.0f || innerRadius >= radius)
                return;
            const float radiusSqr = radius * radius;
            const float innerRadiusSqr = innerRadius * innerRadius;
            ForEach(list, [this, centerX, centerY, minX, minY, maxX, maxY, radiusSqr, innerRadius, innerRadiusSqr](CEntity* entity) {
                if (!IsEligibleExteriorStatic(entity))
                    return;
                const CVector& pos = entity->GetPosition();
                if (pos.x > minX && pos.x < maxX && pos.y > minY && pos.y < maxY) {
                    const float dx = centerX - pos.x;
                    const float dy = centerY - pos.y;
                    const float distanceSqr = dx * dx + dy * dy;
                    const bool outsideInnerRadius = innerRadius <= 0.0f || distanceSqr > innerRadiusSqr;
                    if (outsideInnerRadius && distanceSqr <= radiusSqr)
                        AddEntity(entity);
                }
            });
        }

        // Repeat-sector lists are 16x16 wrapping hash buckets, not 120x120
        // world sectors. Filter by the entity's actual bounds before overriding
        // its area, otherwise distant hash aliases could be instantiated.

        void AddListOverlappingSector(CPtrList* list, int sectorX, int sectorY) {
            if (!list || !IsWorldSpaceInteriorActive(m_referencePosition))
                return;

            constexpr float worldMinX = WORLD_MIN_X;
            constexpr float worldMinY = WORLD_MIN_Y;
            constexpr float sectorWidth = SECTOR_SIZE_X;
            constexpr float sectorHeight = SECTOR_SIZE_Y;
            const float left = worldMinX + sectorX * sectorWidth;
            const float right = left + sectorWidth;
            const float bottom = worldMinY + sectorY * sectorHeight;
            const float top = bottom + sectorHeight;

            ForEach(list, [this, left, right, bottom, top](CEntity* entity) {
                // RepeatSector::Objects also contains dynamic/non-world entities.
                // Filter type, static state and exterior Area Code before querying
                // collision bounds, which are only needed for eligible map objects.
                if (!IsEligibleExteriorStatic(entity))
                    return;

                const CRect bounds = entity->GetBoundRect();
                if (bounds.right >= left && bounds.left <= right &&
                    bounds.top >= bottom && bounds.bottom <= top)
                    AddEntity(entity);
            });
        }

        template <typename Callback>
        static void ForEach(CPtrList* list, Callback callback) {
            if (!list)
                return;

            // Use the verified game ABI rather than SDK member names. CPtrList
            // is a one-pointer header; both single-link and double-link nodes
            // begin with the same { item, next } CPtrNode base.
            const auto* listAbi = reinterpret_cast<const CPtrListAbi*>(list);
            CPtrNodeAbi* node = listAbi->pHead;
            std::size_t visited = 0;

            while (node && visited < kMaxPtrListTraversal) {
                ++visited;
                // Read next before invoking client code. The callback only
                // changes CEntity::m_nAreaCode, never list membership.
                CPtrNodeAbi* next = node->pNext;
                CEntity* entity = static_cast<CEntity*>(node->pEntity);
                if (entity)
                    callback(entity);
                node = next;
            }

            // A hard traversal limit prevents a corrupt or cyclic list from
            // trapping the game thread inside a renderer/streaming callback.
        }

    private:
        void AddEntity(CEntity* entity) {
            if (!IsEligibleExteriorStatic(entity))
                return;
            if (m_count >= m_entries.size())
                return;

            m_entries[m_count].entity = entity;
            m_entries[m_count].oldArea = entity->m_nAreaCode;
            ++m_count;
            entity->m_nAreaCode = static_cast<unsigned char>(CGame::currArea);
        }
        struct RestoreEntry {
            CEntity* entity;
            unsigned char oldArea;
        };
        // Only [0, m_count) is initialized. Avoid zeroing the full 8 KiB array
        // on every sector-scan hook invocation.
        std::array<RestoreEntry, kBatchCapacity> m_entries;
        std::size_t m_count = 0;
        const CVector* m_referencePosition = nullptr;
    };

    void AddExteriorStaticSectorLists(ScopedAreaBatch& batch, int sectorX, int sectorY,
        bool rendererScanSemantics = false) {
        if (sectorX < 0 || sectorY < 0 || sectorX >= NUMSECTORS_X || sectorY >= NUMSECTORS_Y)
            return;

        CSector* sector = CWorld::GetSector(sectorX, sectorY);
        if (sector) {
            // These are the true 120x120 static-sector lists, so their membership
            // already represents the requested world sector.
            batch.AddList(&sector->m_buildingList);
            batch.AddList(&sector->m_dummyList);
        }

        CRepeatSector* repeatSector = CWorld::GetRepeatSector(sectorX & 15, sectorY & 15);
        if (repeatSector) {
            CPtrList* repeatObjects = &repeatSector->m_lists[REPEATSECTOR_OBJECTS];
            if (rendererScanSemantics) {
                // CRenderer::ConstructPtrList scans the whole repeat-sector bucket and
                // marks scan codes before its Area Code test. Match that order exactly:
                // an alias may be encountered before the sector matching its real bounds.
                // Native frustum/visibility and distance checks still decide rendering.
                batch.AddList(repeatObjects);
            } else {
                // InstanceLoadedModels has no per-entity distance test. Restrict this
                // special pass to objects whose actual bounds overlap this world sector,
                // avoiding mass RW instancing from other 16x16 repeat-sector aliases.
                batch.AddListOverlappingSector(repeatObjects, sectorX, sectorY);
            }
        }
    }

    const CVector* GetStreamingRequestPosition() {
        return g_streamRequestDepth > 0 ? &g_streamRequestPosition : nullptr;
    }

    class ScopedStreamingRequestContext {
    public:
        explicit ScopedStreamingRequestContext(const CVector& position, float innerRadius = 0.0f)
            : m_previousPosition(g_streamRequestPosition),
              m_previousInnerRadius(g_streamRequestInnerRadius),
              m_previousDepth(g_streamRequestDepth) {
            g_streamRequestPosition = position;
            g_streamRequestInnerRadius = innerRadius;
            g_streamRequestDepth = m_previousDepth + 1;
        }

        ~ScopedStreamingRequestContext() {
            g_streamRequestPosition = m_previousPosition;
            g_streamRequestInnerRadius = m_previousInnerRadius;
            g_streamRequestDepth = m_previousDepth;
        }

        ScopedStreamingRequestContext(const ScopedStreamingRequestContext&) = delete;
        ScopedStreamingRequestContext& operator=(const ScopedStreamingRequestContext&) = delete;

    private:
        CVector m_previousPosition;
        float m_previousInnerRadius;
        int m_previousDepth;
    };

    class ScopedIplLoadCycle {
    public:
        ScopedIplLoadCycle() : m_previousDepth(g_loadIplsDepth) {
            ++g_loadIplsDepth;
            if (m_previousDepth == 0)
                g_iplFocusAppliedInCycle = false;
        }

        ~ScopedIplLoadCycle() {
            g_loadIplsDepth = m_previousDepth;
        }

        ScopedIplLoadCycle(const ScopedIplLoadCycle&) = delete;
        ScopedIplLoadCycle& operator=(const ScopedIplLoadCycle&) = delete;

    private:
        int m_previousDepth;
    };

    template <typename Fn>
    bool CreateHook(std::uintptr_t absoluteAddress, Fn detour, Fn* original) {
        if (g_createdHookCount >= g_createdHookTargets.size())
            return false;

        void* target = ResolveGameAddress(absoluteAddress);
        if (!IsExecutableAddress(target))
            return false;

        for (std::size_t i = 0; i < g_createdHookCount; ++i) {
            if (g_createdHookTargets[i] == target)
                return false;
        }

        if (MinHookRuntime::CreateHook(
            target, reinterpret_cast<void*>(detour),
            reinterpret_cast<void**>(original)) != MH_OK)
            return false;

        g_createdHookTargets[g_createdHookCount++] = target;
        return true;
    }

    void RemoveOwnedHooks() noexcept {
        while (g_enabledHookCount > 0) {
            void* target = g_enabledHookTargets[--g_enabledHookCount];
            MinHookRuntime::DisableHook(target);
            g_enabledHookTargets[g_enabledHookCount] = nullptr;
        }

        while (g_createdHookCount > 0) {
            void* target = g_createdHookTargets[--g_createdHookCount];
            MinHookRuntime::RemoveHook(target);
            g_createdHookTargets[g_createdHookCount] = nullptr;
        }
    }

    bool EnableOwnedHooks() {
        for (std::size_t i = 0; i < g_createdHookCount; ++i) {
            void* target = g_createdHookTargets[i];
            if (MinHookRuntime::EnableHook(target) != MH_OK)
                return false;

            g_enabledHookTargets[g_enabledHookCount++] = target;
        }
        return true;
    }

    void __cdecl Hook_ScanSectorListModels(int sectorX, int sectorY) {
        ScopedAreaBatch batch;
        if (IsWorldSpaceInteriorActive())
            AddExteriorStaticSectorLists(batch, sectorX, sectorY, true);
        g_originalScanSectorListModels(sectorX, sectorY);
    }

    void __cdecl Hook_ScanSectorListModelsVisible(int sectorX, int sectorY) {
        ScopedAreaBatch batch;
        if (IsWorldSpaceInteriorActive())
            AddExteriorStaticSectorLists(batch, sectorX, sectorY, true);
        g_originalScanSectorListModelsVisible(sectorX, sectorY);
    }

    int __cdecl Hook_SetupEntityVisibility(CEntity* entity, float& outDistance) {
        ScopedEntityAreaOverride overrideArea(entity);
        return g_originalSetupEntityVisibility(entity, outDistance);
    }

    int __cdecl Hook_SetupBigBuildingVisibility(CEntity* entity, float& outDistance) {
        ScopedEntityAreaOverride overrideArea(entity);
        return g_originalSetupBigBuildingVisibility(entity, outDistance);
    }

    bool __cdecl Hook_ShouldModelBeStreamed(CEntity* entity, const CVector& origin, float farClip) {
        ScopedEntityAreaOverride overrideArea(entity, &origin);
        return g_originalShouldModelBeStreamed(entity, origin, farClip);
    }

    void __cdecl Hook_ProcessListRange(CPtrList* list, float centerX, float centerY,
        float minX, float minY, float maxX, float maxY, float radius, unsigned int flags) {
        ScopedAreaBatch batch(GetStreamingRequestPosition());
        batch.AddListWithinBounds(list, centerX, centerY, minX, minY, maxX, maxY,
            radius, g_streamRequestInnerRadius);
        g_originalProcessListRange(list, centerX, centerY, minX, minY, maxX, maxY, radius, flags);
    }

    // Requests only the additional annulus outside the game's native streaming
    // radius. The normal request pass remains untouched and retains the stock
    // 40/80-unit behavior.
    void RequestExpandedExteriorModels(const CVector& position, unsigned int flags) {
        if (!IsFiniteVector(position) || !IsWorldSpaceInteriorActive(&position))
            return;

        const float radius = g_config.exteriorStreamingRadius;
        const float nativeRadius = GetNativeStreamingRadius();
        if (radius <= nativeRadius + 0.01f)
            return;

        // AddModelsToRequestList can run every frame. Four supplemental passes
        // per second are sufficient for the outer ring; scene loads are never
        // throttled so entry/teleport transitions can fill it immediately.
        const DWORD now = GetTickCount();
        const LONG previous = InterlockedCompareExchange(&g_lastExpandedRequestRunTick, 0, 0);
        if (previous != 0 && (flags & kStreamingLoadingSceneFlag) == 0 &&
            static_cast<DWORD>(now - static_cast<DWORD>(previous)) < 250u)
            return;
        InterlockedExchange(&g_lastExpandedRequestRunTick, static_cast<LONG>(now));

        SectorRange sectors{};
        if (!GetClampedSectorRange(position, radius, sectors))
            return;

        const float minX = position.x - radius;
        const float maxX = position.x + radius;
        const float minY = position.y - radius;
        const float maxY = position.y + radius;

        ScopedStreamingRequestContext requestContext(position, nativeRadius);
        CWorld::AdvanceCurrentScanCode();
        std::array<bool, 16 * 16> visitedRepeatSectors{};

        for (int y = sectors.bottom; y <= sectors.top; ++y) {
            for (int x = sectors.left; x <= sectors.right; ++x) {
                CSector* sector = CWorld::GetSector(x, y);
                if (sector) {
                    Hook_ProcessListRange(&sector->m_buildingList,
                        position.x, position.y, minX, minY, maxX, maxY, radius, flags);
                    Hook_ProcessListRange(&sector->m_dummyList,
                        position.x, position.y, minX, minY, maxX, maxY, radius, flags);
                }

                const unsigned int repeatIndex = static_cast<unsigned int>((y & 15) * 16 + (x & 15));
                if (visitedRepeatSectors[repeatIndex])
                    continue;
                visitedRepeatSectors[repeatIndex] = true;

                CRepeatSector* repeatSector = CWorld::GetRepeatSector(x & 15, y & 15);
                if (repeatSector) {
                    Hook_ProcessListRange(&repeatSector->m_lists[REPEATSECTOR_OBJECTS],
                        position.x, position.y, minX, minY, maxX, maxY, radius, flags);
                }
            }
        }
    }

    // InstanceLoadedModels traverses the game's native outside-visible radius.
    // Models requested by the expanded pass can therefore be loaded in CModelInfo
    // while their world entities still have no RW object. Instantiate only loaded,
    // exterior static entities in the additional nativeRadius..R ring; avoid repeat-sector
    // hash aliases by checking the entity's actual XY distance.
    void InstanceExpandedExteriorRing(const CVector& position, float radius, float nativeRadius) {
        if (!IsFiniteVector(position) || !IsWorldSpaceInteriorActive(&position) ||
            !std::isfinite(radius) || radius <= nativeRadius)
            return;

        // Do not rescan a large sector square every frame. Any model that becomes
        // loaded between passes is picked up on the next pass (at most 100 ms later).
        const DWORD now = GetTickCount();
        const LONG previous = InterlockedCompareExchange(&g_lastExpandedInstanceRunTick, 0, 0);
        if (previous != 0 && static_cast<DWORD>(now - static_cast<DWORD>(previous)) < 100u)
            return;
        InterlockedExchange(&g_lastExpandedInstanceRunTick, static_cast<LONG>(now));

        SectorRange sectors{};
        if (!GetClampedSectorRange(position, radius, sectors))
            return;

        const float minRadiusSqr = nativeRadius * nativeRadius;
        const float maxRadiusSqr = radius * radius;
        std::array<bool, 16 * 16> visitedRepeatSectors{};

        auto tryEntity = [&](CEntity* entity) {
            if (!IsEligibleExteriorStatic(entity) || entity->m_pRwObject != nullptr)
                return;

            const CVector& entityPos = entity->GetPosition();
            const float dx = entityPos.x - position.x;
            const float dy = entityPos.y - position.y;
            const float distanceSqr = dx * dx + dy * dy;
            if (!std::isfinite(distanceSqr) || distanceSqr <= minRadiusSqr || distanceSqr > maxRadiusSqr)
                return;

            if (CStreaming::HasModelLoaded(entity->m_nModelIndex))
                entity->CreateRwObject();
        };

        for (int y = sectors.bottom; y <= sectors.top; ++y) {
            for (int x = sectors.left; x <= sectors.right; ++x) {
                CSector* sector = CWorld::GetSector(x, y);
                if (!sector)
                    continue;

                ScopedAreaBatch::ForEach(&sector->m_buildingList, tryEntity);
                ScopedAreaBatch::ForEach(&sector->m_dummyList, tryEntity);

                const unsigned int repeatIndex = static_cast<unsigned int>((y & 15) * 16 + (x & 15));
                if (visitedRepeatSectors[repeatIndex])
                    continue;
                visitedRepeatSectors[repeatIndex] = true;

                CRepeatSector* repeatSector = CWorld::GetRepeatSector(x & 15, y & 15);
                if (repeatSector)
                    ScopedAreaBatch::ForEach(&repeatSector->m_lists[REPEATSECTOR_OBJECTS], tryEntity);
            }
        }
    }

    void __cdecl Hook_ProcessList(CPtrList* list, unsigned int flags) {
        if (g_streamRequestDepth > 0) {
            ScopedAreaBatch batch(GetStreamingRequestPosition());
            batch.AddList(list);
            g_originalProcessList(list, flags);
            return;
        }
        g_originalProcessList(list, flags);
    }

    void __cdecl Hook_InstanceLoadedModels(const CVector* position) {
        if (!position || !IsWorldSpaceInteriorActive(position)) {
            g_originalInstanceLoadedModels(position);
            return;
        }

        // Temporarily make the outdoor static entities selected by the stock
        // sector traversal eligible to the native instancing routine.
        const float nativeRadius = GetNativeStreamingRadius();
        SectorRange sectors{};
        if (!GetClampedSectorRange(*position, nativeRadius, sectors)) {
            g_originalInstanceLoadedModels(position);
            return;
        }

        {
            ScopedAreaBatch batch(position);
            for (int y = sectors.bottom; y <= sectors.top; ++y) {
                for (int x = sectors.left; x <= sectors.right; ++x)
                    AddExteriorStaticSectorLists(batch, x, y);
            }
            g_originalInstanceLoadedModels(position);
        }

        InstanceExpandedExteriorRing(*position, g_config.exteriorStreamingRadius, nativeRadius);
    }

    void __cdecl Hook_AddModelsToRequestList(const CVector* position, unsigned int flags) {
        if (!position) {
            g_originalAddModelsToRequestList(position, flags);
            return;
        }

        {
            ScopedStreamingRequestContext requestContext(*position);
            g_originalAddModelsToRequestList(position, flags);
        }
        RequestExpandedExteriorModels(*position, flags);
    }

    void __cdecl Hook_SetIplsRequired(const CVector* position, int areaCode) {
        g_originalSetIplsRequired(position, areaCode);

        if (!position || g_loadIplsDepth <= 0 || areaCode == kAreaMainMap ||
            !IsWorldSpaceInteriorActive(position))
            return;

        const CVector camera = CRenderer::ms_vecCameraPosition;
        const float dx = camera.x - position->x;
        const float dy = camera.y - position->y;
        const float cameraDeltaSqr = dx * dx + dy * dy;
        constexpr float kMaxCameraToSceneFocusDelta = 160.0f;
        const bool cameraFinite = std::isfinite(camera.x) && std::isfinite(camera.y) &&
            std::isfinite(camera.z);
        const bool useRequestPosition = !cameraFinite || !std::isfinite(cameraDeltaSqr) ||
            cameraDeltaSqr > kMaxCameraToSceneFocusDelta * kMaxCameraToSceneFocusDelta ||
            camera.z >= kWorldSpaceZCeiling;
        const CVector focusPosition = useRequestPosition ? *position : camera;

        if (!IsFiniteVector(focusPosition))
            return;
        if (g_iplFocusAppliedInCycle)
            return;
        g_iplFocusAppliedInCycle = true;

        // The native IPL requirement function stores its area argument globally.
        // Request exterior IPLs around the current scene, then replay the original
        // request so the caller's area context remains unchanged.
        g_originalSetIplsRequired(&focusPosition, kAreaMainMap);
        g_originalSetIplsRequired(position, areaCode);
    }

    void __cdecl Hook_LoadIpls(CVector position, bool avoidVehicleMovingDirection) {
        ScopedIplLoadCycle loadCycle;
        g_originalLoadIpls(position, avoidVehicleMovingDirection);
    }

}

bool FixInstallHooks() {
    if (g_hooksInstalled)
        return true;

    ConfigLoad();
    if (!ValidateTargetImage())
        return false;

    // This resolves FLA's shared MinHook DLL and accepts
    // MH_ERROR_ALREADY_INITIALIZED from another plugin.
    if (!MinHookRuntime::Initialize())
        return false;

    bool ok = true;
    auto add = [&ok](std::uintptr_t address, auto detour, auto* original) {
        if (!CreateHook(address, detour, original))
            ok = false;
    };

    add(SA10US::kRendererScanSectorListModels,
        &Hook_ScanSectorListModels, &g_originalScanSectorListModels);
    add(SA10US::kRendererScanSectorModelsVisible,
        &Hook_ScanSectorListModelsVisible, &g_originalScanSectorListModelsVisible);
    add(SA10US::kRendererSetupEntityVisibility,
        &Hook_SetupEntityVisibility, &g_originalSetupEntityVisibility);
    add(SA10US::kRendererSetupBigVisibility,
        &Hook_SetupBigBuildingVisibility, &g_originalSetupBigBuildingVisibility);
    add(SA10US::kRendererShouldStream,
        &Hook_ShouldModelBeStreamed, &g_originalShouldModelBeStreamed);

    add(SA10US::kStreamingProcessListRange,
        &Hook_ProcessListRange, &g_originalProcessListRange);
    add(SA10US::kStreamingProcessList,
        &Hook_ProcessList, &g_originalProcessList);
    add(SA10US::kStreamingInstanceLoadedModels,
        &Hook_InstanceLoadedModels, &g_originalInstanceLoadedModels);
    add(SA10US::kStreamingAddModelsToRequestList,
        &Hook_AddModelsToRequestList, &g_originalAddModelsToRequestList);

    add(SA10US::kIplStoreSetIplsRequired,
        &Hook_SetIplsRequired, &g_originalSetIplsRequired);
    add(SA10US::kIplStoreLoadIpls,
        &Hook_LoadIpls, &g_originalLoadIpls);

    if (!ok) {
        RemoveOwnedHooks();
        return false;
    }

    if (!EnableOwnedHooks()) {
        // Roll back only targets created by this plugin. In particular, do
        // not use MH_ALL_HOOKS: FLA and other ASIs share this MinHook instance.
        RemoveOwnedHooks();
        return false;
    }

    g_hooksInstalled = true;
    return true;
}
