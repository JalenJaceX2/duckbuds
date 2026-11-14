// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "common/logging.h"

namespace VideoCommon::Nx2Perf {

[[nodiscard]] inline bool VerifySwitchSet() {
    const char* value = std::getenv("EDEN_NX2_PERF_VERIFY");
    return value != nullptr && value[0] == '1';
}

/// Every decision EDEN_NX2_PERF_VERIFY=1 turns to its slow (reference) answer, one per site, so
/// EDEN_NX2_PERF_SLOW=<name>[,<name>...] (or "all") can force them one at a time.
enum class Site : std::uint32_t {
    WritePages,             // buffer_cache.h WriteMemory: one-lookup early return
    NativeIndirectUniforms, // buffer_cache.h native bank once per frame (pre-check only)
    CleanWords,             // buffer_cache.h SynchronizeBuffer: walk only dirty tracker words
    GeometrySync,           // buffer_cache.h SynchronizeGeometryBuffer: skip a synced binding
    StorageBindingCache,    // buffer_cache.h StorageBufferBinding: thread-local result cache
    UsageTracker,           // usage_tracker.h Track: fill only around the full run
    TrackerRuns,            // word_manager.h ApplyCollectedRanges: in place, deferred caching
    DeviceSeparated,        // device_memory_manager.inc UpdatePagesCachedBatch: no sort/merge
    MethodFast,             // maxwell_3d.cpp CallMethod: state-register and fast method paths
    MethodCbBatch,          // maxwell_3d.cpp constant buffer word batches
    StateBlocks,            // maxwell_3d.cpp front/back state register blocks
    MappingRuns,            // memory_manager.cpp MaxContinuousRange from map-time runs
    LayoutSize,             // memory_manager.cpp GetMemoryLayoutSize from the snapshot
    SubmappedRange,         // memory_manager.cpp GetSubmapped(Device)Range from runs
    IndexRebind,            // vk_buffer_cache.cpp BindIndexBuffer: skip an identical rebind
    Unswizzle,              // texture_cache/util.cpp worker unswizzle (verify: twice, compare)
    AsyncPipelinesSpirv,    // vk_pipeline_cache.cpp worker SPIR-V (verify: GPU-thread reference)
    InlineDefer,            // vk_rasterizer.cpp inline writes: deferred invalidation (not Verify)
    Count,
};

inline constexpr std::array<std::string_view, static_cast<std::size_t>(Site::Count)> SiteNames{
    "write_pages",   "native_indirect_uniforms", "clean_words",     "geometry_sync",
    "storage_binding_cache", "usage_tracker",    "tracker_runs",    "device_separated",
    "method_fast",   "method_cb_batch",          "state_blocks",    "mapping_runs",
    "layout_size",   "submapped_range",          "index_rebind",    "unswizzle",
    "async_pipelines_spirv", "inline_defer",
};
static_assert([] {
    for (const std::string_view name : SiteNames) {
        if (name.empty()) {
            return false;
        }
    }
    return true;
}(), "every Nx2Perf::Site needs a name");

struct SlowMask {
    std::uint32_t mask = 0;
    std::uint32_t unknown = 0; // names not recognised
};

/// Parses "a,b,c" (spaces allowed, "all" for every site).
[[nodiscard]] constexpr SlowMask ParseSlow(std::string_view text) {
    SlowMask result;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        std::string_view name = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        while (!name.empty() && name.front() == ' ') {
            name.remove_prefix(1);
        }
        while (!name.empty() && (name.back() == ' ' || name.back() == '\r')) {
            name.remove_suffix(1);
        }
        if (name.empty()) {
            continue;
        }
        if (name == "all") {
            result.mask = (1u << SiteNames.size()) - 1;
            continue;
        }
        bool found = false;
        for (std::size_t i = 0; i < SiteNames.size(); ++i) {
            if (SiteNames[i] == name) {
                result.mask |= 1u << i;
                found = true;
            }
        }
        result.unknown += found ? 0 : 1;
    }
    return result;
}
static_assert(ParseSlow("").mask == 0);
static_assert(ParseSlow("clean_words").mask == 1u << static_cast<int>(Site::CleanWords));
static_assert(ParseSlow(" write_pages , inline_defer\r").mask ==
              ((1u << static_cast<int>(Site::WritePages)) |
               (1u << static_cast<int>(Site::InlineDefer))));
static_assert(ParseSlow("all").mask == (1u << static_cast<int>(Site::Count)) - 1);
static_assert(ParseSlow("bogus,,mapping_runs").unknown == 1);
static_assert(ParseSlow("bogus,,mapping_runs").mask == 1u << static_cast<int>(Site::MappingRuns));

[[nodiscard]] inline SlowMask SlowSwitch() {
    const char* value = std::getenv("EDEN_NX2_PERF_SLOW");
    return value == nullptr ? SlowMask{} : ParseSlow(value);
}

/// EDEN_NX2_PERF_VERIFY=1. A plain flag: the buffer cache, the memory tracker and the 3D engine
/// check it on hot paths, and a function's static is checked for its initialization on every
/// call (MSVC reads a thread-local for it).
inline bool g_verify = VerifySwitchSet();
/// EDEN_NX2_PERF_SLOW's sites, a bit each.
inline std::uint32_t g_slow_mask = SlowSwitch().mask;

/// Called by VideoCore::CreateGPU: static initialization runs before the frontend loads
/// nx2_switches.txt, so a switch set only there takes effect from that point on.
inline void ReadSwitch() {
    g_verify = VerifySwitchSet();
    const SlowMask slow = SlowSwitch();
    g_slow_mask = slow.mask;
    if (slow.unknown != 0) {
        LOG_WARNING(HW_GPU, "EDEN_NX2_PERF_SLOW: {} unknown site name(s) in \"{}\"", slow.unknown,
                    std::getenv("EDEN_NX2_PERF_SLOW"));
    }
    std::string sites;
    for (std::size_t i = 0; i < SiteNames.size(); ++i) {
        // inline_defer is EDEN_NX2_INLINE_DEFER=0, which PERF_VERIFY never implied.
        const bool slow_site =
            (g_slow_mask >> i & 1) != 0 ||
            (g_verify && i != static_cast<std::size_t>(Site::InlineDefer));
        sites += sites.empty() ? "" : " ";
        sites += SiteNames[i];
        sites += slow_site ? "=slow" : "=fast";
    }
    LOG_INFO(HW_GPU, "Nx2Perf sites (PERF_VERIFY={}): {}", g_verify ? 1 : 0, sites);
}

inline bool Verify() {
    return g_verify;
}

/// True when this site takes its slow (reference) path: EDEN_NX2_PERF_VERIFY=1, or the site is
/// named in EDEN_NX2_PERF_SLOW.
inline bool Slow(Site site) {
    return g_verify || (g_slow_mask >> static_cast<std::uint32_t>(site) & 1) != 0;
}

/// EDEN_NX2_PERF_SLOW only (for inline_defer, which PERF_VERIFY does not change).
inline bool SlowNamed(Site site) {
    return (g_slow_mask >> static_cast<std::uint32_t>(site) & 1) != 0;
}

} // namespace VideoCommon::Nx2Perf
