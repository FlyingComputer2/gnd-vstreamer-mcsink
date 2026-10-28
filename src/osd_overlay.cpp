#include "osd_overlay.hpp"

#include <cerrno>
#include <cstdio>
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
    else if (key == "latency_ms") { std::snprintf(buf, sizeof(buf), "%.3f", latency_ms_); *value = buf; }
    else if (key == "enabled") *value = enabled_ ? "1" : "0";
    else return -ENOTSUP;
    return 0;
}

int osd_overlay::open()
{
    std::lock_guard<std::mutex> lock(mu_);
    out_.clear();
    have_caps_ = false;
    window_start_ns_ = now_ns_();
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
    char latency[32] = "---";
    if (latency_ms_ >= 0)
    {
        std::snprintf(latency, sizeof(latency), "%.3f", latency_ms_);
    }
    char line[96];
    std::snprintf(line, sizeof(line), "%llu kbps, %s ms, %.1f fps",
                  static_cast<unsigned long long>(kbps_), latency, fps_);
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
    // Close the window before counting this frame, so it covers whole frame intervals.
    if (now - window_start_ns_ >= k_refresh_ns)
    {
        refresh_locked(now);
    }
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
    if (enabled_ && have_caps_)
    {
        const int scale = std::max(2, caps_.height / 270);
        draw_text_nv12(in.sdu.u8(), in.sdu.size(), caps_, 8 * scale, 8 * scale, scale, 3 * scale,
                       text_);
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
