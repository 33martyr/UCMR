// SPDX-License-Identifier: GPL-2.0-or-later
// Copy extents between images that share guest memory (texture_cache/aliasing.h); adapted from
// cuesta4/shadPS4 (eltutz) tests/test_aliasing.cpp. No game, Vulkan device or window required.
#include <cassert>
#include <cstdio>

#include "video_core/amdgpu/resource.h"
#include "video_core/texture_cache/aliasing.h"
#include "video_core/texture_cache/image_info.h"

using namespace VideoCore;

static ImageInfo Make(Extent3D size, vk::Format format = vk::Format::eR8G8B8A8Unorm) {
    ImageInfo info{};
    info.guest_address = 0x100000;
    info.num_bits = 32;
    info.pixel_format = format;
    info.type = AmdGpu::ImageType::Color2D;
    info.pitch = size.width;
    info.size = size;
    return info;
}

int main() {
    // Same format: the smaller image is fully covered by the larger one.
    {
        const ImageInfo src = Make({1920, 1088, 1}), dst = Make({1920, 1080, 1});
        assert(GetAliasCopyExtent(src, dst) == dst.size);
        assert(GetAliasCopyExtent(dst, src) == dst.size);
    }
    // Compatible views of other formats: the common rows.
    {
        const ImageInfo src = Make({1920, 1088, 1}, vk::Format::eR8G8B8A8Uint);
        const ImageInfo dst = Make({1920, 1080, 1}, vk::Format::eR8G8B8A8Unorm);
        const Extent3D rows{1920, 1080, 1};
        assert(GetAliasCopyExtent(src, dst) == rows);
        assert(GetAliasCopyExtent(dst, src) == rows);
    }
    // Other memory layouts never alias.
    {
        const ImageInfo src = Make({1920, 1088, 1});
        ImageInfo dst = Make({1920, 1080, 1});
        dst.guest_address += 0x1000;
        assert(!GetAliasCopyExtent(src, dst));
        dst = Make({1920, 1080, 1});
        ++dst.pitch;
        assert(!GetAliasCopyExtent(src, dst));
        dst = Make({1920, 1080, 1});
        dst.props.is_depth = true;
        assert(!GetAliasCopyExtent(src, dst));
    }
    // Incompatible intersections.
    {
        const ImageInfo src = Make({1920, 1088, 1}, vk::Format::eR8G8B8A8Uint);
        ImageInfo dst = Make({1280, 1080, 1}, vk::Format::eR8G8B8A8Unorm);
        assert(!GetAliasCopyExtent(src, dst));
        dst = Make({1920, 1080, 1}, vk::Format::eR16G16B16A16Sfloat);
        assert(!GetAliasCopyExtent(src, dst));
        dst = Make({1920, 1080, 1}, vk::Format::eR8G8B8A8Unorm);
        dst.resources.levels = 2;
        assert(!GetAliasCopyExtent(src, dst));
    }
    // Pieces of a surface copy (CP DMA moves at most 2 MiB - 1 per packet): the scene colour
    // into the refraction texture, both RGBA16F 1920x1080 of the same layout elsewhere in memory.
    {
        const auto surface = [](VAddr address) {
            ImageInfo info = Make({1920, 1080, 1}, vk::Format::eR16G16B16A16Sfloat);
            info.num_bits = 64;
            info.guest_address = address;
            info.guest_size = 1920ull * 1088 * 8;
            return info;
        };
        const ImageInfo scene = surface(0x1135ab0000), refraction = surface(0x1149d40000);
        constexpr u64 piece = 0x1ffffc;
        assert(IsSurfacePieceCopy(scene, scene.guest_address, refraction, refraction.guest_address,
                                  piece));
        assert(IsSurfacePieceCopy(scene, scene.guest_address + 3 * piece, refraction,
                                  refraction.guest_address + 3 * piece, piece));
        // The last piece ends at the surface's end; one byte more is past it.
        const u64 tail = scene.guest_size - 7 * piece;
        assert(IsSurfacePieceCopy(scene, scene.guest_address + 7 * piece, refraction,
                                  refraction.guest_address + 7 * piece, tail));
        assert(!IsSurfacePieceCopy(scene, scene.guest_address + 7 * piece, refraction,
                                   refraction.guest_address + 7 * piece, tail + 1));
        // The whole surface in one copy is left to the buffer cache; other offsets, formats,
        // layouts or sizes are not the same piece.
        assert(!IsSurfacePieceCopy(scene, scene.guest_address, refraction, refraction.guest_address,
                                   scene.guest_size));
        assert(!IsSurfacePieceCopy(scene, scene.guest_address + piece, refraction,
                                   refraction.guest_address, piece));
        assert(!IsSurfacePieceCopy(scene, scene.guest_address - 4, refraction,
                                   refraction.guest_address - 4, piece));
        ImageInfo other = refraction;
        other.pixel_format = vk::Format::eR32G32Uint;
        assert(!IsSurfacePieceCopy(scene, scene.guest_address, other, other.guest_address, piece));
        other = refraction;
        other.tile_mode = AmdGpu::TileMode::Thin1DThin;
        assert(!IsSurfacePieceCopy(scene, scene.guest_address, other, other.guest_address, piece));
        other = refraction;
        other.size.height = 540;
        assert(!IsSurfacePieceCopy(scene, scene.guest_address, other, other.guest_address, piece));
        other = refraction;
        other.props.is_depth = true;
        assert(!IsSurfacePieceCopy(scene, scene.guest_address, other, other.guest_address, piece));
        assert(!IsSurfacePieceCopy(scene, scene.guest_address, refraction, refraction.guest_address,
                                   0));
    }
    std::puts("aliasing-test: OK");
    return 0;
}
