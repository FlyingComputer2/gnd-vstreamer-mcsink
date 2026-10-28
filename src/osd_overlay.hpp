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

/*
 * NV12 pass-through that burns an OSD line into each decoded frame, top left:
 *   "<kbps> kbps, <latency.xxx> ms, <fps> fps"
 * refreshed once a second from the window since the last refresh:
 *   kbps     source datagram bytes (RTP as received) per second
 *   latency  mean camera-to-decode: local monotonic now minus the frame's ts_us, which
 *            rtp_h264_depay set from the sender's CLOCK_REALTIME capture stamp; "---"
 *            when no frame in the window had one (ts_us 0)
 *   fps      frames through this node per second (decoder output rate)
 * The latency is only as good as the agreement between the sender's CLOCK_REALTIME and
 * this host's.
 *
 * Configure: enabled (1|0, default 1). Query: text (the current line), kbps, latency_ms
 * (-1 when the window had no capture time), fps.
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
    std::string                           text_ = "--- kbps, --- ms, --- fps";
    // Current window.
    int64_t  window_start_ns_ = 0;
    uint64_t window_bytes_ = 0;
    uint64_t window_frames_ = 0;
    uint64_t window_latency_n_ = 0;
    double   window_latency_sum_ms_ = 0;
    // Last refresh.
    uint64_t kbps_ = 0;
    double   fps_ = 0;
    double   latency_ms_ = -1;
};

}  // namespace mcsink

#endif  // MCSINK_OSD_OVERLAY_HPP
