// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/common_types.h"

/// Requests from the debug server (EDEN_NX2_DEBUG_SERVER, docs/nx2/debug-mcp.md) to the thread
/// that owns the rasterizer. The server posts them from its own thread; RasterizerVulkan takes
/// them at the end of each presented frame, so every cache they touch is used from its owner.
/// Without the server nothing is posted and the per-frame cost is one relaxed atomic load.
namespace VideoCore::Nx2Debug {

struct CaptureRequest {
    u64 id;
    std::filesystem::path directory;
    bool sampled;
    /// The uniform and storage buffers of every draw and dispatch, and dispatch group counts.
    bool buffers;
};

struct CaptureResult {
    std::string error;
};

struct GpuReadRequest {
    u64 id;
    u64 gpu_address;
    u64 size;
};

struct GpuReadResult {
    std::vector<u8> bytes;
    /// Tegra::MemoryManager::GetID of the address space that maps the whole range.
    u64 address_space;
    std::string error;
};

struct RejectChange {
    u64 generation;
    /// Whether a hash left the list, so programs rejected before must be lowered again.
    bool unrejected;
};

/// Graphics draws the rasterizer leaves out (draw_skip, EDEN_NX2_DRAW_SKIP). A draw is skipped
/// when any rule matches. Only the draw call itself is left out: pipeline, bindings, render pass
/// and dynamic state are still set up, so later draws see the same state as without the filter.
struct DrawFilter {
    /// Draw indices within the frame (counted from 0 at each frame end): [range_begin, range_end).
    u32 range_begin{};
    u32 range_end{};
    /// Shader hashes (frame dump "shaders", ShaderHashes()[1] and [5]) of the vertex and pixel
    /// programs.
    std::vector<u64> vs;
    std::vector<u64> ps;
    /// Line, line loop, line strip and line adjacency topologies.
    bool lines{};
    /// Colour target 0 at this GPU address (0: no rule).
    u64 rt0{};

    [[nodiscard]] bool Empty() const noexcept {
        return range_begin >= range_end && vs.empty() && ps.empty() && !lines && rt0 == 0;
    }
};

/// Parses "range=a-b;ps=<hash>,...;vs=<hash>,...;lines=1;rt0=0x..." (EDEN_NX2_DRAW_SKIP). Throws
/// std::invalid_argument on a malformed rule.
[[nodiscard]] DrawFilter ParseDrawFilter(std::string_view text);

/// Server thread: replaces the filter; the rasterizer takes it at the next frame end. Returns the
/// generation to wait for.
u64 SetDrawFilter(DrawFilter filter);
[[nodiscard]] DrawFilter CurrentDrawFilter();
/// Rasterizer thread: the pending filter change, once, with its generation.
[[nodiscard]] std::optional<std::pair<u64, DrawFilter>> TakeDrawFilter();
void NoteDrawFilterApplied(u64 generation);
[[nodiscard]] bool WaitDrawFilterApplied(u64 generation, std::chrono::milliseconds timeout);

/// Opens a debug server session (clears requests and rejections) or ends it (wakes waiters).
void BeginSession();
void EndSession();

/// Rasterizer thread: whether a capture or a reject change is waiting.
[[nodiscard]] bool HasRequests() noexcept;

/// Server thread: queues a frame capture; the rasterizer takes it at the next frame end.
void RequestCapture(CaptureRequest request);
/// Server thread: withdraws a capture that was not taken yet (it timed out).
void CancelCapture(u64 id);
/// Server thread: waits for the capture's manifest to be complete. Empty on timeout or when the
/// session ends.
[[nodiscard]] std::optional<CaptureResult> WaitCapture(u64 id, std::chrono::milliseconds timeout);
/// Rasterizer thread.
[[nodiscard]] std::optional<CaptureRequest> TakeCapture();
/// Frame dump writer thread, once the manifest summary is written (or the capture failed).
void FinishCapture(u64 id, CaptureResult result);

/// Server thread: queues a read of GPU virtual memory; the rasterizer takes it at the next frame
/// end, where the GPU's own reads of its address spaces run.
void RequestGpuRead(GpuReadRequest request);
/// Server thread: withdraws a read that was not taken yet (it timed out).
void CancelGpuRead(u64 id);
/// Server thread: empty on timeout or when the session ends.
[[nodiscard]] std::optional<GpuReadResult> WaitGpuRead(u64 id, std::chrono::milliseconds timeout);
/// Rasterizer thread.
[[nodiscard]] std::optional<GpuReadRequest> TakeGpuRead();
/// Rasterizer thread.
void FinishGpuRead(u64 id, GpuReadResult result);

/// Server thread: adds or removes native program keys or shader hashes (as EDEN_NX2_REJECT_HASH
/// takes them). Returns the generation to wait for.
u64 SetRejected(std::span<const u64> hashes, bool rejected);
[[nodiscard]] std::vector<u64> RejectedHashes();
/// Lowering: whether the debug server rejects this program. Cheap while the list is empty.
[[nodiscard]] bool IsRejected(u64 hash);
[[nodiscard]] bool AnyRejected() noexcept;
/// Rasterizer thread: the pending reject change, once.
[[nodiscard]] std::optional<RejectChange> TakeRejectChange();
/// Rasterizer thread: the cached programs of this generation were dropped.
void NoteRejectApplied(u64 generation);
/// Server thread: waits until the rasterizer applied the generation.
[[nodiscard]] bool WaitRejectApplied(u64 generation, std::chrono::milliseconds timeout);

} // namespace VideoCore::Nx2Debug
