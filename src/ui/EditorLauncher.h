#pragma once

#include "transport/EnvUtils.h"

#include <string>

#include <wx/string.h>
#include <wx/utils.h>

namespace ui {

// Starts the user's editor on one file and returns immediately.
//
// Asynchronous by design: a terminal editor invoked through a wrapper, or a
// GUI editor told to --wait, can sit for as long as the user is editing, and
// the caller is the UI thread. Nothing downstream waits on the exit status —
// a remote edit is driven by the file being written, not by the editor
// finishing — so there is no result to report.
inline void LaunchEditor(const std::string& command, const std::string& path)
{
    wxExecute(wxString::FromUTF8(command + " " + term::transport::ShellQuote(path)),
              wxEXEC_ASYNC);
}

} // namespace ui
