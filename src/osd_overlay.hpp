#ifndef MCSINK_OSD_OVERLAY_HPP
#define MCSINK_OSD_OVERLAY_HPP

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "core/component_coder.hpp"
#include "core/component_pdu.hpp"
#include "core/sdu_caps.hpp"
#include "mcsink_stats.hpp"

namespace mcsink
{

/* Four-line decoder fps, decode error fps, 5 s pacing jitter and screen latency OSD.
 * Pacing is mean absolute interval deviation from the rolling mean interval.
 * Bitrate and camera-to-decode latency remain available through query().
 */
class osd_overlay : public vstreamer::component_coder
{
public:
    using clock_ns = std::function<int64_t()>;
    /* now_ns: the monotonic clock ts_us is on (steady_mono_ns by default). */
    explicit osd_overlay(std::shared_ptr<stats> counters, clock_ns now_ns = {});

    std::string name() const override { return "osd_overlay"; }
    int  configure(std::string_view key, std::string_view value) override;
    int  query(std::string_view key, std::string *value) const override;
    int  open() override;
    void close() override;
    int  input(vstreamer::component_pdu &&in) override;
    int  output(vstreamer::component_pdu &out) override;

private:
    void refresh_locked(int64_t now);

    std::shared_ptr<stats> stats_;
    clock_ns               now_ns_;

    mutable std::mutex                    mu_;
    std::deque<vstreamer::component_pdu> out_;
    vstreamer::video_raw_caps            caps_ {};
    bool                                  have_caps_ = false;
    // Frames the OSD was drawn on / not drawn on (no caps, or frame smaller
    // than the caps' strides), and the size of the last frame.
    uint64_t drawn_ = 0, skipped_ = 0;
    size_t   last_frame_bytes_ = 0;
    bool                                  enabled_ = true;
    std::string                           text_ = "frame --- fps\nerror --- fps\njitter --- ms\nlatency --- ms";
    // Current window.
    int64_t  window_start_ns_ = 0;
    uint64_t window_bytes_ = 0;
    uint64_t window_frames_ = 0;
    uint64_t window_latency_n_ = 0;
    double   window_latency_sum_ms_ = 0;
    struct interval { int64_t end_ns; double ms; };
    std::deque<interval> intervals_;
    std::deque<int64_t> frame_times_;
    int64_t metrics_start_ns_ = 0;
    double nominal_fps_ = 30;
    double decode_error_fps_ = 0;
    int64_t last_frame_ns_ = -1;
    double pacing_error_ms_ = -1;
    // Last refresh.
    uint64_t kbps_ = 0;
    double   fps_ = 0;
    double   latency_ms_ = -1;
    // Capture -> on screen (the display's last 1 s window), set by the app;
    // < 0 = unknown and not shown.
    double   screen_latency_ms_ = -1;
};

}  // namespace mcsink

#endif  // MCSINK_OSD_OVERLAY_HPP
