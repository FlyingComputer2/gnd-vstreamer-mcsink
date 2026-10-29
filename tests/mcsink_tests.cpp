// Host tests: OSD text, latency window and NV12 burn-in. No network or display.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "core/sdu_caps.hpp"
#include "osd_font.hpp"
#include "osd_overlay.hpp"

using namespace vstreamer;

namespace
{

int failures = 0;

#define CHECK(cond)                                                                    \
    do                                                                                 \
    {                                                                                  \
        if (!(cond))                                                                   \
        {                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

video_raw_caps caps_1080p()
{
    video_raw_caps c {};
    c.width = 1920;
    c.height = 1080;
    c.hor_stride = 1920;
    c.ver_stride = 1088;
    c.fps_num = 30;
    c.fps_den = 1;
    return c;
}

// rows: plane height in the buffer; 0 = the caps' ver_stride.
component_pdu frame(const video_raw_caps &c, uint64_t ts_us, uint8_t luma, int rows = 0)
{
    const size_t plane = size_t(c.hor_stride) * size_t(rows ? rows : c.ver_stride);
    std::vector<uint8_t> bytes(plane * 3 / 2, 128);
    std::memset(bytes.data(), luma, plane);
    component_pdu pdu;
    pdu.sdu_type = sdu_type_e::NV12;
    pdu.sdu = bfc::shared_sized_buffer::copy_from(bytes.data(), bytes.size());
    pdu.ts_us = ts_us;
    return pdu;
}

void test_text_and_latency()
{
    auto counters = std::make_shared<mcsink::stats>();
    int64_t now = 10'000'000'000;  // ns
    mcsink::osd_overlay osd(counters, [&] { return now; });
    CHECK(osd.open() == 0);
    const auto caps = caps_1080p();
    CHECK(osd.input(make_caps_pdu(sdu_type_e::CAPS_VIDEO_RAW, caps, 0, 0)) == 0);
    component_pdu out;
    CHECK(osd.output(out) == 0 && out.sdu_type == sdu_type_e::CAPS_VIDEO_RAW);
    // 30 frames over one second, each decoded 80 ms after capture; 750 kB received.
    for (int i = 0; i < 30; ++i)
    {
        now += 33'333'000;  // whole µs, like ts_us
        counters->bytes.fetch_add(25'000);
        CHECK(osd.input(frame(caps, uint64_t(now / 1000 - 80'000), 100)) == 0);
        CHECK(osd.output(out) == 0);
    }
    // One more frame closes the window.
    now += 1'000'000;
    CHECK(osd.input(frame(caps, 0, 100)) == 0);
    std::string text;
    CHECK(osd.query("text", &text) == 0);
    std::printf("osd text: %s\n", text.c_str());
    // 6000 kbit over 1.001 s; 30 frames over the same window.
    CHECK(text == "frame 30.0 fps\nerror 0.0 fps\njitter 2.1 ms\nlatency --- ms");  // glass latency not set yet
    std::string decode;
    CHECK(osd.query("latency_ms", &decode) == 0 && decode == "80.000");
    // The frame now carries the burned-in text: white glyph pixels in the top-left box.
    CHECK(osd.output(out) == 0);
    const uint8_t *y = out.sdu.u8();
    size_t white = 0, dark = 0;
    for (int r = 0; r < 120; ++r)
        for (int c = 0; c < 1200; ++c)
        {
            const uint8_t v = y[size_t(r) * 1920 + size_t(c)];
            white += v == 235;
            dark += v == 37;  // 100 * 3 / 8
        }
    CHECK(white > 1000);
    CHECK(dark > 1000);
    // Below the box the frame is untouched.
    CHECK(y[size_t(500) * 1920 + 500] == 100);
}

// The app sets the display's capture -> on-screen latency; the next window
// shows it as the OSD's latency.
void test_screen_latency()
{
    auto counters = std::make_shared<mcsink::stats>();
    int64_t now = 1'000'000'000;
    mcsink::osd_overlay osd(counters, [&] { return now; });
    osd.open();
    const auto caps = caps_1080p();
    osd.input(make_caps_pdu(sdu_type_e::CAPS_VIDEO_RAW, caps, 0, 0));
    CHECK(osd.configure("screen_latency_ms", "x") == -EINVAL);
    CHECK(osd.configure("screen_latency_ms", "97.30") == 0);
    for (int i = 0; i < 40; ++i)
    {
        now += 33'000'000;
        osd.input(frame(caps, uint64_t(now / 1000 - 80'000), 100));
    }
    std::string text;
    osd.query("text", &text);
    std::printf("osd text: %s\n", text.c_str());
    CHECK(text.find("\nlatency 97.3 ms") != std::string::npos);
}

void test_no_capture_time()
{
    auto counters = std::make_shared<mcsink::stats>();
    int64_t now = 1'000'000'000;
    mcsink::osd_overlay osd(counters, [&] { return now; });
    osd.open();
    const auto caps = caps_1080p();
    osd.input(make_caps_pdu(sdu_type_e::CAPS_VIDEO_RAW, caps, 0, 0));
    for (int i = 0; i < 40; ++i)
    {
        now += 33'000'000;
        osd.input(frame(caps, 0, 100));
    }
    std::string text;
    osd.query("text", &text);
    std::string decode;
    osd.query("latency_ms", &decode);
    CHECK(decode == "-1.000");
}

// The MPP decoder packs frames to the visible height (1080 rows) while its
// caps keep ver_stride 1088; the OSD must still draw, with chroma at row 1080.
void test_packed_frame()
{
    auto counters = std::make_shared<mcsink::stats>();
    int64_t now = 1'000'000'000;
    mcsink::osd_overlay osd(counters, [&] { return now; });
    osd.open();
    const auto caps = caps_1080p();
    osd.input(make_caps_pdu(sdu_type_e::CAPS_VIDEO_RAW, caps, 0, 0));
    component_pdu out;
    osd.output(out);
    for (int i = 0; i < 40; ++i)
    {
        now += 33'000'000;
        CHECK(osd.input(frame(caps, 0, 100, 1080)) == 0);
        CHECK(osd.output(out) == 0);
    }
    std::string draw;
    CHECK(osd.query("draw", &draw) == 0);
    std::printf("osd draw: %s\n", draw.c_str());
    CHECK(draw.rfind("drawn=40 skipped=0 ", 0) == 0);
    const uint8_t *y = out.sdu.u8();
    size_t white = 0;
    for (int r = 0; r < 120; ++r)
        for (int c = 0; c < 1200; ++c)
            white += y[size_t(r) * 1920 + size_t(c)] == 235;
    CHECK(white > 1000);
    // Below the box the last luma row is untouched.
    CHECK(y[size_t(1079) * 1920 + 10] == 100);
}

// Synthetic clock only: verify 5 s rollover, rate shortfall and stream reset.
void test_rolling_metrics()
{
    int64_t now = 0;
    mcsink::osd_overlay osd({}, [&] { return now; });
    CHECK(osd.configure("enabled", "0") == 0);
    CHECK(osd.configure("fps", "nan") == -EINVAL);
    CHECK(osd.configure("fps", "0") == -EINVAL);
    CHECK(osd.configure("fps", "30") == 0);
    CHECK(osd.open() == 0);
    component_pdu out;
    auto tick = [&] {
        component_pdu f;
        f.sdu_type = sdu_type_e::NV12;
        CHECK(osd.input(std::move(f)) == 0);
        CHECK(osd.output(out) == 0);
    };
    // 28 fps over 5 seconds. Force a refresh at the exact boundary.
    for (int i = 1; i <= 139; ++i) { now = int64_t(i) * 5'000'000'000 / 140; tick(); }
    now = 5'000'000'000;
    // Existing refreshes are slightly offset; query at the next exact refresh below.
    tick();
    now = 6'100'000'000; tick();
    std::string error, jitter;
    CHECK(osd.query("decode_error_fps", &error) == 0);
    CHECK(std::stod(error) > 7); // last five seconds include a 1.1 s gap
    CHECK(osd.query("frame_pacing_error_ms", &jitter) == 0);
    CHECK(std::stod(jitter) > 10);
    // After the gap expires, perfectly even intervals recover to zero jitter.
    for (int i = 1; i <= 180; ++i) { now = 6'100'000'000 + int64_t(i) * 40'000'000; tick(); }
    CHECK(osd.query("frame_pacing_error_ms", &jitter) == 0 && jitter == "0.000");
    CHECK(osd.query("decode_error_fps", &error) == 0 && error == "5.000");
    now += 6'000'000'000;
    CHECK(osd.configure("refresh", "1") == 0);
    CHECK(osd.query("decode_error_fps", &error) == 0 && error == "30.000");
    CHECK(osd.query("frame_pacing_error_ms", &jitter) == 0 && jitter == "-1.000");
    CHECK(osd.open() == 0);
    std::string text;
    CHECK(osd.query("text", &text) == 0 && text == "frame --- fps\nerror --- fps\njitter --- ms\nlatency --- ms");
}

void test_known_pacing_deviation()
{
    int64_t now = 0;
    mcsink::osd_overlay osd({}, [&] { return now; });
    osd.configure("enabled", "0");
    osd.open();
    component_pdu out;
    for (int i = 0; i <= 34; ++i) {
        if (i) now += i % 2 ? 20'000'000 : 40'000'000;
        component_pdu f;
        f.sdu_type = sdu_type_e::NV12;
        CHECK(osd.input(std::move(f)) == 0);
        CHECK(osd.output(out) == 0);
    }
    std::string jitter;
    CHECK(osd.query("frame_pacing_error_ms", &jitter) == 0 && jitter == "10.000");
}

void test_multiline_font()
{
    auto caps = caps_1080p();
    std::vector<uint8_t> data(size_t(caps.hor_stride) * caps.ver_stride * 3 / 2, 100);
    CHECK(mcsink::text_width("frame\nlatency", 2) == mcsink::text_width("latency", 2));
    CHECK(mcsink::draw_text_nv12(data.data(), data.size(), caps, 20, 20, 2, 2,
                                "frame\nerror\njitter\nlatency"));
    for (int line = 0; line < 4; ++line) {
        int white = 0;
        for (int y = 20 + line * 16; y < 34 + line * 16; ++y)
            for (int x = 20; x < 110; ++x) white += data[size_t(y) * caps.hor_stride + x] == 235;
        CHECK(white > 0);
    }
    CHECK(data[size_t(100) * caps.hor_stride + 20] == 100);
}

void test_draw_rejects_small_buffer()
{
    const auto caps = caps_1080p();
    std::vector<uint8_t> small(1000);
    CHECK(!mcsink::draw_text_nv12(small.data(), small.size(), caps, 10, 10, 4, 8, "12"));
}

}  // namespace

int main()
{
    test_known_pacing_deviation();
    test_rolling_metrics();
    test_multiline_font();
    test_text_and_latency();
    test_screen_latency();
    test_no_capture_time();
    test_packed_frame();
    test_draw_rejects_small_buffer();
    if (failures)
    {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("mcsink_tests passed\n");
    return 0;
}
