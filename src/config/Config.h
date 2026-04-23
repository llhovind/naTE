#pragma once
#include <wx/colour.h>
#include <wx/string.h>

struct AppConfig {
    int      columns       = 80;
    int      rows          = 24;
    int      fontSize      = 12;
    int      scrollbackLines = 100'000;
    wxColour textColour{0, 0, 0};
    wxColour bgColour{255, 255, 204};

    static AppConfig load(const wxString& path);
};
