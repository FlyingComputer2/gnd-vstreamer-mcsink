/*
 * gnd-vstreamer-mcsink: displays the ground P4's RTP H.264 multicast with an OSD.
 *
 *   rx:      multicast_source -> rtp_h264_depay -> h264_decoder_mpp -> osd_overlay -> queue
 *   display: queue -> sdl_sink (KMSDRM on the console, or a window under X/Wayland)
 *
 * The OSD shows decode fps, rate shortfall, pacing jitter and camera-to-screen latency (the MCSINK line also has
 * camera-to-decode). Latency compares the
 * rover's capture CLOCK_REALTIME (RTP header extension id 2) with this host's, so this
 * host's clock must be the one the ground P4 follows (its SNTP server) or agree with it.
 */
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "core/pipeline.hpp"
#include "core/pipeline_queue.hpp"
#include "mcsink_stats.hpp"
#include "multicast_source.hpp"
#include "osd_overlay.hpp"

using namespace vstreamer;

namespace
{

volatile std::sig_atomic_t g_running = 1;

void on_signal(int)
{
    g_running = 0;
}

void usage(const char *prog)
{
    std::fprintf(stderr,
                 "Usage: %s [options]\n"
                 "  --group ADDR          multicast group (default 239.255.42.1)\n"
                 "  --port N              UDP port (default 5600)\n"
                 "  --interface NAME|IP   multicast interface (default: routing choice)\n"
                 "  --display kmsdrm|sdl  default kmsdrm\n"
                 "  --fps N               nominal frame rate (default 30)\n"
                 "  --osd 1|0             burn the OSD into the video (default 1)\n"
                 "  --show-corrupt 1|0    show frames decoded after a loss, with artifacts, instead\n"
                 "                        of freezing until the next IDR (default 1)\n"
                 "  --stats S             seconds between stderr MCSINK lines, 0 = off (default 1)\n",
                 prog);
}

std::string q(const pipeline &p, std::string_view node, std::string_view key)
{
    std::string v;
    return p.query(node, key, &v) == 0 ? v : "?";
}

}  // namespace

int main(int argc, char **argv)
{
    std::string group = "239.255.42.1", port = "5600", iface, display_mode = "kmsdrm", fps = "30",
                osd = "1", show_corrupt = "1";
    int stats_s = 1;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 == argc)
        {
            usage(argv[0]);
            return 2;
        }
        const char *v = argv[++i];
        if (arg == "--group") group = v;
        else if (arg == "--port") port = v;
        else if (arg == "--interface") iface = v;
        else if (arg == "--display") display_mode = v;
        else if (arg == "--fps") fps = v;
        else if (arg == "--osd") osd = v;
        else if (arg == "--show-corrupt") show_corrupt = v;
        else if (arg == "--stats") stats_s = std::atoi(v);
        else
        {
            usage(argv[0]);
            return 2;
        }
    }
    if (display_mode != "kmsdrm" && display_mode != "sdl")
    {
        usage(argv[0]);
        return 2;
    }

    auto counters = std::make_shared<mcsink::stats>();
    pipeline rx("mcsink"), display("display");
    auto frames = std::make_shared<pipeline_queue>(2);
    auto check = [](const char *what, int r) {
        if (r < 0) std::fprintf(stderr, "gnd-vstreamer-mcsink: %s failed: %d\n", what, r);
        return r >= 0;
    };
    if (!check("add src", rx.add_source("src", std::make_unique<mcsink::multicast_source>(counters))) ||
        !check("add depay", rx.add_coder("depay", "rtp_h264_depay")) ||
        !check("add decode", rx.add_coder("decode", "h264_decoder_mpp")) ||
        !check("add osd", rx.add_coder("osd", std::make_unique<mcsink::osd_overlay>(counters))) ||
        !check("add frames", rx.add_queue_sink("frames", frames)) ||
        !check("link rx", rx.link_chain({"src", "depay", "decode", "osd", "frames"})) ||
        !check("add display src", display.add_queue_source("frames", frames)) ||
        !check("add screen", display.add_sink("screen", "sdl_sink")) ||
        !check("link display", display.link("frames", "screen")) ||
        !check("group", rx.configure("src", "group", group)) ||
        !check("port", rx.configure("src", "port", port)) ||
        !check("interface", rx.configure("src", "interface", iface)) ||
        !check("depay fps", rx.configure("depay", "fps", fps)) ||
        !check("decode fps", rx.configure("decode", "fps", fps)) ||
        !check("decode size", rx.configure("decode", "output_size_mode", "stream")) ||
        !check("show corrupt", rx.configure("decode", "show_corrupt", show_corrupt)) ||
        !check("osd fps", rx.configure("osd", "fps", fps)) ||
        !check("osd", rx.configure("osd", "enabled", osd)) ||
        !check("title", display.configure("screen", "title", "gnd-vstreamer-mcsink")) ||
        !check("driver", display.configure("screen", "video_driver",
                                           display_mode == "sdl" ? "auto" : "kmsdrm")))
    {
        return 2;
    }
    auto errors = [](const std::string &node, int err) {
        std::fprintf(stderr, "gnd-vstreamer-mcsink: %s failed: %d\n", node.c_str(), err);
    };
    rx.set_error_handler(errors);
    display.set_error_handler(errors);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    int r = display.start();
    if (r == 0) r = rx.start();
    if (r < 0)
    {
        std::fprintf(stderr, "gnd-vstreamer-mcsink: start failed: %d\n", r);
        return 1;
    }
    std::fprintf(stderr, "gnd-vstreamer-mcsink: %s:%s interface=%s -> depay -> MPP -> OSD -> %s\n",
                 group.c_str(), port.c_str(), iface.empty() ? "default" : iface.c_str(),
                 display_mode.c_str());
    auto next = std::chrono::steady_clock::now() + std::chrono::seconds(stats_s);
    auto next_screen = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::string screen_ms = "?";
    while (g_running && rx.state() != pipeline_state_e::ERROR &&
           display.state() != pipeline_state_e::ERROR)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // The display's capture -> on-screen latency (last 1 s window) goes into the OSD.
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_screen)
        {
            next_screen = now + std::chrono::seconds(1);
            rx.configure("osd", "refresh", "1");
            screen_ms = q(display, "screen", "present_latency_ms");
            if (screen_ms != "?") rx.configure("osd", "screen_latency_ms", screen_ms);
        }
        if (stats_s <= 0 || now < next) continue;
        next += std::chrono::seconds(stats_s);
        // key=value, one line per interval: the OSD numbers (last 1 s window) plus counters.
        std::fprintf(stderr,
                     "MCSINK kbps=%s latency_ms=%s screen_latency_ms=%s fps=%s decode_error_fps=%s frame_pacing_error_ms=%s received=%s dropped=%s rtp_lost=%s "
                     "capture_rejected=%s corrupt_frames=%s osd=\"%s\" display=\"%s\"\n",
                     q(rx, "osd", "kbps").c_str(), q(rx, "osd", "latency_ms").c_str(), screen_ms.c_str(),
                     q(rx, "osd", "fps").c_str(), q(rx, "osd", "decode_error_fps").c_str(),
                     q(rx, "osd", "frame_pacing_error_ms").c_str(), q(rx, "src", "received").c_str(),
                     q(rx, "src", "dropped").c_str(), q(rx, "depay", "rtp_lost").c_str(),
                     q(rx, "depay", "capture_ts_rejected").c_str(),
                     q(rx, "decode", "corrupt_frames").c_str(),
                     q(rx, "osd", "draw").c_str(),
                     q(display, "screen", "stats").c_str());
    }
    const bool failed =
        rx.state() == pipeline_state_e::ERROR || display.state() == pipeline_state_e::ERROR;
    rx.stop();
    display.stop();
    return failed ? 1 : 0;
}
