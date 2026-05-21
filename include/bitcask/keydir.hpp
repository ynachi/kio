#pragma once
#include "absl/container/flat_hash_map.h"

#include <optional>

#include "types.hpp"

namespace bitcask
{
class KeyDir
{
public:
    std::optional<ValueLocation> get(KeyView key) const noexcept;
    void                    put(KeyView key, ValueLocation loc);
    void                    del(KeyView key);

private:
    absl::flat_hash_map<std::string, ValueLocation> map_;
};
}