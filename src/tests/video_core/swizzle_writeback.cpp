// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"
#include "video_core/textures/decoders.h"

TEST_CASE("Span write-back swizzles only the copied rows", "[video_core][texture_cache][writeback]") {
    VideoCommon::ImageInfo info;
    info.type = VideoCommon::ImageType::Linear;
    info.format = VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM;
    info.size = {4, 4, 1};
    info.resources = {1, 1};
    info.pitch = 32;
    const std::array<VideoCommon::BufferImageCopy, 1> copies{{{
        .buffer_size = 64,
        .image_subresource = {0, 0, 1},
        .image_offset = {1, 1, 0},
        .image_extent = {2, 2, 1},
    }}};
    std::array<u8, 128> guest;
    guest.fill(0xCC);
    std::array<u8, 64> data;
    data.fill(0x5A);
    std::vector<std::pair<u64, u64>> written;
    VideoCommon::SwizzleImageTo(guest, info, copies, data, false, written);
    REQUIRE(written == std::vector<std::pair<u64, u64>>{{36, 44}, {68, 76}});
    for (size_t index = 0; index < guest.size(); ++index) {
        const bool copied = (36 <= index && index < 44) || (68 <= index && index < 76);
        REQUIRE(guest[index] == (copied ? 0x5A : 0xCC));
    }
}

TEST_CASE("Span write-back swizzles each block-linear layer", "[video_core][texture_cache][writeback]") {
    VideoCommon::ImageInfo info;
    info.type = VideoCommon::ImageType::e2D;
    info.format = VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM;
    info.size = {4, 4, 1};
    info.resources = {1, 2};
    info.layer_stride = 512;
    const std::array<VideoCommon::BufferImageCopy, 1> copies{{{
        .buffer_size = 128,
        .image_subresource = {0, 0, 2},
        .image_extent = {4, 4, 1},
    }}};
    std::array<u8, 1024> guest{};
    std::array<u8, 128> data;
    for (size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<u8>(index);
    }
    std::vector<std::pair<u64, u64>> written;
    VideoCommon::SwizzleImageTo(guest, info, copies, data, false, written);
    REQUIRE(written == std::vector<std::pair<u64, u64>>{{0, 512}, {512, 1024}});
    std::array<u8, 128> decoded{};
    for (size_t layer = 0; layer < 2; ++layer) {
        Tegra::Texture::UnswizzleTexture(std::span(decoded).subspan(layer * 64, 64),
                                         std::span(guest).subspan(layer * 512, 512), 4, 4, 4, 1,
                                         0, 0);
    }
    REQUIRE(decoded == data);
}
