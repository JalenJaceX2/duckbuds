// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2_perf.h"

/// The GPU back thread's work outside draw preparation: locks and passes it skips where a check
/// without the lock shows the locked work would change nothing (ported from jpkto/Lichii
/// 7a5e5357cb and f5edeabdbb). Guest CPU write traps hold the buffer cache's lock across page
/// protection calls, so every acquisition the back makes can meet one and spin.
namespace VideoCommon::BackX {

/// EDEN_NX2_BACKX_VERIFY=1: every skip also runs the locked check it replaces, logs
/// "BackX verify mismatch" where that check would have done something, and then does it. The
/// GPU dirty memory managers count their collects (Core::GPUDirtyMemoryManager::count_collects).
inline bool g_verify{};

/// The Duskbloods network test (dusk-v3r experiment, never for nx2): the back micro-cuts of #1586
/// (5e428570d2) and two split-thread register cuts (#1585 blocks, #1095 direct state write) keep
/// upstream behaviour unless EDEN_NX2_DUSK_BACKX=off, which runs them as before those PRs, all
/// but the pieces named in EDEN_NX2_DUSK_BACKX_PIECES=<name>,... (those stay upstream). The old
/// paths did not remove the hub lasers (cd233b9707 test), so upstream is the default again; the
/// debug server's perf_flip backx.<piece> flips each piece live. Other titles: upstream.
enum class DuskPiece : u32 {
    GatherSkip,        // gpu.cpp InvalidateGPUCache: gather only when MayHavePending
    InlineLockFree,    // vk_rasterizer.cpp AccelerateInlineToMemory: skip the lock and InlineMemory
    TrackerReset,      // usage_tracker.h Reset: clear only the words set since the last reset
    InvalidationBatch, // vk_rasterizer.cpp OnCacheInvalidations: each cache's lock once
    VertexInputCache,  // vk_rasterizer.cpp UpdateVertexInput: cached vertex input states
    UboStreak,         // uniform_stream_reuse.h: direct binds after a miss streak
    BreakSkip,         // nx2_fast_draw.h Break: the drawing thread skips repeated serial bumps
    StateBlocks,       // maxwell_3d.cpp: the split front's register blocks (#1585)
    DirectStateWrite,  // maxwell_3d.cpp CallMethod: state registers bypass CallMethodFast (#1095)
    Count,
};
inline constexpr std::array<std::string_view, static_cast<size_t>(DuskPiece::Count)>
    DuskPieceNames{"gather_skip",        "inline_lock_free", "tracker_reset",
                   "invalidation_batch", "vertex_input_cache", "ubo_streak",
                   "break_skip",         "state_blocks",     "direct_state_write"};

/// Pieces that take their old path: a bit each. `mode` is EDEN_NX2_DUSK_BACKX, `pieces`
/// EDEN_NX2_DUSK_BACKX_PIECES (either may be null). Unknown names count in `unknown`.
[[nodiscard]] constexpr u32 DuskOffMask(bool duskbloods, std::string_view mode,
                                        std::string_view pieces, u32* unknown = nullptr) {
    if (!duskbloods || mode != "off") {
        return 0;
    }
    u32 off = (1u << DuskPieceNames.size()) - 1;
    while (!pieces.empty()) {
        const size_t comma = pieces.find(',');
        std::string_view name = pieces.substr(0, comma);
        pieces = comma == std::string_view::npos ? std::string_view{} : pieces.substr(comma + 1);
        while (!name.empty() && name.front() == ' ') {
            name.remove_prefix(1);
        }
        while (!name.empty() && (name.back() == ' ' || name.back() == '\r')) {
            name.remove_suffix(1);
        }
        if (name.empty()) {
            continue;
        }
        bool found = false;
        for (size_t i = 0; i < DuskPieceNames.size(); ++i) {
            if (DuskPieceNames[i] == name) {
                off &= ~(1u << i);
                found = true;
            }
        }
        if (!found && unknown != nullptr) {
            ++*unknown;
        }
    }
    return off;
}
static_assert(DuskOffMask(false, "", "") == 0);
static_assert(DuskOffMask(true, "on", "") == 0);
static_assert(DuskOffMask(true, "", "") == 0);
static_assert(DuskOffMask(true, "off", "") == (1u << 9) - 1);
static_assert(DuskOffMask(true, "off", " state_blocks ,gather_skip\r") ==
              ((1u << 9) - 1 & ~(1u << 7) & ~1u));

/// Nx2Perf::g_dusk_off: set by ReadSwitch (CreateGPU runs after the program id is known, before
/// any GPU thread), then by the debug server's perf_flip at frame ends.
inline bool DuskOff(DuskPiece piece) {
    return (Nx2Perf::g_dusk_off.load(std::memory_order_relaxed) >> static_cast<u32>(piece) & 1) !=
           0;
}

/// "name=old name=upstream ..." for the log.
inline std::string DuskPiecesText(u32 off) {
    std::string text;
    for (size_t i = 0; i < DuskPieceNames.size(); ++i) {
        text += text.empty() ? "" : " ";
        text += DuskPieceNames[i];
        text += (off >> i & 1) != 0 ? "=old" : "=upstream";
    }
    return text;
}

/// EDEN_NX2_DUSK_BACKX / _PIECES for the program `program_id`.
inline u32 DuskOffMaskFromEnv(u64 program_id, u32* unknown = nullptr) {
    const char* const mode = std::getenv("EDEN_NX2_DUSK_BACKX");
    const char* const pieces = std::getenv("EDEN_NX2_DUSK_BACKX_PIECES");
    return DuskOffMask(program_id == 0x0400D1D02399A000ULL, mode ? mode : "",
                       pieces ? pieces : "", unknown);
}

/// Called by VideoCore::CreateGPU, before the caches exist.
inline void ReadSwitch(u64 program_id) {
    const char* const value = std::getenv("EDEN_NX2_BACKX_VERIFY");
    g_verify = value != nullptr && value[0] == '1';
    if (g_verify) {
        LOG_INFO(HW_GPU, "BackX verify on");
    }
    u32 unknown = 0;
    const u32 off = DuskOffMaskFromEnv(program_id, &unknown);
    Nx2Perf::g_dusk_off.store(off, std::memory_order_relaxed);
    if (unknown != 0) {
        LOG_WARNING(HW_GPU, "EDEN_NX2_DUSK_BACKX_PIECES: {} unknown piece name(s)", unknown);
    }
    if (program_id == 0x0400D1D02399A000ULL) {
        LOG_INFO(HW_GPU, "Duskbloods back pieces: {}", DuskPiecesText(off));
    }
}

/// Verify: a skip the locked check contradicts (logged 64 times at most).
inline void Mismatch(const char* what, u64 address, u64 size) {
    static std::atomic<u32> logged{};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 64) {
        LOG_ERROR(HW_GPU, "BackX verify mismatch: {} at {:#x}+{:#x}", what, address, size);
    }
}

/// Verify of a gather InvalidateGPUCache skipped: `found` when the gather found writes,
/// `collected` when a manager collected one after the skip's check (a race the next call covers
/// either way). Writes found with nothing collected since are a miss. Logs the counts every 10 s.
inline void SkippedGather(bool found, bool collected, std::pair<u64, u64> first_write) {
    static std::mutex mutex;
    static u64 skips{};
    static u64 races{};
    static u64 misses{};
    static auto last_log = std::chrono::steady_clock::now();
    std::scoped_lock lock{mutex};
    ++skips;
    if (found && collected) {
        ++races;
    } else if (found) {
        ++misses;
        Mismatch("collected CPU write while no manager showed one", first_write.first,
                 first_write.second);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log < std::chrono::seconds{10}) {
        return;
    }
    LOG_INFO(HW_GPU,
             "BackX gather checks in 10 s: {} skipped gathers, {} found writes collected after "
             "the check, {} found writes no manager showed (must stay 0)",
             skips, races, misses);
    skips = races = misses = 0;
    last_log = now;
}

} // namespace VideoCommon::BackX
