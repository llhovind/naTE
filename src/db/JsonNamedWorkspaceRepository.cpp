#include "db/JsonNamedWorkspaceRepository.h"
#include "db/JsonSessionRestoreRepository.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace term::db {

JsonNamedWorkspaceRepository::JsonNamedWorkspaceRepository(std::string dir)
    : m_dir(std::move(dir))
{}

std::string JsonNamedWorkspaceRepository::PathFor(const std::string& name) const
{
    return m_dir + "/" + name + ".json";
}

std::vector<std::string> JsonNamedWorkspaceRepository::List() const
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

bool JsonNamedWorkspaceRepository::Exists(const std::string& name) const
{
    return fs::exists(PathFor(name));
}

term::session::RestoreState JsonNamedWorkspaceRepository::Load(const std::string& name) const
{
    return JsonSessionRestoreRepository(PathFor(name)).Load();
}

void JsonNamedWorkspaceRepository::Save(const std::string& name,
                                        const term::session::RestoreState& state)
{
    std::error_code ec;
    fs::create_directories(m_dir, ec);
    JsonSessionRestoreRepository(PathFor(name)).Save(state);
}

void JsonNamedWorkspaceRepository::Delete(const std::string& name)
{
    std::remove(PathFor(name).c_str());
}

} // namespace term::db
