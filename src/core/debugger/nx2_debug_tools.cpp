// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <numeric>
#include <thread>

#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "common/logging.h"
#include "common/nx2_sampler.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/stb.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/debugger/nx2_debug_server.h"
#include "core/debugger/nx2_debug_tools.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "hid_core/hid_core.h"
#include "video_core/gpu.h"
#include "video_core/nx2_backx.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_perf.h"
#include "video_core/renderer_base.h"

namespace Core::Nx2Debug {

using nlohmann::json;
using namespace std::chrono_literals;

/// Lines the log ring keeps for log_tail.
constexpr size_t RING_LINES = 20000;
/// Bytes one read_memory or read_gpu_memory call returns at most.
constexpr u64 MAX_READ = 64 * 1024;
/// Presented frames one input or input_sequence call holds at most (10 minutes at 60 fps).
constexpr u64 MAX_INPUT_FRAMES = 36000;
constexpr size_t MAX_INPUT_STEPS = 1000;

constexpr std::array<std::pair<std::string_view, HID::NpadButton>, 16> INPUT_BUTTONS{{
    {"A", HID::NpadButton::A},         {"B", HID::NpadButton::B},
    {"X", HID::NpadButton::X},         {"Y", HID::NpadButton::Y},
    {"L", HID::NpadButton::L},         {"R", HID::NpadButton::R},
    {"ZL", HID::NpadButton::ZL},       {"ZR", HID::NpadButton::ZR},
    {"Plus", HID::NpadButton::Plus},   {"Minus", HID::NpadButton::Minus},
    {"Up", HID::NpadButton::Up},       {"Down", HID::NpadButton::Down},
    {"Left", HID::NpadButton::Left},   {"Right", HID::NpadButton::Right},
    {"StickL", HID::NpadButton::StickL}, {"StickR", HID::NpadButton::StickR},
}};

struct ToolState {
    explicit ToolState(System& system_, std::string title_, u64 program_id_,
                       std::filesystem::path dump_directory_)
        : system{system_}, title{std::move(title_)}, program_id{program_id_},
          dump_directory{std::move(dump_directory_)} {}

    System& system;
    std::string title;
    u64 program_id;
    std::filesystem::path dump_directory;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    /// Names this session's captures apart from earlier ones in the same dump directory.
    s64 session{std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()};
    std::string log_filter{Settings::values.log_filter.GetValue()};
    u64 captures{};
    u64 screenshots{};
    u64 samples{};
    u64 gpu_reads{};
    u64 draw_lists{};
    std::atomic<bool> stopping{};
};

namespace {

std::string Hex(u64 value) {
    return fmt::format("{:016x}", value);
}

json HexList(const std::vector<u64>& values) {
    json list = json::array();
    for (const u64 value : values) {
        list.push_back(Hex(value));
    }
    return list;
}

template <typename T>
T Param(const json& params, const char* name, T fallback, T low, T high) {
    const T value = params.value(name, fallback);
    return (std::clamp)(value, low, high);
}

std::string HexAddress(u64 value) {
    return fmt::format("{:#x}", value);
}

/// An address as a JSON number or a hex string (0x optional).
u64 AddressParam(const json& params, const char* name) {
    const json& value = params.at(name);
    if (value.is_number_unsigned()) {
        return value.get<u64>();
    }
    if (!value.is_string()) {
        throw std::runtime_error{fmt::format("{} must be a hex string or a non-negative number", name)};
    }
    const std::string text = value.get<std::string>();
    const std::string_view view{text};
    const std::string_view digits =
        view.starts_with("0x") || view.starts_with("0X") ? view.substr(2) : view;
    u64 address = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), address, 16);
    if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size()) {
        throw std::runtime_error{fmt::format("bad {} \"{}\": want hex, e.g. 0x8012345000", name, text)};
    }
    return address;
}

/// The size of a read: 1 to MAX_READ bytes, not wrapping past the end of the address space.
u64 ReadSize(const json& params, u64 address) {
    const json& value = params.at("size");
    if (!value.is_number_integer() || value.get<s64>() < 1 || value.get<u64>() > MAX_READ) {
        throw std::runtime_error{fmt::format("size must be 1 to {} bytes", MAX_READ)};
    }
    const u64 size = value.get<u64>();
    if (address + size < address) {
        throw std::runtime_error{"address + size wraps around"};
    }
    return size;
}

Kernel::KProcess& Application(ToolState& state) {
    Kernel::KProcess* const process = state.system.ApplicationProcess();
    if (process == nullptr) {
        throw std::runtime_error{"no application process is running"};
    }
    return *process;
}

/// A host pointer to the byte at address, or null when its page is not mapped. GetPointerSilent
/// reads the page entry as guest cores do, with no commit, log or GPU download; it masks the top
/// 16 bits, so addresses there would alias and are unmapped instead.
const u8* GuestByte(Memory::Memory& memory, u64 address) {
    return (address >> 48) == 0 ? memory.GetPointerSilent(address) : nullptr;
}

bool IsVulkan() {
    return Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan;
}

/// Waits for a future in short slices so a stopping server does not wait out the timeout.
template <typename T>
bool WaitReady(const ToolState& state, std::future<T>& future, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (future.wait_for(50ms) != std::future_status::ready) {
        if (state.stopping.load(std::memory_order_relaxed) ||
            std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
    }
    return true;
}

void AppendPng(void* context, void* data, int size) {
    auto& png = *static_cast<std::vector<u8>*>(context);
    const u8* const bytes = static_cast<const u8*>(data);
    png.insert(png.end(), bytes, bytes + size);
}

json Status(ToolState& state, const json& params) {
    const auto window = std::chrono::milliseconds{Param<int>(params, "fps_window_ms", 500, 0, 5000)};
    const u64 before = g_nx2_present_count.load(std::memory_order_relaxed);
    const auto begin = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - begin < window &&
           !state.stopping.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(20ms);
    }
    const u64 after = g_nx2_present_count.load(std::memory_order_relaxed);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    return {
        {"title", state.title},
        {"program_id", fmt::format("{:016X}", state.program_id)},
        {"build", std::string{Common::g_build_fullname}},
        {"scm_rev", std::string{Common::g_scm_rev}},
        {"switch2", Settings::CurrentProgramIsSwitch2()},
        {"renderer", IsVulkan() ? "vulkan" : "other"},
        {"powered_on", state.system.IsPoweredOn()},
        {"paused", state.system.IsPaused()},
        {"presents", after},
        {"fps", seconds > 0.0 ? static_cast<double>(after - before) / seconds : 0.0},
        {"uptime_s", std::chrono::duration<double>(std::chrono::steady_clock::now() - state.start)
                         .count()},
        {"dump_dir", state.dump_directory.generic_string()},
        {"log_filter", state.log_filter},
        {"rejected", HexList(VideoCore::Nx2Debug::RejectedHashes())},
        {"dlss", [] {
             std::scoped_lock lock{VideoCommon::Nx2Perf::g_dlss_stats_mutex};
             return VideoCommon::Nx2Perf::g_dlss_stats;
         }()},
    };
}

json FrameTimes(ToolState& state, const json& params) {
    const double seconds = params.value("seconds", 5.0);
    const double refresh_hz = params.value("refresh_hz", 60.0);
    if (!std::isfinite(seconds) || seconds <= 0.0 || seconds > 60.0 ||
        !std::isfinite(refresh_hz) || refresh_hz <= 0.0) {
        throw std::runtime_error{"seconds must be > 0 and <= 60; refresh_hz must be > 0"};
    }
    const u64 before = g_nx2_present_count.load(std::memory_order_relaxed);
    const auto begin = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() <
               seconds &&
           !state.stopping.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(20ms);
    }
    const auto end = std::chrono::steady_clock::now();
    const u64 after = g_nx2_present_count.load(std::memory_order_relaxed);
    const u64 begin_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(begin.time_since_epoch()).count();
    const u64 end_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count();
    return ReadFrameTimes(before, after, begin_ns, end_ns, refresh_hz);
}

} // Anonymous namespace

json ReadFrameTimes(u64 before, u64 after, u64 begin_ns, u64 end_ns, double refresh_hz) {
    const u64 capacity = g_nx2_present_times_ns.size();
    bool truncated = after < before || after - before > capacity;
    const u64 first = (std::max)(before + 1, after > capacity ? after - capacity + 1 : u64{1});
    std::vector<double> intervals;
    u64 previous = 0;
    for (u64 frame = after; frame >= first; --frame) {
        const u64 timestamp =
            g_nx2_present_times_ns[frame % capacity].load(std::memory_order_relaxed);
        // The counter advances before its store; the slot can also be overwritten during a read.
        if (timestamp < begin_ns || timestamp > end_ns) {
            truncated |= timestamp > end_ns;
            previous = 0;
            continue;
        }
        if (previous != 0 && timestamp > previous) {
            intervals.clear();
        } else if (previous != 0) {
            intervals.push_back(static_cast<double>(previous - timestamp) / 1'000'000.0);
        }
        previous = timestamp;
    }

    std::ranges::reverse(intervals);
    json recent = json::array();
    for (size_t i = intervals.size() > 120 ? intervals.size() - 120 : 0; i < intervals.size(); ++i) {
        recent.push_back(std::round(intervals[i] * 10.0) / 10.0);
    }
    std::ranges::sort(intervals);
    const size_t frames = intervals.size();
    const auto percentile = [&](double fraction) {
        if (frames == 0) {
            return 0.0;
        }
        const double index = fraction * static_cast<double>(frames - 1);
        const size_t low = static_cast<size_t>(index);
        const size_t high = (std::min)(low + 1, frames - 1);
        return intervals[low] +
               (intervals[high] - intervals[low]) * (index - static_cast<double>(low));
    };
    const double mean = frames == 0 ? 0.0 : std::accumulate(intervals.begin(), intervals.end(), 0.0) /
                                              static_cast<double>(frames);
    const double median = percentile(0.5);
    std::array<size_t, 4> buckets{};
    size_t stutters = 0;
    for (const double interval : intervals) {
        const double periods = interval * refresh_hz / 1000.0;
        ++buckets[periods < 1.5 ? 0 : periods < 2.5 ? 1 : periods < 3.5 ? 2 : 3];
        stutters += interval > 2.0 * median;
    }
    const auto share = [&](size_t bucket) {
        return frames == 0 ? 0.0 : static_cast<double>(buckets[bucket]) / static_cast<double>(frames);
    };
    return {{"frames", frames},
            {"mean_ms", mean},
            {"fps", mean > 0.0 ? 1000.0 / mean : 0.0},
            {"p50_ms", median},
            {"p90_ms", percentile(0.9)},
            {"p99_ms", percentile(0.99)},
            {"max_ms", frames == 0 ? 0.0 : intervals.back()},
            {"min_ms", frames == 0 ? 0.0 : intervals.front()},
            {"vsync_buckets", {{"1", share(0)}, {"2", share(1)}, {"3", share(2)}, {"4+", share(3)}}},
            {"stutters", stutters},
            {"intervals_ms", std::move(recent)},
            {"truncated", truncated}};
}

namespace {

json Screenshot(ToolState& state, const json& params) {
    auto& renderer = state.system.GPU().Renderer();
    if (renderer.IsScreenshotPending()) {
        throw std::runtime_error{"a screenshot is already pending: no frame was presented since"};
    }
    const u32 height = Param<u32>(params, "height", 720, 144, 2160);
    const auto layout = Layout::DefaultFrameLayout(height * 16 / 9, height);
    const auto pixels = std::make_shared<std::vector<u8>>(size_t{layout.width} * layout.height * 4);
    const auto done = std::make_shared<std::promise<bool>>();
    auto future = done->get_future();
    renderer.RequestScreenshot(
        pixels->data(), [pixels, done](bool invert_y) { done->set_value(invert_y); }, layout);
    const int timeout = Param<int>(params, "timeout_s", 5, 1, 30);
    if (!WaitReady(state, future, std::chrono::seconds{timeout})) {
        throw std::runtime_error{
            fmt::format("no frame was presented within {} s (paused or loading?)", timeout)};
    }
    const bool invert_y = future.get();
    std::vector<u8> rgb(size_t{layout.width} * layout.height * 3);
    for (u32 y = 0; y < layout.height; ++y) {
        const u8* source = pixels->data() + size_t{invert_y ? layout.height - 1 - y : y} * layout.width * 4;
        u8* target = rgb.data() + size_t{y} * layout.width * 3;
        for (u32 x = 0; x < layout.width; ++x, source += 4, target += 3) {
            target[0] = source[2];
            target[1] = source[1];
            target[2] = source[0];
        }
    }
    std::vector<u8> png;
    if (!stbi_write_png_to_func(AppendPng, &png, static_cast<int>(layout.width),
                                static_cast<int>(layout.height), 3, rgb.data(),
                                static_cast<int>(layout.width * 3))) {
        throw std::runtime_error{"PNG encoding failed"};
    }
    json result = {{"width", layout.width}, {"height", layout.height}};
    if (params.value("save", false)) {
        std::filesystem::create_directories(state.dump_directory);
        const auto path =
            state.dump_directory / fmt::format("screenshot-{}-{:04}.png", state.session, ++state.screenshots);
        std::ofstream file{path, std::ios::binary};
        file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!file) {
            throw std::runtime_error{fmt::format("cannot write {}", path.generic_string())};
        }
        result["path"] = path.generic_string();
    }
    if (params.value("inline", true)) {
        result["png_base64"] = Base64(png);
    }
    return result;
}

json LogTail(ToolState&, const json& params) {
    const u64 after = params.value("after", u64{0});
    const size_t limit = Param<size_t>(params, "limit", 200, 1, 5000);
    const std::string contains = params.value("contains", std::string{});
    const std::vector<Common::Log::RingLine> lines = Common::Log::ReadRing(after);
    std::vector<const Common::Log::RingLine*> matched;
    for (const auto& line : lines) {
        if (contains.empty() || line.text.find(contains) != std::string::npos) {
            matched.push_back(&line);
        }
    }
    // From the start the newest lines; after a sequence number the oldest ones, to page forward.
    const bool more = matched.size() > limit;
    if (more) {
        if (after == 0) {
            matched.erase(matched.begin(), matched.end() - static_cast<std::ptrdiff_t>(limit));
        } else {
            matched.resize(limit);
        }
    }
    json text = json::array();
    for (const auto* line : matched) {
        text.push_back(line->text);
    }
    const u64 next = more && after != 0 ? matched.back()->sequence
                     : lines.empty()    ? after
                                        : lines.back().sequence;
    return {{"lines", std::move(text)}, {"next", next}, {"more", more}};
}

json LogFilter(ToolState& state, const json& params) {
    const std::string filter = params.at("filter").get<std::string>();
    Common::Log::Filter parsed;
    parsed.ParseFilterString(filter);
    Common::Log::SetGlobalFilter(parsed);
    json result = {{"previous", state.log_filter}, {"filter", filter}};
    state.log_filter = filter;
    return result;
}

json CaptureFrame(ToolState& state, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"frame capture needs the Vulkan renderer"};
    }
    const u64 id = ++state.captures;
    const auto directory = state.dump_directory / fmt::format("capture-{}-{:03}", state.session, id);
    std::filesystem::create_directories(directory);
    const bool sampled = params.value("sampled", false);
    const bool buffers = params.value("buffers", false);
    const int timeout = Param<int>(params, "timeout_s", 120, 1, 600);
    VideoCore::Nx2Debug::RequestCapture(
        {.id = id, .directory = directory, .sampled = sampled, .buffers = buffers});
    const auto finished = VideoCore::Nx2Debug::WaitCapture(id, std::chrono::seconds{timeout});
    if (!finished) {
        VideoCore::Nx2Debug::CancelCapture(id);
        throw std::runtime_error{fmt::format(
            "the capture did not finish within {} s (paused or loading?); if it started, its "
            "files are still being written to {}",
            timeout, directory.generic_string())};
    }
    if (!finished->error.empty()) {
        throw std::runtime_error{finished->error};
    }
    const size_t keep = Param<size_t>(params, "manifest_lines", 200, 0, 5000);
    json entries = json::array();
    json kinds = json::object();
    json frame;
    json summary;
    size_t total = 0;
    std::ifstream manifest{directory / "manifest.jsonl"};
    for (std::string line; std::getline(manifest, line);) {
        json entry = json::parse(line, nullptr, false);
        if (entry.is_discarded()) {
            continue;
        }
        const std::string kind = entry.value("kind", std::string{});
        if (kind == "frame") {
            frame = std::move(entry);
        } else if (kind == "summary") {
            summary = std::move(entry);
        } else {
            kinds[kind] = kinds.value(kind, 0) + 1;
            if (total++ < keep) {
                entries.push_back(std::move(entry));
            }
        }
    }
    return {
        {"directory", directory.generic_string()},
        {"sampled", sampled},
        {"buffers", buffers},
        {"frame", std::move(frame)},
        {"summary", std::move(summary)},
        {"kinds", std::move(kinds)},
        {"entries_total", total},
        {"entries", std::move(entries)},
        {"index_command",
         fmt::format("python3 -I tools/nx2/frame_dump_index.py \"{}\"", directory.generic_string())},
    };
}

json RejectShaders(ToolState&, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"runtime rejection needs the Vulkan renderer"};
    }
    std::vector<u64> hashes;
    for (const json& hash : params.value("hashes", json::array())) {
        hashes.push_back(ParseShaderHash(hash.get<std::string>()));
    }
    const bool reject = params.value("reject", true);
    const u64 generation = VideoCore::Nx2Debug::SetRejected(hashes, reject);
    const int timeout = Param<int>(params, "timeout_s", 5, 0, 60);
    const bool applied = hashes.empty() ||
                         VideoCore::Nx2Debug::WaitRejectApplied(generation, std::chrono::seconds{timeout});
    json result = {{"rejected", HexList(VideoCore::Nx2Debug::RejectedHashes())}, {"applied", applied}};
    if (!applied) {
        result["note"] = "no frame was presented yet; the change applies at the next frame end";
    }
    return result;
}

json DrawFilterJson(const VideoCore::Nx2Debug::DrawFilter& filter) {
    return {{"range", {filter.range_begin, filter.range_end}},
            {"vs", HexList(filter.vs)},
            {"ps", HexList(filter.ps)},
            {"lines", filter.lines},
            {"rt0", HexAddress(filter.rt0)}};
}

/// Replaces the draw filter: {range: [a, b]?, vs: [hash]?, ps: [hash]?, lines?, rt0?} or
/// {spec: "<EDEN_NX2_DRAW_SKIP text>"}; {} clears it. Skips only the draw calls, at the next frame.
json DrawSkip(ToolState&, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"draw_skip needs the Vulkan renderer"};
    }
    VideoCore::Nx2Debug::DrawFilter filter;
    if (params.contains("spec")) {
        try {
            filter = VideoCore::Nx2Debug::ParseDrawFilter(params.at("spec").get<std::string>());
        } catch (const std::invalid_argument& error) {
            throw std::runtime_error{error.what()};
        }
    }
    if (const json& range = params.value("range", json::array()); !range.empty()) {
        if (!range.is_array() || range.size() != 2) {
            throw std::runtime_error{"range must be [first, end) as two draw indices"};
        }
        filter.range_begin = range[0].get<u32>();
        filter.range_end = range[1].get<u32>();
    }
    for (const json& hash : params.value("vs", json::array())) {
        filter.vs.push_back(ParseShaderHash(hash.get<std::string>()));
    }
    for (const json& hash : params.value("ps", json::array())) {
        filter.ps.push_back(ParseShaderHash(hash.get<std::string>()));
    }
    filter.lines |= params.value("lines", false);
    if (params.contains("rt0")) {
        filter.rt0 = AddressParam(params, "rt0");
    }
    const u64 generation = VideoCore::Nx2Debug::SetDrawFilter(filter);
    const int timeout = Param<int>(params, "timeout_s", 5, 0, 60);
    const bool applied =
        VideoCore::Nx2Debug::WaitDrawFilterApplied(generation, std::chrono::seconds{timeout});
    json result = {{"filter", DrawFilterJson(filter)}, {"active", !filter.Empty()}, {"applied", applied}};
    if (!applied) {
        result["note"] = "no frame was presented yet; the filter applies at the next frame end";
    }
    return result;
}

// Live optimisation switches (perf_sites / perf_slow / perf_flip): Nx2Perf::LiveState, applied
// by the rasterizer at the next frame end.
namespace Perf = VideoCommon::Nx2Perf;

/// What the slow path of each site costs, roughly (for spacing screenshots after a flip).
constexpr std::array<std::string_view, Perf::SiteNames.size()> SiteCosts{
    "low",    "low",  "medium", "medium", "low",  "low",    "low",    "low",  "high",
    "medium", "low",  "medium", "high",   "medium", "low", "high", "high (new pipelines)", "medium",
};

json LiveJson(const Perf::LiveState& live) {
    json sites = json::array();
    const u32 slow = live.Effective();
    for (size_t i = 0; i < Perf::SiteNames.size(); ++i) {
        sites.push_back({{"name", Perf::SiteNames[i]},
                         {"state", (slow >> i & 1) != 0 ? "slow" : "fast"},
                         {"named", (live.named >> i & 1) != 0},
                         {"live", true},
                         {"cost", SiteCosts[i]}});
    }
    json pieces = json::object();
    for (size_t i = 0; i < VideoCommon::BackX::DuskPieceNames.size(); ++i) {
        pieces[std::string{VideoCommon::BackX::DuskPieceNames[i]}] =
            (live.dusk_off >> i & 1) != 0 ? "old" : "upstream";
    }
    const auto control = [](int value) -> json {
        return value < 0 ? json("default") : json(value);
    };
    return {
        {"verify", live.verify},
        {"sites", std::move(sites)},
        {"backx", std::move(pieces)},
        {"inline_defer", (slow >> static_cast<u32>(Perf::Site::InlineDefer) & 1) == 0 ? 1 : 0},
        {"uboreuse", control(live.uboreuse)},
        {"frames_ahead", control(live.frames_ahead)},
        {"fence_batch", control(live.fence_batch)},
        {"sync_submit", control(live.sync_submit)},
        {"alias", [&] {
             json alias = json::object();
             for (size_t i = 0; i < Perf::AliasNames.size(); ++i) {
                 alias[std::string{Perf::AliasNames[i]}] = (live.alias >> i & 1) != 0 ? 1 : 0;
             }
             alias["alias_sync_always"] = control(live.alias_sync);
             return alias;
         }()},
        {"alias_stats", [] {
             json stats = json::object();
             for (size_t i = 0; i < Perf::AliasStatNames.size(); ++i) {
                 stats[std::string{Perf::AliasStatNames[i]}] =
                     Perf::g_alias_stats_last[i].load(std::memory_order_relaxed);
             }
             return stats;
         }()},
        {"start_time_only",
         {"state_blocks: the split front decides whether it sends register blocks when it starts "
          "(the back's handling of them is live)",
          "EDEN_NX2_SPLIT, EDEN_NX2_HOSTTIMING_PRIO and every switch not listed here"}},
    };
}

/// Queues `live` for the next frame end and waits (timeout_s, default 5) until it is applied.
json ApplyLiveParams(ToolState& state, const json& params, const Perf::LiveState& live) {
    if (!IsVulkan()) {
        throw std::runtime_error{"live switches need the Vulkan renderer (applied at its frame end)"};
    }
    const u64 request = Perf::RequestLive(live);
    const auto timeout = std::chrono::seconds{Param<int>(params, "timeout_s", 5, 0, 60)};
    const auto begin = std::chrono::steady_clock::now();
    while (Perf::g_live_applied.load(std::memory_order_acquire) < request &&
           std::chrono::steady_clock::now() - begin < timeout &&
           !state.stopping.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(10ms);
    }
    const bool applied = Perf::g_live_applied.load(std::memory_order_acquire) >= request;
    json result = LiveJson(applied ? Perf::CurrentLive() : live);
    result["applied"] = applied;
    result["request"] = request;
    result["frame"] = Perf::g_live_applied_frame.load(std::memory_order_relaxed);
    result["presents"] = g_nx2_present_count.load(std::memory_order_relaxed);
    if (!applied) {
        result["note"] = "no frame end yet; the change applies at the next one";
    }
    return result;
}

u32 SiteBit(std::string_view name) {
    for (size_t i = 0; i < Perf::SiteNames.size(); ++i) {
        if (Perf::SiteNames[i] == name) {
            return 1u << i;
        }
    }
    throw std::runtime_error{"unknown site \"" + std::string{name} + "\" (see perf_sites)"};
}

/// Every Nx2Perf site, fast or slow now, and the other live controls.
json PerfSites(ToolState&, const json&) {
    json result = LiveJson(Perf::CurrentLive());
    result["pending"] = Perf::g_live_requested.load(std::memory_order_relaxed) !=
                        Perf::g_live_applied.load(std::memory_order_relaxed);
    result["frame"] = Perf::g_live_applied_frame.load(std::memory_order_relaxed);
    return result;
}

/// {sites: [name, ...] | "all" | [], verify?: bool (default false)}: replaces the slow set
/// (inline_defer included; [] is everything fast, the normal state).
json PerfSlow(ToolState& state, const json& params) {
    Perf::LiveState live = Perf::TargetLive();
    const json sites = params.value("sites", json::array());
    if (sites.is_string()) {
        if (sites.get<std::string>() != "all") {
            throw std::runtime_error{"sites must be a list of names, \"all\" or []"};
        }
        live.named = (1u << Perf::SiteNames.size()) - 1;
    } else if (sites.is_array()) {
        live.named = 0;
        for (const json& name : sites) {
            live.named |= SiteBit(name.get<std::string>());
        }
    } else {
        throw std::runtime_error{"sites must be a list of names, \"all\" or []"};
    }
    live.verify = params.value("verify", false);
    return ApplyLiveParams(state, params, live);
}

/// {control, value}: inline_defer 0|1, uboreuse / frames_ahead / fence_batch / sync_submit
/// 0|1|"default", backx.<piece> or backx (all pieces) "old"|"upstream", verify true|false.
json PerfFlip(ToolState& state, const json& params) {
    Perf::LiveState live = Perf::TargetLive();
    const std::string control = params.at("control").get<std::string>();
    const json& value = params.at("value");
    const auto number = [&]() -> int {
        if (value.is_string() && value.get<std::string>() == "default") {
            return -1;
        }
        const int number = value.is_boolean() ? (value.get<bool>() ? 1 : 0) : value.get<int>();
        if (number != 0 && number != 1) {
            throw std::runtime_error{"value must be 0, 1 or \"default\""};
        }
        return number;
    };
    if (control == "inline_defer") {
        const u32 bit = 1u << static_cast<u32>(Perf::Site::InlineDefer);
        live.named = number() == 0 ? live.named | bit : live.named & ~bit;
    } else if (control == "uboreuse") {
        live.uboreuse = number();
    } else if (control == "frames_ahead") {
        live.frames_ahead = number();
    } else if (control == "fence_batch") {
        live.fence_batch = number();
    } else if (control == "sync_submit") {
        live.sync_submit = number();
    } else if (control == "verify") {
        live.verify = number() == 1;
    } else if (control == "alias_sync_always") {
        live.alias_sync = number(); // "default": The Duskbloods' images of at most 16x16
    } else if (const auto alias_it = std::ranges::find(Perf::AliasNames, control);
               alias_it != Perf::AliasNames.end()) {
        const u32 bit = 1u << static_cast<u32>(alias_it - Perf::AliasNames.begin());
        live.alias = number() == 1 ? live.alias | bit : live.alias & ~bit;
    } else if (control == "backx" || control.starts_with("backx.")) {
        const std::string text = value.get<std::string>();
        if (text != "old" && text != "upstream") {
            throw std::runtime_error{"backx values are \"old\" or \"upstream\""};
        }
        u32 bits = (1u << VideoCommon::BackX::DuskPieceNames.size()) - 1;
        if (control != "backx") {
            const std::string_view piece = std::string_view{control}.substr(6);
            const auto& names = VideoCommon::BackX::DuskPieceNames;
            const auto it = std::ranges::find(names, piece);
            if (it == names.end()) {
                throw std::runtime_error{"unknown back piece \"" + std::string{piece} + "\""};
            }
            bits = 1u << static_cast<u32>(it - names.begin());
        }
        live.dusk_off = text == "old" ? live.dusk_off | bits : live.dusk_off & ~bits;
    } else {
        throw std::runtime_error{"unknown control \"" + control +
                                 "\": inline_defer, uboreuse, frames_ahead, fence_batch, "
                                 "sync_submit, verify, backx, backx.<piece>, alias_sync_always, "
                                 "alias_full_resolve, alias_resync_upload, alias_diag"};
    }
    return ApplyLiveParams(state, params, live);
}

/// The graphics draws of the next whole frame with their shaders and state; with save (default)
/// also written as drawlist-<session>-<n>.json to the dump directory.
json DrawList(ToolState& state, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"draw_list needs the Vulkan renderer"};
    }
    const u64 id = ++state.draw_lists;
    const int timeout = Param<int>(params, "timeout_s", 10, 1, 60);
    VideoCore::Nx2Debug::RequestDrawList(id);
    const auto draws = VideoCore::Nx2Debug::WaitDrawList(id, std::chrono::seconds{timeout});
    if (!draws) {
        VideoCore::Nx2Debug::CancelDrawList(id);
        throw std::runtime_error{
            fmt::format("no whole frame was presented within {} s (paused or loading?)", timeout)};
    }
    const auto hex = [](u64 value) { return fmt::format("{:016x}", value); };
    json list = json::array();
    size_t skipped = 0;
    for (const auto& draw : *draws) {
        skipped += draw.skipped ? 1 : 0;
        json programs = json::array();
        json hashes = json::array();
        for (size_t stage = 0; stage < draw.programs.size(); ++stage) {
            programs.push_back(HexAddress(draw.programs[stage]));
            hashes.push_back(hex(draw.shader_hashes[stage]));
        }
        json targets = json::array();
        for (u32 target = 0; target < (std::min)(draw.rt_count, 8U); ++target) {
            targets.push_back({{"address", HexAddress(draw.rt[target])},
                               {"format", fmt::format("{:#x}", draw.rt_format[target])}});
        }
        json streams = json::array();
        for (size_t binding = 0; binding < draw.stream_address.size(); ++binding) {
            if (draw.stream_address[binding] != 0) {
                streams.push_back({{"binding", binding},
                                   {"address", HexAddress(draw.stream_address[binding])},
                                   {"stride", draw.stream_stride[binding]}});
            }
        }
        json entry = {
            {"index", draw.index},
            {"topology", draw.topology},
            {"indexed", draw.indexed},
            {"indirect", draw.indirect},
            {"skipped", draw.skipped},
            {"count", draw.count},
            {"first", draw.first},
            {"instances", draw.instances},
            {"vs", hex(draw.shader_hashes[1])},
            {"ps", hex(draw.shader_hashes[5])},
            {"vs_address", HexAddress(draw.programs[1])},
            {"ps_address", HexAddress(draw.programs[5])},
            {"programs", std::move(programs)},
            {"shader_hashes", std::move(hashes)},
            {"rt0", HexAddress(draw.rt[0])},
            {"targets", std::move(targets)},
            {"rt_size", {draw.rt_width, draw.rt_height}},
            {"depth", {{"address", HexAddress(draw.zeta)},
                       {"format", fmt::format("{:#x}", draw.zeta_format)},
                       {"enabled", draw.zeta_enable},
                       {"test", draw.depth_test},
                       {"write", draw.depth_write},
                       {"func", fmt::format("{:#x}", draw.depth_func)}}},
            {"blend", {{"enable_mask", draw.blend_enable},
                       {"per_target", draw.blend_per_target},
                       {"rt0", draw.blend0}}},
            {"color_mask0", fmt::format("{:#x}", draw.color_mask0)},
            {"cull", {{"enabled", draw.cull}, {"face", fmt::format("{:#x}", draw.cull_face)}}},
            {"line_width", draw.line_width},
            {"viewport0", draw.viewport0},
            {"streams", std::move(streams)},
        };
        if (draw.indexed) {
            entry["index_buffer"] = {{"address", HexAddress(draw.index_address)},
                                     {"format", draw.index_format}};
        }
        list.push_back(std::move(entry));
    }
    json result = {{"draws_total", draws->size()},
                   {"skipped", skipped},
                   {"topologies",
                    "0 points, 1 lines, 2 line loop, 3 line strip, 4 triangles, 5 strip, 6 fan, "
                    "7 quads, 10/11 line adjacency, 12/13 triangle adjacency, 14 patches"}};
    if (params.value("save", true)) {
        std::filesystem::create_directories(state.dump_directory);
        const auto path =
            state.dump_directory / fmt::format("drawlist-{}-{:03}.json", state.session, id);
        std::ofstream file{path};
        file << json{{"draws", list}}.dump(1);
        if (!file) {
            throw std::runtime_error{fmt::format("cannot write {}", path.generic_string())};
        }
        result["path"] = path.generic_string();
    }
    if (params.value("inline", true)) {
        result["draws"] = std::move(list);
    }
    return result;
}

json SampleThreads(ToolState& state, const json& params) {
    const std::string threads = params.value("threads", std::string{"GPU,GPUBack"});
    const double seconds = Param<double>(params, "seconds", 10.0, 1.0, 60.0);
    std::filesystem::create_directories(state.dump_directory);
    const auto path =
        state.dump_directory / fmt::format("samples-{}-{:03}.txt", state.session, ++state.samples);
    std::string report = Nx2Sampler::Sample(threads.c_str(), seconds, path.string());
    report.resize((std::min)(report.size(), Param<size_t>(params, "max_chars", 60000, 0, 4000000)));
    return {{"file", path.generic_string()}, {"report", std::move(report)}};
}

// Lifts or restores the speed limit (test scripts boot through the logos and menus unlimited).
json SpeedLimit(ToolState&, const json& params) {
    auto& setting = Settings::values.use_speed_limit;
    const bool previous = setting.GetValue();
    if (params.contains("on")) {
        setting.SetValue(params.at("on").get<bool>());
    }
    return {{"previous", previous}, {"on", setting.GetValue()}};
}

json HostTiming(ToolState& state, const json& params) {
    auto& stats = state.system.CoreTiming().host_wait_stats;
    const bool previous = stats.load();
    if (params.contains("stats")) {
        stats = params.at("stats").get<bool>();
    }
    return {{"previous", previous}, {"stats", stats.load()}};
}

json ReadMemory(ToolState& state, const json& params) {
    const u64 address = AddressParam(params, "address");
    const u64 size = ReadSize(params, address);
    const std::string as = params.value("as", std::string{"hex"});
    static_cast<void>(FormatMemory(std::span<const u8>{}, as));
    std::vector<u8> bytes(size);
    if (const auto unmapped = ReadGuestMemory(Application(state).GetMemory(), address, bytes)) {
        throw std::runtime_error{fmt::format(
            "guest address {:#x} is not mapped (reading {} bytes at {:#x})", *unmapped, size, address)};
    }
    json result = {{"address", HexAddress(address)}, {"size", size}};
    result.update(FormatMemory(bytes, as));
    return result;
}

json SearchMemory(ToolState& state, const json& params) {
    const std::vector<u8> pattern = ParseHexBytes(params.at("pattern").get<std::string>());
    if (pattern.size() > 256) {
        throw std::runtime_error{"the pattern is longer than 256 bytes"};
    }
    Kernel::KProcess& process = Application(state);
    const std::string regions = params.value("regions", std::string{"heap"});
    u64 region_start = 0;
    u64 region_size = 0;
    if (regions == "heap") {
        region_start = GetInteger(process.GetPageTable().GetHeapRegionStart());
        region_size = process.GetPageTable().GetHeapRegionSize();
    } else if (regions == "all") {
        region_start = GetInteger(process.GetPageTable().GetAddressSpaceStart());
        region_size = process.GetPageTable().GetAddressSpaceSize();
    } else {
        throw std::runtime_error{fmt::format("regions \"{}\": want heap or all", regions)};
    }
    const u64 start = params.contains("start") ? (std::max)(region_start, AddressParam(params, "start"))
                                               : region_start;
    const u64 end = params.contains("end")
                        ? (std::min)(region_start + region_size, AddressParam(params, "end"))
                        : region_start + region_size;
    const size_t max_hits = Param<size_t>(params, "max_hits", 64, 1, 256);
    const int budget = Param<int>(params, "budget_ms", 2000, 1, 10000);
    const auto begin = std::chrono::steady_clock::now();
    const GuestSearch search =
        SearchGuestMemory(process.GetMemory(), pattern, start, (std::max)(start, end), max_hits,
                          begin + std::chrono::milliseconds{budget});
    json hits = json::array();
    for (const u64 hit : search.hits) {
        hits.push_back(HexAddress(hit));
    }
    json result = {
        {"hits", std::move(hits)},
        {"complete", search.complete},
        {"start", HexAddress(start)},
        {"end", HexAddress((std::max)(start, end))},
        {"scanned_bytes", search.scanned},
        {"elapsed_ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                 begin)
                           .count()},
    };
    if (!search.complete) {
        result["next"] = HexAddress(search.next);
    }
    return result;
}

json ReadGpuMemory(ToolState& state, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"GPU memory reads need the Vulkan renderer"};
    }
    const u64 address = AddressParam(params, "gpu_address");
    const u64 size = ReadSize(params, address);
    const std::string as = params.value("as", std::string{"hex"});
    static_cast<void>(FormatMemory(std::span<const u8>{}, as));
    const u64 id = ++state.gpu_reads;
    const int timeout = Param<int>(params, "timeout_s", 5, 1, 60);
    VideoCore::Nx2Debug::RequestGpuRead({.id = id, .gpu_address = address, .size = size});
    const auto read = VideoCore::Nx2Debug::WaitGpuRead(id, std::chrono::seconds{timeout});
    if (!read) {
        VideoCore::Nx2Debug::CancelGpuRead(id);
        throw std::runtime_error{
            fmt::format("no frame was presented within {} s (paused or loading?)", timeout)};
    }
    if (!read->error.empty()) {
        throw std::runtime_error{read->error};
    }
    json result = {{"gpu_address", HexAddress(address)},
                   {"size", size},
                   {"address_space", read->address_space}};
    result.update(FormatMemory(read->bytes, as));
    return result;
}

/// Holds the steps on player 1 (or the handheld controller when player 1 is not connected) from
/// the next present on, and returns once they are done. The injector stops by itself after the
/// last step; leaving this function by any path removes it.
json RunInput(ToolState& state, const std::vector<InputStep>& steps, const json& params,
              const ClientGone& client_gone) {
    u64 total = 0;
    for (const InputStep& step : steps) {
        total += step.frames;
    }
    if (total > MAX_INPUT_FRAMES) {
        throw std::runtime_error{
            fmt::format("{} frames in total; at most {} per call", total, MAX_INPUT_FRAMES)};
    }
    auto& hid = state.system.HIDCore();
    HID::EmulatedController* controller = hid.GetEmulatedController(HID::NpadIdType::Player1);
    std::string name = "player1";
    if (!controller->IsConnected()) {
        controller = hid.GetEmulatedController(HID::NpadIdType::Handheld);
        name = "handheld";
    }
    if (!controller->IsConnected()) {
        throw std::runtime_error{"neither player 1 nor the handheld controller is connected"};
    }
    const int timeout = Param<int>(params, "timeout_s", static_cast<int>(total / 20 + 10), 1, 900);
    const u64 start = g_nx2_present_count.load(std::memory_order_relaxed) + 1;
    const auto script = std::make_shared<const std::vector<InputStep>>(steps);
    controller->SetInputInjector([script, start] {
        return InputAt(*script, start, g_nx2_present_count.load(std::memory_order_relaxed));
    });
    SCOPE_EXIT {
        controller->SetInputInjector({});
    };
    const auto begin = std::chrono::steady_clock::now();
    const auto deadline = begin + std::chrono::seconds{timeout};
    for (;;) {
        const u64 now = g_nx2_present_count.load(std::memory_order_relaxed);
        if (now >= start + total) {
            break;
        }
        if (state.stopping.load(std::memory_order_relaxed)) {
            throw std::runtime_error{"the game is shutting down; input released"};
        }
        if (client_gone()) {
            LOG_INFO(Debug, "Debug server client left during input; input released");
            throw std::runtime_error{"the client disconnected; input released"};
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error{fmt::format(
                "{} of {} frames were presented within {} s (paused or loading?); input released",
                now >= start ? now - start : 0, total, timeout)};
        }
        std::this_thread::sleep_for(2ms);
    }
    return {
        {"controller", name},
        {"steps", steps.size()},
        {"frames", total},
        {"first_present", start},
        {"elapsed_s",
         std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count()},
    };
}

json Input(ToolState& state, const json& params, const ClientGone& client_gone) {
    json step = params;
    step.erase("timeout_s");
    return RunInput(state, {ParseInputStep(step)}, params, client_gone);
}

json InputSequence(ToolState& state, const json& params, const ClientGone& client_gone) {
    const json& list = params.at("steps");
    if (!list.is_array() || list.empty() || list.size() > MAX_INPUT_STEPS) {
        throw std::runtime_error{fmt::format("steps must be an array of 1 to {}", MAX_INPUT_STEPS)};
    }
    std::vector<InputStep> steps;
    for (size_t index = 0; index < list.size(); ++index) {
        try {
            steps.push_back(ParseInputStep(list[index]));
        } catch (const std::exception& error) {
            throw std::runtime_error{fmt::format("step {}: {}", index, error.what())};
        }
    }
    return RunInput(state, steps, params, client_gone);
}

Handlers MakeHandlers(const std::shared_ptr<ToolState>& state) {
    const auto handler = [&state](json (*tool)(ToolState&, const json&)) -> Handler {
        return [state, tool](const json& params, const ClientGone&) { return tool(*state, params); };
    };
    const auto waiting = [&state](json (*tool)(ToolState&, const json&, const ClientGone&)) -> Handler {
        return [state, tool](const json& params, const ClientGone& client_gone) {
            return tool(*state, params, client_gone);
        };
    };
    return {
        {"status", handler(Status)},
        {"frame_times", handler(FrameTimes)},
        {"screenshot", handler(Screenshot)},
        {"log_tail", handler(LogTail)},
        {"log_filter", handler(LogFilter)},
        {"capture_frame", handler(CaptureFrame)},
        {"reject_shaders", handler(RejectShaders)},
        {"draw_skip", handler(DrawSkip)},
        {"draw_list", handler(DrawList)},
        {"perf_sites", handler(PerfSites)},
        {"perf_slow", handler(PerfSlow)},
        {"perf_flip", handler(PerfFlip)},
        {"sample", handler(SampleThreads)},
        {"speed_limit", handler(SpeedLimit)},
        {"host_timing", handler(HostTiming)},
        {"read_memory", handler(ReadMemory)},
        {"search_memory", handler(SearchMemory)},
        {"read_gpu_memory", handler(ReadGpuMemory)},
        {"input", waiting(Input)},
        {"input_sequence", waiting(InputSequence)},
    };
}

} // Anonymous namespace

u64 ParseShaderHash(std::string_view text) {
    const std::string_view digits =
        text.starts_with("0x") || text.starts_with("0X") ? text.substr(2) : text;
    u64 value = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
    if (digits.size() != 16 || error != std::errc{} || end != digits.data() + digits.size()) {
        throw std::runtime_error{fmt::format(
            "bad hash \"{}\": want all 16 hex digits of a native program key (shader dump file "
            "name) or shader hash (frame dump \"shaders\"); a shorter one would match nothing",
            text)};
    }
    return value;
}

std::vector<u8> ParseHexBytes(std::string_view text) {
    std::string digits;
    const std::string_view body =
        text.starts_with("0x") || text.starts_with("0X") ? text.substr(2) : text;
    for (const char c : body) {
        if (c != ' ') {
            digits.push_back(c);
        }
    }
    std::vector<u8> bytes(digits.size() / 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
        const auto [end, error] =
            std::from_chars(digits.data() + i * 2, digits.data() + i * 2 + 2, bytes[i], 16);
        if (error != std::errc{} || end != digits.data() + i * 2 + 2) {
            bytes.clear();
            break;
        }
    }
    if (bytes.empty() || digits.size() % 2 != 0) {
        throw std::runtime_error{fmt::format(
            "bad pattern \"{}\": want hex byte pairs in memory order (little endian: 1.0f is "
            "0000803f)",
            text)};
    }
    return bytes;
}

json FormatMemory(std::span<const u8> bytes, std::string_view as) {
    if (as == "hex") {
        std::string hex;
        hex.reserve(bytes.size() * 2);
        for (const u8 byte : bytes) {
            fmt::format_to(std::back_inserter(hex), "{:02x}", byte);
        }
        return {{"hex", std::move(hex)}};
    }
    const size_t element = as == "u32" || as == "f32" ? 4 : as == "u64" ? 8 : 0;
    if (element == 0) {
        throw std::runtime_error{fmt::format("as \"{}\": want hex, u32, u64 or f32", as)};
    }
    if (bytes.size() % element != 0) {
        throw std::runtime_error{fmt::format("size must be a multiple of {} for {}", element, as)};
    }
    json values = json::array();
    for (size_t offset = 0; offset < bytes.size(); offset += element) {
        if (element == 8) {
            u64 value;
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            values.push_back(HexAddress(value));
            continue;
        }
        u32 value;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        if (as == "u32") {
            values.push_back(value);
            continue;
        }
        const float number = std::bit_cast<float>(value);
        if (std::isnan(number)) {
            values.push_back("nan");
        } else if (std::isinf(number)) {
            values.push_back(number < 0 ? "-inf" : "inf");
        } else {
            values.push_back(number);
        }
    }
    return {{std::string{as}, std::move(values)}};
}

std::optional<u64> ReadGuestMemory(Memory::Memory& memory, u64 address, std::span<u8> out) {
    for (size_t offset = 0; offset < out.size();) {
        const u64 current = address + offset;
        const size_t length = (std::min)(static_cast<size_t>(Memory::LICHII_PAGESIZE -
                                                             (current & Memory::LICHII_PAGEMASK)),
                                         out.size() - offset);
        const u8* const source = GuestByte(memory, current);
        if (source == nullptr) {
            return current;
        }
        std::memcpy(out.data() + offset, source, length);
        offset += length;
    }
    return std::nullopt;
}

GuestSearch SearchGuestMemory(Memory::Memory& memory, std::span<const u8> pattern, u64 start,
                              u64 end, size_t max_hits,
                              std::chrono::steady_clock::time_point deadline) {
    GuestSearch search{.hits = {}, .next = end, .complete = true, .scanned = 0};
    // The bytes of a run of mapped pages that a match starting there could still need.
    std::vector<u8> window;
    window.reserve(Memory::LICHII_PAGESIZE + pattern.size());
    for (u64 address = start; address < end;) {
        if (std::chrono::steady_clock::now() >= deadline) {
            search.next = address - window.size();
            search.complete = false;
            return search;
        }
        const u64 page_end = (std::min)((address | Memory::LICHII_PAGEMASK) + 1, end);
        const u8* const source = GuestByte(memory, address);
        if (source == nullptr) {
            window.clear();
            address = page_end;
            continue;
        }
        const u64 window_start = address - window.size();
        const size_t length = page_end - address;
        window.insert(window.end(), source, source + length);
        search.scanned += length;
        for (size_t offset = 0; offset + pattern.size() <= window.size(); ++offset) {
            const void* const found = std::memchr(window.data() + offset, pattern[0],
                                                  window.size() - pattern.size() + 1 - offset);
            if (found == nullptr) {
                break;
            }
            offset = static_cast<size_t>(static_cast<const u8*>(found) - window.data());
            if (std::memcmp(window.data() + offset, pattern.data(), pattern.size()) != 0) {
                continue;
            }
            search.hits.push_back(window_start + offset);
            if (search.hits.size() >= max_hits) {
                search.next = window_start + offset + 1;
                search.complete = false;
                return search;
            }
        }
        window.erase(window.begin(),
                     window.end() - static_cast<std::ptrdiff_t>(
                                        (std::min)(pattern.size() - 1, window.size())));
        address = page_end;
    }
    return search;
}

InputStep ParseInputStep(const json& step) {
    if (!step.is_object()) {
        throw std::runtime_error{"a step is an object"};
    }
    for (const auto& [key, value] : step.items()) {
        if (key != "buttons" && key != "left_stick" && key != "right_stick" && key != "frames") {
            throw std::runtime_error{fmt::format(
                "unknown key \"{}\": want buttons, left_stick, right_stick, frames", key)};
        }
    }
    InputStep result{.input = {}, .frames = 0};
    const json& frames = step.at("frames");
    if (!frames.is_number_integer() || frames.get<s64>() < 1 ||
        frames.get<u64>() > MAX_INPUT_FRAMES) {
        throw std::runtime_error{fmt::format("frames must be 1 to {}", MAX_INPUT_FRAMES)};
    }
    result.frames = frames.get<u64>();
    for (const json& button : step.value("buttons", json::array())) {
        const std::string text = button.is_string() ? button.get<std::string>() : button.dump();
        const auto it = std::ranges::find(INPUT_BUTTONS, std::string_view{text},
                                          &std::pair<std::string_view, HID::NpadButton>::first);
        if (it == INPUT_BUTTONS.end()) {
            std::string names;
            for (const auto& [known, _] : INPUT_BUTTONS) {
                names += names.empty() ? "" : ", ";
                names += known;
            }
            throw std::runtime_error{fmt::format("unknown button {}: want one of {}", text, names)};
        }
        result.input.buttons |= it->second;
    }
    const auto stick = [&step](const char* key) -> std::optional<HID::AnalogStickState> {
        if (!step.contains(key)) {
            return std::nullopt;
        }
        const json& value = step.at(key);
        const auto axis = [&](size_t index) {
            if (!value.is_array() || value.size() != 2 || !value[index].is_number() ||
                std::abs(value[index].get<double>()) > 1.0) {
                throw std::runtime_error{fmt::format("{} must be [x, y] with both in -1..1", key)};
            }
            return static_cast<s32>(std::lround(value[index].get<double>() * HID::HID_JOYSTICK_MAX));
        };
        return HID::AnalogStickState{.x = axis(0), .y = axis(1)};
    };
    result.input.left = stick("left_stick");
    result.input.right = stick("right_stick");
    return result;
}

std::optional<HID::InjectedInput> InputAt(std::span<const InputStep> steps, u64 start, u64 now) {
    if (now < start) {
        return std::nullopt;
    }
    u64 end = start;
    for (const InputStep& step : steps) {
        end += step.frames;
        if (now < end) {
            return step.input;
        }
    }
    return std::nullopt;
}

Service::Service(System& system, std::string title, u64 program_id, const Endpoint& endpoint,
                 std::string dump_directory)
    : state{std::make_shared<ToolState>(system, std::move(title), program_id,
                                        std::move(dump_directory))} {
    VideoCore::Nx2Debug::BeginSession();
    Common::Log::SetRingCapacity(RING_LINES);
    try {
        server = std::make_unique<Server>(endpoint, MakeHandlers(state));
    } catch (...) {
        VideoCore::Nx2Debug::EndSession();
        throw;
    }
    g_nx2_present_time_services.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO(Debug, "Debug server listening on {}:{}, dump dir {}", endpoint.host, server->Port(),
             state->dump_directory.generic_string());
}

Service::~Service() {
    state->stopping.store(true, std::memory_order_relaxed);
    g_nx2_present_time_services.fetch_sub(1, std::memory_order_relaxed);
    VideoCore::Nx2Debug::EndSession();
    server.reset();
}

std::unique_ptr<Service> MaybeStartService(System& system, std::string title, u64 program_id) {
    const char* const text = std::getenv("EDEN_NX2_DEBUG_SERVER");
    if (text == nullptr || *text == '\0') {
        return nullptr;
    }
    std::string error;
    const auto endpoint = ParseEndpoint(text, error);
    if (!endpoint) {
        LOG_ERROR(Debug, "EDEN_NX2_DEBUG_SERVER={}: {}; the debug server is off", text, error);
        return nullptr;
    }
    const char* const directory = std::getenv("EDEN_NX2_DEBUG_DIR");
    std::string dump_directory = directory != nullptr && *directory != '\0'
        ? std::string{directory}
        : (Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "debug-server").generic_string();
    try {
        return std::make_unique<Service>(system, std::move(title), program_id, *endpoint,
                                         std::move(dump_directory));
    } catch (const std::exception& failure) {
        LOG_ERROR(Debug, "Debug server cannot listen on {}:{}: {}", endpoint->host, endpoint->port,
                  failure.what());
        return nullptr;
    }
}

} // namespace Core::Nx2Debug
