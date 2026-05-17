#pragma once
#include "session/EnvVar.h"
#include <string>
#include <vector>

namespace term::session {

// App-wide defaults for session initialisation, constructed from AppConfig
// and passed through to all transport constructors.
struct AppSessionDefaults {
    std::string         workingDir;
    std::vector<EnvVar> envVars;
    std::string         envFilePath;
    bool                loginShell = false;
};

} // namespace term::session
