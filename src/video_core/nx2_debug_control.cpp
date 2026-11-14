// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <stdexcept>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "video_core/nx2_debug_control.h"

namespace VideoCore::Nx2Debug {
namespace {

struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<CaptureRequest> captures;
    std::unordered_map<u64, CaptureResult> finished;
    std::deque<GpuReadRequest> gpu_reads;
    std::unordered_map<u64, GpuReadResult> gpu_read_results;
    std::vector<u64> rejected;
    u64 reject_generation{};
    u64 applied_generation{};
    bool reject_pending{};
    bool unrejected{};
    bool open{};
    DrawFilter filter;
    u64 filter_generation{};
    u64 filter_applied{};
    bool filter_pending{};
};

State state;
std::atomic<bool> pending{};
std::atomic<bool> any_rejected{};

void UpdatePending() {
    pending.store(!state.captures.empty() || !state.gpu_reads.empty() || state.reject_pending ||
                      state.filter_pending,
                  std::memory_order_relaxed);
}

u64 ParseNumber(std::string_view text, int base) {
    if (base == 16 && (text.starts_with("0x") || text.starts_with("0X"))) {
        text.remove_prefix(2);
    }
    u64 value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        throw std::invalid_argument{"bad number \"" + std::string{text} + "\""};
    }
    return value;
}

} // Anonymous namespace

DrawFilter ParseDrawFilter(std::string_view text) {
    DrawFilter filter;
    while (!text.empty()) {
        const size_t end = (std::min)(text.find(';'), text.size());
        const std::string_view rule = text.substr(0, end);
        text.remove_prefix((std::min)(end + 1, text.size()));
        if (rule.empty()) {
            continue;
        }
        const size_t equals = rule.find('=');
        if (equals == std::string_view::npos) {
            throw std::invalid_argument{"rule \"" + std::string{rule} + "\" has no '='"};
        }
        const std::string_view key = rule.substr(0, equals);
        std::string_view value = rule.substr(equals + 1);
        if (key == "range") {
            const size_t dash = value.find('-');
            if (dash == std::string_view::npos) {
                throw std::invalid_argument{"range wants a-b"};
            }
            filter.range_begin = static_cast<u32>(ParseNumber(value.substr(0, dash), 10));
            filter.range_end = static_cast<u32>(ParseNumber(value.substr(dash + 1), 10));
        } else if (key == "ps" || key == "vs") {
            auto& list = key == "ps" ? filter.ps : filter.vs;
            while (!value.empty()) {
                const size_t comma = (std::min)(value.find(','), value.size());
                list.push_back(ParseNumber(value.substr(0, comma), 16));
                value.remove_prefix((std::min)(comma + 1, value.size()));
            }
        } else if (key == "lines") {
            filter.lines = value != "0";
        } else if (key == "rt0") {
            filter.rt0 = ParseNumber(value, 16);
        } else {
            throw std::invalid_argument{"unknown rule \"" + std::string{key} +
                                        "\": want range, ps, vs, lines or rt0"};
        }
    }
    return filter;
}

u64 SetDrawFilter(DrawFilter filter) {
    std::scoped_lock lock{state.mutex};
    state.filter = std::move(filter);
    state.filter_pending = true;
    UpdatePending();
    return ++state.filter_generation;
}

DrawFilter CurrentDrawFilter() {
    std::scoped_lock lock{state.mutex};
    return state.filter;
}

std::optional<std::pair<u64, DrawFilter>> TakeDrawFilter() {
    std::scoped_lock lock{state.mutex};
    if (!state.filter_pending) {
        return std::nullopt;
    }
    state.filter_pending = false;
    UpdatePending();
    return std::make_pair(state.filter_generation, state.filter);
}

void NoteDrawFilterApplied(u64 generation) {
    {
        std::scoped_lock lock{state.mutex};
        state.filter_applied = (std::max)(state.filter_applied, generation);
    }
    state.changed.notify_all();
}

bool WaitDrawFilterApplied(u64 generation, std::chrono::milliseconds timeout) {
    std::unique_lock lock{state.mutex};
    return state.changed.wait_for(lock, timeout, [generation] {
        return !state.open || state.filter_applied >= generation;
    }) && state.filter_applied >= generation;
}

void BeginSession() {
    std::scoped_lock lock{state.mutex};
    state.captures.clear();
    state.finished.clear();
    state.gpu_reads.clear();
    state.gpu_read_results.clear();
    state.rejected.clear();
    state.reject_pending = false;
    state.unrejected = false;
    state.applied_generation = state.reject_generation;
    state.filter = {};
    state.filter_pending = false;
    state.filter_applied = state.filter_generation;
    state.open = true;
    any_rejected.store(false, std::memory_order_relaxed);
    UpdatePending();
}

void EndSession() {
    {
        std::scoped_lock lock{state.mutex};
        state.open = false;
        state.captures.clear();
        state.gpu_reads.clear();
        UpdatePending();
    }
    state.changed.notify_all();
}

bool HasRequests() noexcept {
    return pending.load(std::memory_order_relaxed);
}

void RequestCapture(CaptureRequest request) {
    std::scoped_lock lock{state.mutex};
    state.captures.push_back(std::move(request));
    UpdatePending();
}

void CancelCapture(u64 id) {
    std::scoped_lock lock{state.mutex};
    std::erase_if(state.captures, [id](const CaptureRequest& request) { return request.id == id; });
    state.finished.erase(id);
    UpdatePending();
}

std::optional<CaptureResult> WaitCapture(u64 id, std::chrono::milliseconds timeout) {
    std::unique_lock lock{state.mutex};
    state.changed.wait_for(lock, timeout, [id] { return !state.open || state.finished.contains(id); });
    const auto it = state.finished.find(id);
    if (it == state.finished.end()) {
        return std::nullopt;
    }
    CaptureResult result = std::move(it->second);
    state.finished.erase(it);
    return result;
}

std::optional<CaptureRequest> TakeCapture() {
    std::scoped_lock lock{state.mutex};
    if (state.captures.empty()) {
        return std::nullopt;
    }
    CaptureRequest request = std::move(state.captures.front());
    state.captures.pop_front();
    UpdatePending();
    return request;
}

void FinishCapture(u64 id, CaptureResult result) {
    {
        std::scoped_lock lock{state.mutex};
        if (!state.open) {
            return;
        }
        state.finished.insert_or_assign(id, std::move(result));
    }
    state.changed.notify_all();
}

void RequestGpuRead(GpuReadRequest request) {
    std::scoped_lock lock{state.mutex};
    state.gpu_reads.push_back(request);
    UpdatePending();
}

void CancelGpuRead(u64 id) {
    std::scoped_lock lock{state.mutex};
    std::erase_if(state.gpu_reads, [id](const GpuReadRequest& request) { return request.id == id; });
    state.gpu_read_results.erase(id);
    UpdatePending();
}

std::optional<GpuReadResult> WaitGpuRead(u64 id, std::chrono::milliseconds timeout) {
    std::unique_lock lock{state.mutex};
    state.changed.wait_for(lock, timeout,
                           [id] { return !state.open || state.gpu_read_results.contains(id); });
    const auto it = state.gpu_read_results.find(id);
    if (it == state.gpu_read_results.end()) {
        return std::nullopt;
    }
    GpuReadResult result = std::move(it->second);
    state.gpu_read_results.erase(it);
    return result;
}

std::optional<GpuReadRequest> TakeGpuRead() {
    std::scoped_lock lock{state.mutex};
    if (state.gpu_reads.empty()) {
        return std::nullopt;
    }
    const GpuReadRequest request = state.gpu_reads.front();
    state.gpu_reads.pop_front();
    UpdatePending();
    return request;
}

void FinishGpuRead(u64 id, GpuReadResult result) {
    {
        std::scoped_lock lock{state.mutex};
        if (!state.open) {
            return;
        }
        state.gpu_read_results.insert_or_assign(id, std::move(result));
    }
    state.changed.notify_all();
}

u64 SetRejected(std::span<const u64> hashes, bool rejected) {
    std::scoped_lock lock{state.mutex};
    bool changed = false;
    for (const u64 hash : hashes) {
        const auto it = std::ranges::find(state.rejected, hash);
        if (rejected && it == state.rejected.end()) {
            state.rejected.push_back(hash);
            changed = true;
        } else if (!rejected && it != state.rejected.end()) {
            state.rejected.erase(it);
            state.unrejected = true;
            changed = true;
        }
    }
    any_rejected.store(!state.rejected.empty(), std::memory_order_relaxed);
    if (changed) {
        ++state.reject_generation;
        state.reject_pending = true;
        UpdatePending();
    }
    return state.reject_generation;
}

std::vector<u64> RejectedHashes() {
    std::scoped_lock lock{state.mutex};
    return state.rejected;
}

bool IsRejected(u64 hash) {
    if (!any_rejected.load(std::memory_order_relaxed)) {
        return false;
    }
    std::scoped_lock lock{state.mutex};
    return std::ranges::find(state.rejected, hash) != state.rejected.end();
}

bool AnyRejected() noexcept {
    return any_rejected.load(std::memory_order_relaxed);
}

std::optional<RejectChange> TakeRejectChange() {
    std::scoped_lock lock{state.mutex};
    if (!state.reject_pending) {
        return std::nullopt;
    }
    const RejectChange change{.generation = state.reject_generation,
                              .unrejected = state.unrejected};
    state.reject_pending = false;
    state.unrejected = false;
    UpdatePending();
    return change;
}

void NoteRejectApplied(u64 generation) {
    {
        std::scoped_lock lock{state.mutex};
        state.applied_generation = (std::max)(state.applied_generation, generation);
    }
    state.changed.notify_all();
}

bool WaitRejectApplied(u64 generation, std::chrono::milliseconds timeout) {
    std::unique_lock lock{state.mutex};
    return state.changed.wait_for(lock, timeout, [generation] {
        return !state.open || state.applied_generation >= generation;
    }) && state.applied_generation >= generation;
}

} // namespace VideoCore::Nx2Debug
