#include "osd_overlay.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/port_caps.hpp"
#include "core/time_util.hpp"
#include "osd_font.hpp"

namespace mcsink
{

using namespace vstreamer;

namespace
{

constexpr int64_t k_refresh_ns = 1'000'000'000;

const std::vector<port_desc> &nv12_ports()
{
    static const std::vector<port_desc> ports = [] {
        port_desc p;
        port_caps_entry caps {};
        caps.sdu_type = sdu_type_e::CAPS_VIDEO_RAW;
        p.caps.push_back(caps);
        port_caps_entry data {};
        data.sdu_type = sdu_type_e::NV12;
        p.caps.push_back(data);
        return std::vector<port_desc> {p};
    }();
    return ports;
}

}  // namespace

osd_overlay::osd_overlay(std::shared_ptr<stats> counters, clock_ns now_ns)
    : stats_(std::move(counters)), now_ns_(now_ns ? std::move(now_ns) : clock_ns(steady_mono_ns))
{
}

int osd_overlay::configure(std::string_view key, std::string_view value)
{
    if (key == "refresh")
    {
        std::lock_guard<std::mutex> lock(mu_);
        const int64_t now = now_ns_();
        if (now - window_start_ns_ >= k_refresh_ns) refresh_locked(now);
        return 0;
    }
    if (key == "enabled")
    {
        if (value != "0" && value != "1")
        {
            return -EINVAL;
        }
        std::lock_guard<std::mutex> lock(mu_);
        enabled_ = value == "1";
        return 0;
    }
    if (key == "fps")
    {
        char *end = nullptr;
        const std::string v(value);
        const double fps = std::strtod(v.c_str(), &end);
        if (v.empty() || *end || !std::isfinite(fps) || fps <= 0) return -EINVAL;
        std::lock_guard<std::mutex> lock(mu_);
        nominal_fps_ = fps;
        return 0;
    }
    if (key == "screen_latency_ms")
    {
        char *end = nullptr;
        const std::string v(value);
        const double ms = std::strtod(v.c_str(), &end);
        if (v.empty() || *end)
        {
            return -EINVAL;
        }
        std::lock_guard<std::mutex> lock(mu_);
        screen_latency_ms_ = ms;
        return 0;
    }
    return -ENOTSUP;
}

int osd_overlay::query(std::string_view key, std::string *value) const
{
    if (!value)
    {
        return -EINVAL;
    }
    int r = port_caps_query(nv12_ports(), true, key, value);
    if (r == 0 || r == -EINVAL)
    {
        return r;
    }
    r = port_caps_query(nv12_ports(), false, key, value);
    if (r == 0 || r == -EINVAL)
    {
        return r;
    }
    std::lock_guard<std::mutex> lock(mu_);
    char buf[64];
    if (key == "text") *value = text_;
    else if (key == "kbps") *value = std::to_string(kbps_);
    else if (key == "fps") { std::snprintf(buf, sizeof(buf), "%.2f", fps_); *value = buf; }
    else if (key == "decode_error_fps") { std::snprintf(buf, sizeof(buf), "%.3f", decode_error_fps_); *value = buf; }
    else if (key == "frame_pacing_error_ms") { std::snprintf(buf, sizeof(buf), "%.3f", pacing_error_ms_); *value = buf; }
    else if (key == "latency_ms") { std::snprintf(buf, sizeof(buf), "%.3f", latency_ms_); *value = buf; }
    else if (key == "enabled") *value = enabled_ ? "1" : "0";
    else if (key == "draw")
    {
        char d[160];
        std::snprintf(d, sizeof(d), "drawn=%llu skipped=%llu caps=%s%dx%d stride=%dx%d frame_bytes=%zu",
                      (unsigned long long)drawn_, (unsigned long long)skipped_, have_caps_ ? "" : "none:",
                      caps_.width, caps_.height, caps_.hor_stride, caps_.ver_stride, last_frame_bytes_);
        *value = d;
    }
    else return -ENOTSUP;
    return 0;
}

int osd_overlay::open()
{
    std::lock_guard<std::mutex> lock(mu_);
    out_.clear();
    have_caps_ = false;
    intervals_.clear();
    frame_times_.clear();
    last_frame_ns_ = -1;
    pacing_error_ms_ = -1;
    window_start_ns_ = now_ns_();
    metrics_start_ns_ = window_start_ns_;
    fps_ = 0; kbps_ = 0; latency_ms_ = -1; screen_latency_ms_ = -1;
    decode_error_fps_ = 0;
    text_ = "frame --- fps\nerror --- fps\njitter --- ms\nlatency --- ms";
    window_bytes_ = stats_ ? stats_->bytes.load() : 0;
    window_frames_ = 0;
    window_latency_n_ = 0;
    window_latency_sum_ms_ = 0;
    return 0;
}

void osd_overlay::close()
{
    std::lock_guard<std::mutex> lock(mu_);
    out_.clear();
}

void osd_overlay::refresh_locked(int64_t now)
{
    const double   secs = double(now - window_start_ns_) / 1e9;
    const uint64_t bytes = stats_ ? stats_->bytes.load() : 0;
    kbps_ = secs > 0 ? uint64_t(double(bytes - window_bytes_) * 8 / 1000 / secs + 0.5) : 0;
    fps_ = secs > 0 ? double(window_frames_) / secs : 0;
    latency_ms_ = window_latency_n_ ? window_latency_sum_ms_ / double(window_latency_n_) : -1;
    while (!intervals_.empty() && intervals_.front().end_ns <= now - 5'000'000'000)
        intervals_.pop_front();
    while (!frame_times_.empty() && frame_times_.front() <= now - 5'000'000'000)
        frame_times_.pop_front();
    const double metric_secs = std::min(5.0, double(now - metrics_start_ns_) / 1e9);
    decode_error_fps_ = metric_secs > 0 ? std::max(0.0, nominal_fps_ - frame_times_.size() / metric_secs) : 0;
    pacing_error_ms_ = -1;
    if (!intervals_.empty()) {
        double mean = 0, error = 0;
        for (const auto &i : intervals_) mean += i.ms;
        mean /= intervals_.size();
        for (const auto &i : intervals_) error += std::abs(i.ms - mean);
        pacing_error_ms_ = error / intervals_.size();
    }
    char jitter[32] = "---";
    if (pacing_error_ms_ >= 0) std::snprintf(jitter, sizeof(jitter), "%.1f", pacing_error_ms_);
    char glass[32] = "---";
    if (screen_latency_ms_ >= 0)
    {
        std::snprintf(glass, sizeof(glass), "%.1f", screen_latency_ms_);
    }
    // Camera-to-decode latency (latency_ms_) stays in the MCSINK line.
    char line[160];
    std::snprintf(line, sizeof(line), "frame %.1f fps\nerror %.1f fps\njitter %s ms\nlatency %s ms",
                  fps_, decode_error_fps_, jitter, glass);
    text_ = line;
    window_start_ns_ = now;
    window_bytes_ = bytes;
    window_frames_ = 0;
    window_latency_n_ = 0;
    window_latency_sum_ms_ = 0;
}

int osd_overlay::input(component_pdu &&in)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (in.sdu_type == sdu_type_e::CAPS_VIDEO_RAW)
    {
        video_raw_caps caps {};
        if (read_caps(in, &caps) < 0)
        {
            return -EINVAL;
        }
        caps_ = caps;
        have_caps_ = true;
        out_.push_back(std::move(in));
        notify_wakeup();
        return 0;
    }
    if (in.sdu_type != sdu_type_e::NV12)
    {
        return -EINVAL;
    }
    const int64_t now = now_ns_();
    frame_times_.push_back(now);
    if (last_frame_ns_ >= 0)
        intervals_.push_back({now, double(now - last_frame_ns_) / 1e6});
    last_frame_ns_ = now;
    while (!intervals_.empty() && intervals_.front().end_ns <= now - 5'000'000'000)
        intervals_.pop_front();
    // Close the window before counting this frame, so it covers whole frame intervals.
    if (now - window_start_ns_ >= k_refresh_ns)
    {
        refresh_locked(now);
    }
    while (!frame_times_.empty() && frame_times_.front() <= now - 5'000'000'000)
        frame_times_.pop_front();
    ++window_frames_;
    if (in.ts_us > 0)
    {
        const double ms = double(now - int64_t(in.ts_us) * 1000) / 1e6;
        if (ms >= 0 && ms < 10'000)
        {
            window_latency_sum_ms_ += ms;
            ++window_latency_n_;
        }
    }
    last_frame_bytes_ = in.sdu.size();
    if (enabled_)
    {
        const int scale = std::max(2, caps_.height / 270);
        // The MPP decoder delivers frames packed to the visible height while
        // its caps keep the coded vertical stride (1080 vs 1088 rows), so take
        // the plane height from the frame size when it is the smaller one.
        video_raw_caps layout = caps_;
        if (layout.hor_stride > 0)
        {
            const size_t rows = in.sdu.size() * 2 / 3 / size_t(layout.hor_stride);
            if (rows < size_t(layout.ver_stride) && rows >= size_t(layout.height))
                layout.ver_stride = int(rows);
        }
        if (have_caps_ && draw_text_nv12(in.sdu.u8(), in.sdu.size(), layout, 8 * scale, 8 * scale,
                                         scale, 3 * scale, text_))
            ++drawn_;
        else
            ++skipped_;
    }
    out_.push_back(std::move(in));
    notify_wakeup();
    return 0;
}

int osd_overlay::output(component_pdu &out)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (out_.empty())
    {
        return -EAGAIN;
    }
    out = std::move(out_.front());
    out_.pop_front();
    return 0;
}

}  // namespace mcsink
