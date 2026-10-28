#include "osd_font.hpp"

#include <algorithm>

namespace mcsink
{

namespace
{

/* 5x7 glyphs, one byte per row, bit 4 = leftmost column. */
struct glyph
{
    char    c;
    uint8_t rows[7];
};

constexpr glyph k_font[] = {
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}},
    {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}},
    {'3', {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e}},
    {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}},
    {'5', {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}},
    {'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}},
    {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}},
    {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0c, 0x04, 0x08}},
    {'-', {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}},
    {':', {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00}},
    {'b', {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1e}},
    {'c', {0x00, 0x00, 0x0e, 0x10, 0x10, 0x11, 0x0e}},
    {'e', {0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e}},
    {'f', {0x06, 0x09, 0x08, 0x1c, 0x08, 0x08, 0x08}},
    {'k', {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}},
    {'m', {0x00, 0x00, 0x1a, 0x15, 0x15, 0x11, 0x11}},
    {'n', {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11}},
    {'p', {0x00, 0x00, 0x1e, 0x11, 0x1e, 0x10, 0x10}},
    {'r', {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}},
    {'s', {0x00, 0x00, 0x0e, 0x10, 0x0e, 0x01, 0x1e}},
};

const uint8_t *rows_for(char c)
{
    for (const glyph &g : k_font)
    {
        if (g.c == c)
        {
            return g.rows;
        }
    }
    return nullptr;
}

}  // namespace

int text_width(std::string_view text, int scale)
{
    return int(text.size()) * k_glyph_w * scale - scale;
}

int text_height(int scale)
{
    return (k_glyph_h - 1) * scale;
}

bool draw_text_nv12(uint8_t *frame, size_t size, const vstreamer::video_raw_caps &caps, int x,
                    int y, int scale, int pad, std::string_view text)
{
    const int w = caps.width, h = caps.height, stride = caps.hor_stride, vstride = caps.ver_stride;
    if (!frame || w <= 0 || h <= 0 || stride < w || vstride < h || scale < 1 ||
        size < size_t(stride) * size_t(vstride) * 3 / 2)
    {
        return false;
    }
    uint8_t *luma = frame;
    uint8_t *chroma = frame + size_t(stride) * size_t(vstride);
    // Box, on even coordinates so it covers whole chroma samples.
    const int bx0 = std::max(0, (x - pad) & ~1), by0 = std::max(0, (y - pad) & ~1);
    const int bx1 = std::min(w, (x + text_width(text, scale) + pad + 1) & ~1);
    const int by1 = std::min(h, (y + text_height(scale) + pad + 1) & ~1);
    for (int yy = by0; yy < by1; ++yy)
    {
        uint8_t *row = luma + size_t(yy) * size_t(stride);
        for (int xx = bx0; xx < bx1; ++xx)
        {
            row[xx] = uint8_t(row[xx] * 3 / 8);
        }
    }
    for (int yy = by0 / 2; yy < by1 / 2; ++yy)
    {
        uint8_t *row = chroma + size_t(yy) * size_t(stride);
        for (int xx = bx0; xx < bx1; ++xx)
        {
            row[xx] = uint8_t(128 + (int(row[xx]) - 128) / 4);
        }
    }
    for (size_t i = 0; i < text.size(); ++i)
    {
        const uint8_t *rows = rows_for(text[i]);
        if (!rows)
        {
            continue;
        }
        const int gx = x + int(i) * k_glyph_w * scale;
        for (int r = 0; r < 7; ++r)
        {
            for (int c = 0; c < 5; ++c)
            {
                if (!(rows[r] & (0x10 >> c)))
                {
                    continue;
                }
                for (int dy = 0; dy < scale; ++dy)
                {
                    const int py = y + r * scale + dy;
                    if (py < 0 || py >= h)
                    {
                        continue;
                    }
                    uint8_t *row = luma + size_t(py) * size_t(stride);
                    for (int dx = 0; dx < scale; ++dx)
                    {
                        const int px = gx + c * scale + dx;
                        if (px >= 0 && px < w)
                        {
                            row[px] = 235;
                        }
                    }
                }
            }
        }
    }
    return true;
}

}  // namespace mcsink
