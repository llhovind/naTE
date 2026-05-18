#include "db/JsonNamedSnapshotRepository.h"
#include "db/JsonSessionRestoreRepository.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace term::db {

JsonNamedSnapshotRepository::JsonNamedSnapshotRepository(std::string dir)
    : m_dir(std::move(dir))
{}

std::string JsonNamedSnapshotRepository::PathFor(const std::string& name) const
{
    return m_dir + "/" + name + ".json";
}

std::vector<std::string> JsonNamedSnapshotRepository::List() const
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(m_dir, ec)) {
        if (entry.path().extension() == ".json")
            names.push_back(entry.path().stem().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool JsonNamedSnapshotRepository::Exists(const std::string& name) const
{
    return fs::exists(PathFor(name));
}

term::session::RestoreState JsonNamedSnapshotRepository::Load(const std::string& name) const
{
    return JsonSessionRestoreRepository(PathFor(name)).Load();
}

void JsonNamedSnapshotRepository::Save(const std::string& name,
                                       const term::session::RestoreState& state)
{
    std::error_code ec;
    fs::create_directories(m_dir, ec);
    JsonSessionRestoreRepository(PathFor(name)).Save(state);
}

void JsonNamedSnapshotRepository::Delete(const std::string& name)
{
    std::remove(PathFor(name).c_str());
}

} // namespace term::db
