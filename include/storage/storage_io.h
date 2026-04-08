#pragma once
#include "base.h"

namespace kio::storage
{
class StorageIO : public IStorage
{
public:
    Task<Result<FD>> Open(std::filesystem::path path, OpenOptions opts) override;
};
}  // namespace kio::storage
