#include "Config.h"
#include <wx/fileconf.h>
#include <wx/wfstream.h>

AppConfig AppConfig::load(const wxString& path) {
    AppConfig cfg;
    wxFileInputStream stream(path);
    if (!stream.IsOk()) return cfg;

    wxFileConfig ini(stream);

    ini.Read("/Panel/Columns",  &cfg.columns,  cfg.columns);
    ini.Read("/Panel/Rows",     &cfg.rows,     cfg.rows);
    ini.Read("/Panel/FontSize", &cfg.fontSize, cfg.fontSize);

    auto readColour = [&](wxColour& c,
                          const wxString& rk, const wxString& gk, const wxString& bk) {
        int r = c.Red(), g = c.Green(), b = c.Blue();
        ini.Read(rk, &r, r);
        ini.Read(gk, &g, g);
        ini.Read(bk, &b, b);
        c.Set(r, g, b);
    };

    readColour(cfg.textColour,
               "/Colors/TextR", "/Colors/TextG", "/Colors/TextB");
    readColour(cfg.bgColour,
               "/Colors/BgR",   "/Colors/BgG",   "/Colors/BgB");

    return cfg;
}
