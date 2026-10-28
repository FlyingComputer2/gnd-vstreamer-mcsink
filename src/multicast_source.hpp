#ifndef MCSINK_MULTICAST_SOURCE_HPP
#define MCSINK_MULTICAST_SOURCE_HPP

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/component_pdu.hpp"
#include "core/component_source.hpp"
#include "mcsink_stats.hpp"

namespace mcsink
{

/*
 * RTP datagrams from an IPv4 multicast group, one PDU (sdu_type RTP) each.
 * A reader thread fills a bounded queue (oldest dropped when full) and wakes the pipeline.
 * PDUs carry ts_us 0: rtp_h264_depay then stamps each AU with its capture time from the
 * RTP header extension, or leaves 0 when that is missing or implausible, so a frame
 * without a real capture time never counts toward latency.
 *
 * Configure (before open): group (239.255.42.1), port (5600), interface (name or IPv4,
 * empty = routing default), max_datagram (2048), queue_depth (512).
 * Query: received, dropped, plus the configure keys.
 */
class multicast_source : public vstreamer::component_source
{
public:
    explicit multicast_source(std::shared_ptr<stats> counters);
    ~multicast_source() override;

    std::string name() const override { return "multicast_source"; }
    int  configure(std::string_view key, std::string_view value) override;
    int  query(std::string_view key, std::string *value) const override;
    int  open() override;
    void close() override;
    int  output(vstreamer::component_pdu &out) override;

private:
    void run(int fd);

    std::shared_ptr<stats> stats_;
    std::string group_ = "239.255.42.1";
    std::string interface_;
    int         port_ = 5600;
    size_t      max_datagram_ = 2048;
    size_t      queue_depth_ = 512;

    mutable std::mutex                    mu_;
    std::deque<vstreamer::component_pdu> queue_;
    std::thread                           reader_;
    std::atomic<bool>                     stop_ {false};
    int                                   fd_ = -1;
};

}  // namespace mcsink

#endif  // MCSINK_MULTICAST_SOURCE_HPP
