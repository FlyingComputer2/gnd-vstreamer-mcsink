#include "multicast_source.hpp"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/port_caps.hpp"

namespace mcsink
{

using namespace vstreamer;

namespace
{

const std::vector<port_desc> &output_ports()
{
    static const std::vector<port_desc> ports = [] {
        port_desc p;
        port_caps_entry caps {};
        caps.sdu_type = sdu_type_e::RTP;
        p.caps.push_back(caps);
        return std::vector<port_desc> {p};
    }();
    return ports;
}

bool parse_size(std::string_view v, size_t lo, size_t hi, size_t *out)
{
    size_t n = 0;
    if (v.empty())
    {
        return false;
    }
    for (char c : v)
    {
        if (c < '0' || c > '9' || n > hi)
        {
            return false;
        }
        n = n * 10 + size_t(c - '0');
    }
    if (n < lo || n > hi)
    {
        return false;
    }
    *out = n;
    return true;
}

/* Interface by name or IPv4 address; INADDR_ANY when empty. */
bool interface_addr(int fd, const std::string &iface, in_addr *out)
{
    out->s_addr = htonl(INADDR_ANY);
    if (iface.empty() || inet_pton(AF_INET, iface.c_str(), out) == 1)
    {
        return true;
    }
    ifreq ifr {};
    std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFADDR, &ifr) != 0)
    {
        return false;
    }
    *out = reinterpret_cast<sockaddr_in *>(&ifr.ifr_addr)->sin_addr;
    return true;
}

}  // namespace

multicast_source::multicast_source(std::shared_ptr<stats> counters) : stats_(std::move(counters)) {}

multicast_source::~multicast_source()
{
    close();
}

int multicast_source::configure(std::string_view key, std::string_view value)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_ >= 0)
    {
        return -EBUSY;
    }
    size_t n = 0;
    if (key == "group")
    {
        in_addr a {};
        if (inet_pton(AF_INET, std::string(value).c_str(), &a) != 1 || !IN_MULTICAST(ntohl(a.s_addr)))
        {
            return -EINVAL;
        }
        group_ = value;
        return 0;
    }
    if (key == "interface")
    {
        interface_ = value;
        return 0;
    }
    if (key == "port")
    {
        if (!parse_size(value, 1, 65535, &n))
        {
            return -EINVAL;
        }
        port_ = int(n);
        return 0;
    }
    if (key == "max_datagram")
    {
        if (!parse_size(value, 64, 65507, &n))
        {
            return -EINVAL;
        }
        max_datagram_ = n;
        return 0;
    }
    if (key == "queue_depth")
    {
        if (!parse_size(value, 1, 65536, &n))
        {
            return -EINVAL;
        }
        queue_depth_ = n;
        return 0;
    }
    return -ENOTSUP;
}

int multicast_source::query(std::string_view key, std::string *value) const
{
    if (!value)
    {
        return -EINVAL;
    }
    const int r = port_caps_query(output_ports(), false, key, value);
    if (r != -ENOTSUP)
    {
        return r;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (key == "received") *value = std::to_string(stats_->datagrams.load());
    else if (key == "dropped") *value = std::to_string(stats_->dropped.load());
    else if (key == "group") *value = group_;
    else if (key == "interface") *value = interface_;
    else if (key == "port") *value = std::to_string(port_);
    else if (key == "max_datagram") *value = std::to_string(max_datagram_);
    else if (key == "queue_depth") *value = std::to_string(queue_depth_);
    else return -ENOTSUP;
    return 0;
}

int multicast_source::open()
{
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_ >= 0)
    {
        return 0;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        return -errno;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    const int rcvbuf = 4 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port_));
    inet_pton(AF_INET, group_.c_str(), &addr.sin_addr);
    ip_mreq mreq {};
    mreq.imr_multiaddr = addr.sin_addr;
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        !interface_addr(fd, interface_, &mreq.imr_interface) ||
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0)
    {
        const int err = errno ? -errno : -EINVAL;
        std::fprintf(stderr, "multicast_source: %s:%d interface=%s: %s\n", group_.c_str(), port_,
                     interface_.c_str(), std::strerror(-err));
        ::close(fd);
        return err;
    }
    fd_ = fd;
    stop_.store(false);
    queue_.clear();
    reader_ = std::thread([this, fd] { run(fd); });
    return 0;
}

void multicast_source::close()
{
    std::thread reader;
    int         fd = -1;
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_.store(true);
        reader = std::move(reader_);
        fd = fd_;
        fd_ = -1;
    }
    if (reader.joinable())
    {
        reader.join();
    }
    if (fd >= 0)
    {
        ::close(fd);
    }
    std::lock_guard<std::mutex> lock(mu_);
    queue_.clear();
}

void multicast_source::run(int fd)
{
    std::vector<uint8_t> buf(max_datagram_ + 1);
    while (!stop_.load())
    {
        pollfd p {fd, POLLIN, 0};
        if (poll(&p, 1, 100) <= 0)
        {
            continue;
        }
        const ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0 || size_t(n) > max_datagram_)
        {
            continue;
        }
        component_pdu pdu;
        pdu.sdu_type = sdu_type_e::RTP;
        pdu.sdu = bfc::shared_sized_buffer::copy_from(buf.data(), size_t(n));
        pdu.ts_us = 0;
        stats_->datagrams.fetch_add(1);
        stats_->bytes.fetch_add(uint64_t(n));
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (queue_.size() >= queue_depth_)
            {
                queue_.pop_front();
                stats_->dropped.fetch_add(1);
            }
            queue_.push_back(std::move(pdu));
        }
        notify_wakeup();
    }
}

int multicast_source::output(component_pdu &out)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_ < 0)
    {
        return -ENODEV;
    }
    if (queue_.empty())
    {
        return -EAGAIN;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return 0;
}

}  // namespace mcsink
