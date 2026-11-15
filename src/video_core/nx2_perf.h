// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
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

/// The sites PERF_VERIFY=1 turns slow: all but inline_defer (EDEN_NX2_INLINE_DEFER=0).
inline constexpr std::uint32_t VerifyMask =
    ((1u << SiteNames.size()) - 1) & ~(1u << static_cast<std::uint32_t>(Site::InlineDefer));

/// Texture alias controls for the Duskbloods exposure flicker (live through perf_flip, start
/// switches EDEN_NX2_<NAME upper case>=1; all off is the normal behaviour).
enum class Alias : std::uint32_t {
    SyncAlways,    ///< unused bit: alias_sync_always is the tri-state LiveState::alias_sync
    FullResolve,   ///< fast draw delta draws take the full path when a sampled image has aliases
    ResyncUpload,  ///< an upload of an aliased image copies GPU-written aliases of the same tick
    Diag,          ///< per-second counts of alias work on images of at most 16x16
    Count,
};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(Alias::Count)> AliasNames{
    "alias_sync_always", "alias_full_resolve", "alias_resync_upload", "alias_diag",
};

/// The live state. Start: the switches (ReadSwitch). Then the debug server's perf_slow and
/// perf_flip (docs/nx2/debug-mcp.md), applied by RasterizerVulkan::TickFrame at a frame end.
/// Every site reads it with one relaxed atomic load where it decides, and each decision is per
/// call (the fast and the slow path of a site leave the same state), so a flip is safe wherever
/// another thread is; the frame end only gives the caches a reset point (ApplyLive's caller).
struct LiveState {
    bool verify{};              ///< EDEN_NX2_PERF_VERIFY
    std::uint32_t named{};      ///< EDEN_NX2_PERF_SLOW sites (+ inline_defer for INLINE_DEFER=0)
    std::uint32_t dusk_off{};   ///< BackX::DuskPiece bits on their old path
    int uboreuse = -1;          ///< -1: the start switch, else 0/1 (EDEN_NX2_UBOREUSE)
    int frames_ahead = -1;      ///< -1: the start switch, else 0/1 (EDEN_NX2_FRAMES_AHEAD)
    int fence_batch = -1;       ///< -1: the start switch, else 0/1 (EDEN_NX2_FENCE_BATCH)
    int sync_submit = -1;       ///< -1: the start switch, else 0/1 (EDEN_NX2_SYNC_SUBMIT)
    std::uint32_t alias{};      ///< Alias bits
    /// SynchronizeAliases walks the aliases even when no alias tick is newer: -1 (default) for
    /// The Duskbloods' images of at most 16x16, 0 never, 1 always (EDEN_NX2_ALIAS_SYNC_ALWAYS)
    int alias_sync = -1;

    [[nodiscard]] std::uint32_t Effective() const noexcept {
        return named | (verify ? VerifyMask : 0);
    }
};

/// Sites on their slow path (verify folded in): what Slow() reads.
inline std::atomic<std::uint32_t> g_slow{};
inline std::atomic<bool> g_verify{};
inline std::atomic<std::uint32_t> g_named{};
inline std::atomic<std::uint32_t> g_dusk_off{};
inline std::atomic<int> g_uboreuse{-1};
inline std::atomic<int> g_frames_ahead{-1};
inline std::atomic<int> g_fence_batch{-1};
inline std::atomic<int> g_sync_submit{-1};
inline std::atomic<std::uint32_t> g_alias{};
inline std::atomic<int> g_alias_sync{-1};
/// Live changes requested / applied, and the rasterizer frame of the last applied one.
inline std::atomic<std::uint64_t> g_live_requested{};
inline std::atomic<std::uint64_t> g_live_applied{};
inline std::atomic<std::uint64_t> g_live_applied_frame{};
inline std::mutex g_live_mutex;
inline LiveState g_live_pending;
/// The last "DLSS stats:" line (NVIDIA DLSS through Streamline), for the debug server's status;
/// empty until DLSS has run.
inline std::mutex g_dlss_stats_mutex;
inline std::string g_dlss_stats;

inline void StoreLive(const LiveState& state) {
    g_verify.store(state.verify, std::memory_order_relaxed);
    g_named.store(state.named, std::memory_order_relaxed);
    g_slow.store(state.Effective(), std::memory_order_relaxed);
    g_dusk_off.store(state.dusk_off, std::memory_order_relaxed);
    g_uboreuse.store(state.uboreuse, std::memory_order_relaxed);
    g_frames_ahead.store(state.frames_ahead, std::memory_order_relaxed);
    g_fence_batch.store(state.fence_batch, std::memory_order_relaxed);
    g_sync_submit.store(state.sync_submit, std::memory_order_relaxed);
    g_alias.store(state.alias, std::memory_order_relaxed);
    g_alias_sync.store(state.alias_sync, std::memory_order_relaxed);
}

[[nodiscard]] inline LiveState CurrentLive() {
    return LiveState{
        .verify = g_verify.load(std::memory_order_relaxed),
        .named = g_named.load(std::memory_order_relaxed),
        .dusk_off = g_dusk_off.load(std::memory_order_relaxed),
        .uboreuse = g_uboreuse.load(std::memory_order_relaxed),
        .frames_ahead = g_frames_ahead.load(std::memory_order_relaxed),
        .fence_batch = g_fence_batch.load(std::memory_order_relaxed),
        .sync_submit = g_sync_submit.load(std::memory_order_relaxed),
        .alias = g_alias.load(std::memory_order_relaxed),
        .alias_sync = g_alias_sync.load(std::memory_order_relaxed),
    };
}

/// The state the next frame end applies (the pending one, else the live one).
[[nodiscard]] inline LiveState TargetLive() {
    std::scoped_lock lock{g_live_mutex};
    return g_live_requested.load(std::memory_order_relaxed) !=
                   g_live_applied.load(std::memory_order_relaxed)
               ? g_live_pending
               : CurrentLive();
}

/// Debug server thread: the next frame end applies `state`. Returns the request to wait for.
inline std::uint64_t RequestLive(const LiveState& state) {
    std::scoped_lock lock{g_live_mutex};
    g_live_pending = state;
    return g_live_requested.fetch_add(1, std::memory_order_release) + 1;
}

/// "name=slow name=fast ..." of an effective mask.
[[nodiscard]] inline std::string SitesText(std::uint32_t slow) {
    std::string sites;
    for (std::size_t i = 0; i < SiteNames.size(); ++i) {
        sites += sites.empty() ? "" : " ";
        sites += SiteNames[i];
        sites += (slow >> i & 1) != 0 ? "=slow" : "=fast";
    }
    return sites;
}

/// Rasterizer thread, at a frame end: applies a requested change. True when one was applied, so
/// the caller resets the caches the flipped paths fill differently.
inline bool ApplyLive(std::uint64_t frame) {
    const std::uint64_t requested = g_live_requested.load(std::memory_order_acquire);
    if (requested == g_live_applied.load(std::memory_order_relaxed)) {
        return false;
    }
    LiveState state;
    {
        std::scoped_lock lock{g_live_mutex};
        state = g_live_pending;
    }
    StoreLive(state);
    g_live_applied_frame.store(frame, std::memory_order_relaxed);
    g_live_applied.store(requested, std::memory_order_release);
    LOG_INFO(HW_GPU,
             "Nx2Perf live change #{} at frame {}: verify={} dusk_off={:#x} uboreuse={} "
             "frames_ahead={} fence_batch={} sync_submit={} alias={:#x} alias_sync={} sites: {}",
             requested, frame, state.verify ? 1 : 0, state.dusk_off, state.uboreuse,
             state.frames_ahead, state.fence_batch, state.sync_submit, state.alias,
             state.alias_sync, SitesText(state.Effective()));
    return true;
}

/// Called by VideoCore::CreateGPU: static initialization runs before the frontend loads
/// nx2_switches.txt, so a switch set only there takes effect from that point on.
inline void ReadSwitch() {
    const SlowMask slow = SlowSwitch();
    if (slow.unknown != 0) {
        LOG_WARNING(HW_GPU, "EDEN_NX2_PERF_SLOW: {} unknown site name(s) in \"{}\"", slow.unknown,
                    std::getenv("EDEN_NX2_PERF_SLOW"));
    }
    LiveState state = CurrentLive();
    state.verify = VerifySwitchSet();
    state.named = slow.mask;
    if (const char* defer = std::getenv("EDEN_NX2_INLINE_DEFER"); defer && defer[0] == '0') {
        state.named |= 1u << static_cast<std::uint32_t>(Site::InlineDefer);
    }
    for (std::size_t i = 0; i < AliasNames.size(); ++i) {
        std::string name = "EDEN_NX2_";
        for (const char c : AliasNames[i]) {
            name += static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        }
        if (const char* value = std::getenv(name.c_str()); value && value[0] == '1') {
            state.alias |= 1u << i;
        }
    }
    if (const char* value = std::getenv("EDEN_NX2_ALIAS_SYNC_ALWAYS"); value && *value) {
        state.alias_sync = value[0] == '1' ? 1 : 0;
    }
    StoreLive(state);
    LOG_INFO(HW_GPU, "Nx2Perf sites (PERF_VERIFY={}, alias={:#x}, alias_sync={}): {}",
             state.verify ? 1 : 0, state.alias, state.alias_sync, SitesText(state.Effective()));
}

inline bool Verify() {
    return g_verify.load(std::memory_order_relaxed);
}

/// True when this site takes its slow (reference) path: EDEN_NX2_PERF_VERIFY=1, the site is
/// named in EDEN_NX2_PERF_SLOW, or the debug server's perf_slow turned it slow.
inline bool Slow(Site site) {
    return (g_slow.load(std::memory_order_relaxed) >> static_cast<std::uint32_t>(site) & 1) != 0;
}

/// inline_defer, which PERF_VERIFY does not change (VerifyMask leaves its bit out).
inline bool SlowNamed(Site site) {
    return Slow(site);
}

/// A live override (-1: none) of a start-time boolean switch.
[[nodiscard]] inline bool LiveOr(const std::atomic<int>& live, bool start) {
    const int value = live.load(std::memory_order_relaxed);
    return value < 0 ? start : value != 0;
}

[[nodiscard]] inline int AliasSync() {
    return g_alias_sync.load(std::memory_order_relaxed);
}

[[nodiscard]] inline bool AliasOn(Alias control) {
    return (g_alias.load(std::memory_order_relaxed) >> static_cast<std::uint32_t>(control) & 1) != 0;
}

/// alias_diag counts (texture cache thread), images of at most 16x16 only.
enum class AliasStat : std::uint32_t {
    Skipped,     ///< SynchronizeAliases returned at the newest_alias_tick check
    NoNewer,     ///< walked, no alias newer
    Synced,      ///< walked and copied (one per image sync)
    Uploads,     ///< uploads from guest memory of an image with aliases
    CreatedAlias,///< created with at least one alias
    CreatedBad,  ///< created with a bad overlap and no alias
    Count,
};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(AliasStat::Count)>
    AliasStatNames{"skipped", "no_newer", "synced", "uploads", "created_alias", "created_bad"};
inline std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(AliasStat::Count)>
    g_alias_stats{};
/// The last whole second's counts (perf_sites "alias_stats").
inline std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(AliasStat::Count)>
    g_alias_stats_last{};

inline void NoteAlias(AliasStat stat, std::uint32_t width, std::uint32_t height) {
    if (width <= 16 && height <= 16 && AliasOn(Alias::Diag)) {
        g_alias_stats[static_cast<std::size_t>(stat)].fetch_add(1, std::memory_order_relaxed);
    }
}

/// Rasterizer thread, each frame end: at most once a second, logs and keeps the counts.
inline void FlushAliasStats(std::uint64_t now_ms) {
    static std::uint64_t last_ms{};
    if (!AliasOn(Alias::Diag) || now_ms - last_ms < 1000) {
        return;
    }
    last_ms = now_ms;
    std::string text;
    for (std::size_t i = 0; i < g_alias_stats.size(); ++i) {
        const std::uint64_t count = g_alias_stats[i].exchange(0, std::memory_order_relaxed);
        g_alias_stats_last[i].store(count, std::memory_order_relaxed);
        text += (text.empty() ? "" : " ") + std::string{AliasStatNames[i]} + "=" +
                std::to_string(count);
    }
    LOG_INFO(HW_GPU, "Alias sync (images <= 16x16, last second): {}", text);
}

} // namespace VideoCommon::Nx2Perf
