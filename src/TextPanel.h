#pragma once
#include <wx/scrolwin.h>
#include "Config.h"

class TextPanel : public wxScrolledWindow {
public:
    TextPanel(wxWindow* parent, const AppConfig& cfg);

private:
    void OnPaint(wxPaintEvent&);

    AppConfig m_cfg;
    wxFont    m_font;
    wxSize    m_charSize;
};
