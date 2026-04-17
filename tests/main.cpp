#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>
#include <wx/init.h>

int main(int argc, char* argv[]) {
    // wxBase init — no GUI needed; required for wx string/file classes
    wxInitializer wx;
    if (!wx.IsOk()) return 1;
    return Catch::Session().run(argc, argv);
}
