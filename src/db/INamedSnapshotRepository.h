#pragma once
#include "session/RestoreState.h"
#include <string>
#include <vector>

namespace term::db {

class INamedSnapshotRepository {
public:
    virtual ~INamedSnapshotRepository() = default;

    virtual std::vector<std::string>    List()                                          const = 0;
    virtual bool                        Exists(const std::string& name)                 const = 0;
    virtual term::session::RestoreState Load(const std::string& name)                   const = 0;
    virtual void                        Save(const std::string& name,
                                             const term::session::RestoreState& state)        = 0;
    virtual void                        Delete(const std::string& name)                       = 0;
};

} // namespace term::db
