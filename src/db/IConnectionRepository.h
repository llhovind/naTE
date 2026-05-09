#pragma once
#include "db/ConnectionProfile.h"
#include <string>
#include <vector>

namespace term::db {

class IConnectionRepository {
public:
    virtual ~IConnectionRepository() = default;

    virtual std::vector<ConnectionProfile> LoadAll() = 0;

    // Upsert by id: inserts if not present, updates if already present.
    virtual void Save(const ConnectionProfile& profile) = 0;

    virtual void Delete(const std::string& id) = 0;
};

} // namespace term::db
