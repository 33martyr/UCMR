// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "video_core/texture_cache/types.h"

namespace VideoCore {

struct ImageInfo;

[[nodiscard]] std::optional<Extent3D> GetAliasCopyExtent(const ImageInfo& src,
                                                         const ImageInfo& dst);

/// gow3 25 Nov 2026 (The Temple's water): whether a guest memory copy of `size` bytes from
/// src_addr to dst_addr moves a piece (not all) of colour image `src` into the same piece of
/// image `dst`: one mip, same format, size and memory layout, the range at the same offset in
/// both. Copying the whole image then gives what the pieces of a whole-surface copy give.
[[nodiscard]] bool IsSurfacePieceCopy(const ImageInfo& src, VAddr src_addr, const ImageInfo& dst,
                                      VAddr dst_addr, u64 size);

} // namespace VideoCore
