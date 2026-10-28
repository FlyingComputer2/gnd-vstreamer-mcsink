#ifndef MCSINK_OSD_FONT_HPP
#define MCSINK_OSD_FONT_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "core/sdu_caps.hpp"

namespace mcsink
{

/* Glyph cell of the built-in 5x7 font, in font pixels (glyph plus one column/row gap). */
constexpr int k_glyph_w = 6;
constexpr int k_glyph_h = 8;

/* Size in frame pixels of text drawn at scale (no padding). */
int text_width(std::string_view text, int scale);
int text_height(int scale);

/*
 * Burns text into an NV12 frame at (x, y) on a darkened box padded by pad pixels: white
 * glyphs, background luma scaled to 3/8 and chroma pulled toward neutral. Characters the
 * font lacks draw as blanks. Clipped to the frame; returns false when the buffer is smaller
 * than hor_stride * ver_stride * 3 / 2 or the caps are unusable.
 */
bool draw_text_nv12(uint8_t *frame, size_t size, const vstreamer::video_raw_caps &caps, int x,
                    int y, int scale, int pad, std::string_view text);

}  // namespace mcsink

#endif  // MCSINK_OSD_FONT_HPP
