#pragma once
#include "db/IConnectionRepository.h"
#include <string>

namespace term::db {

class JsonConnectionRepository : public IConnectionRepository {
public:
    explicit JsonConnectionRepository(std::string path);

    std::vector<ConnectionProfile> LoadAll() override;
    void Save(const ConnectionProfile& profile) override;
    void Delete(const std::string& id) override;

private:
    std::string m_path;     // ~/.nate/connections.json
    std::string TmpPath() const;
};

} // namespace term::db
