#include "kio/executor.hpp"

#include <unistd.h>

namespace kio::detail
{

void SplicePipePool::Lease::recycle()
{
    if (read_fd != -1)
    {
        return_to_pool(read_fd, write_fd);
        read_fd = -1;
        write_fd = -1;
    }
}

}  // namespace kio::detail