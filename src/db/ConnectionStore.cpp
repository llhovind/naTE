#include "db/ConnectionStore.h"
#include <algorithm>
#include <iomanip>
#include <random>
#include <sstream>

namespace term::db {

ConnectionStore::ConnectionStore(std::unique_ptr<IConnectionRepository> repo)
    : m_repo(std::move(repo))
{
    m_profiles = m_repo->LoadAll();
}

const std::vector<ConnectionProfile>& ConnectionStore::GetAll() const
{
    return m_profiles;
}

const ConnectionProfile& ConnectionStore::Add(const std::string& name,
                                               const term::session::TransportDesc& transport,
                                               bool wordWrap,
                                               unsigned short columnWidth)
{
    ConnectionProfile p;
    p.id          = GenerateId();
    p.name        = name;
    p.transport   = transport;
    p.wordWrap    = wordWrap;
    p.columnWidth = columnWidth;
    p.createdAt   = std::time(nullptr);
    p.lastUsed    = 0;

    m_repo->Save(p);
    m_profiles.push_back(p);
    return m_profiles.back();
}

void ConnectionStore::Update(const ConnectionProfile& profile)
{
    auto it = std::find_if(m_profiles.begin(), m_profiles.end(),
                           [&](const ConnectionProfile& p){ return p.id == profile.id; });
    if (it == m_profiles.end())
        return;

    *it = profile;
    m_repo->Save(profile);
}

void ConnectionStore::Remove(const std::string& id)
{
    m_profiles.erase(
        std::remove_if(m_profiles.begin(), m_profiles.end(),
                       [&](const ConnectionProfile& p){ return p.id == id; }),
        m_profiles.end());
    m_repo->Delete(id);
}

void ConnectionStore::UpdateLastUsed(const std::string& id)
{
    auto it = std::find_if(m_profiles.begin(), m_profiles.end(),
                           [&](const ConnectionProfile& p){ return p.id == id; });
    if (it == m_profiles.end())
        return;

    it->lastUsed = std::time(nullptr);
    m_repo->Save(*it);
}

std::string ConnectionStore::GenerateId()
{
    static std::mt19937_64 rng{std::random_device{}()};
    static std::uniform_int_distribution<uint64_t> dist;

    const uint64_t hi = dist(rng);
    const uint64_t lo = dist(rng);

    // Format as UUID v4: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    // Set version bits (4) and variant bits (10xx)
    const uint64_t a = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    const uint64_t b = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(8)  << ((a >> 32) & 0xFFFFFFFF) << '-'
        << std::setw(4)  << ((a >> 16) & 0xFFFF)     << '-'
        << std::setw(4)  << (a         & 0xFFFF)      << '-'
        << std::setw(4)  << ((b >> 48) & 0xFFFF)      << '-'
        << std::setw(12) << (b         & 0xFFFFFFFFFFFFULL);
    return oss.str();
}

} // namespace term::db
