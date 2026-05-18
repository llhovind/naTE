#pragma once
#include "db/ISessionRestoreRepository.h"
#include <string>

namespace term::db {

class JsonSessionRestoreRepository : public ISessionRestoreRepository {
public:
    explicit JsonSessionRestoreRepository(std::string path);

    bool HasSnapshot() const override;
    term::session::RestoreState Load() const override;
    void Save(const term::session::RestoreState& state) override;
    void Delete() override;

private:
    std::string TmpPath() const;

    std::string m_path;
};

} // namespace term::db
