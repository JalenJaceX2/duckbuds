// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "video_core/rasterizer_download_area.h"

TEST_CASE("A landing published while another lands waits for it to end",
          "[video_core][download_landing]") {
    using namespace std::chrono_literals;
    // The texture cache's landing and the buffer cache's, each under its own cache's lock.
    std::optional<VideoCore::DownloadLanding> first;
    first.emplace(0x1000, 0x2000);
    std::atomic<bool> second_published{};
    std::thread second([&] {
        const VideoCore::DownloadLanding landing{0x5000, 0x6000};
        second_published.store(true);
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(second_published.load());
    // A guest write collected in the first landing's range still stores its bytes after it.
    std::atomic<bool> stored{};
    std::thread writer([&] {
        VideoCore::DownloadLanding::Await(0x1800, 4);
        stored.store(true);
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(stored.load());
    first.reset();
    second.join();
    writer.join();
    CHECK(second_published.load());
    CHECK(stored.load());
}
