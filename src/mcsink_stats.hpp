#ifndef MCSINK_STATS_HPP
#define MCSINK_STATS_HPP

#include <atomic>
#include <cstdint>

namespace mcsink
{

/* Counters the source writes and the OSD reads. */
struct stats
{
    std::atomic<uint64_t> datagrams {0};
    std::atomic<uint64_t> bytes {0};
    std::atomic<uint64_t> dropped {0};
};

}  // namespace mcsink

#endif  // MCSINK_STATS_HPP
