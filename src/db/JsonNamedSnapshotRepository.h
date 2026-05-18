#pragma once
#include "db/INamedSnapshotRepository.h"
#include <string>

namespace term::db {

class JsonNamedSnapshotRepository : public INamedSnapshotRepository {
public:
    explicit JsonNamedSnapshotRepository(std::string dir);

    std::vector<std::string>    List()                                          const override;
    bool                        Exists(const std::string& name)                 const override;
    term::session::RestoreState Load(const std::string& name)                   const override;
    void                        Save(const std::string& name,
                                     const term::session::RestoreState& state)        override;
    void                        Delete(const std::string& name)                       override;

private:
    std::string PathFor(const std::string& name) const;

    std::string m_dir;
};

} // namespace term::db
