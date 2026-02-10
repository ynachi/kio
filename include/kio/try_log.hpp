#pragma once

#include "kio/logger.hpp"
#include "kio/core/core.hpp"

#define KIO_CO_TRY_LOG(expr)                                  \
({                                                            \
auto __res = (expr);                                          \
if (!__res)                                                   \
{                                                             \
ALOG_ERROR("{} ({})",                                         \
__res.error().message(),                                      \
__res.error().value());                                       \
co_return std::unexpected(__res.error());                     \
}                                                             \
::kio::kio_try_internal::unwrap_impl(__res);                  \
})
