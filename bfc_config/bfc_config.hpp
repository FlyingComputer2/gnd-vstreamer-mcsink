#pragma once
// bfc config for this host receiver (vstreamer needs the top-level project to
// supply one through BFC_CONFIG_DIR). Same choices as vstreamer's host default.
#include <functional>
#include <memory>

#define BFC_CONFIG_ID "gnd-vstreamer-mcsink"
#define BFC_CONFIG_REACTOR_EPOLL 1

namespace bfc::config
{
template <typename Signature>
using function = std::function<Signature>;
template <typename T>
using allocator = std::allocator<T>;
} // namespace bfc::config
