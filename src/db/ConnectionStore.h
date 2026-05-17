#pragma once
#include "db/ConnectionProfile.h"
#include "db/IConnectionRepository.h"
#include "session/Connection.h"
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace term::db {

class ConnectionStore {
public:
    explicit ConnectionStore(std::unique_ptr<IConnectionRepository> repo);

    // Returns all saved profiles, ordered by insertion time.
    const std::vector<ConnectionProfile>& GetAll() const;

    // Creates a new profile with a generated UUID and current timestamp.
    const ConnectionProfile& Add(const std::string& name,
                                 const term::session::TransportDesc& transport,
                                 bool wrapMode,
                                 unsigned short columnWidth,
                                 unsigned short rows = 24,
                                 term::session::SessionInit sessionInit = {},
                                 std::string profileTitle = {},
                                 bool useProfileTitle = false);

    // Replaces the profile with profile.id; no-op if id not found.
    void Update(const ConnectionProfile& profile);

    // Removes the profile with the given id; no-op if not found.
    void Remove(const std::string& id);

    // Updates lastUsed timestamp for the profile with the given id.
    void UpdateLastUsed(const std::string& id);

private:
    static std::string GenerateId();

    std::unique_ptr<IConnectionRepository> m_repo;
    std::vector<ConnectionProfile>         m_profiles;
};

} // namespace term::db
