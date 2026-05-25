#pragma once

#include <memory>

#include "cache.hpp"
#include "types.hpp"
#include "uring/fd.hpp"

namespace bitcask
{
/// Fdcache using a shared ptr of FD. On put, the cache would return
/// the evicted Fd. We advise the caller to check and close the Fd
/// asynchronously if they are the sole owner.
using FdCache = Cache<SegmentId, std::shared_ptr<URing::Fd>>;
}  // namespace bitcask