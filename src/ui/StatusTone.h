#pragma once

#include "config/Config.h"
#include "ui/ColorUtils.h"

#include <wx/activityindicator.h>
#include <wx/colour.h>

namespace ui {

// base16 slots, plain and bright, for the roles a status line can take on.
// Both of each pair are offered to pickContrasting because one of the two
// always disappears into the surface — the dim one on a dark scheme, the
// bright one on a light one.
inline constexpr size_t kAnsiRed          = 1;
inline constexpr size_t kAnsiYellow       = 3;
inline constexpr size_t kAnsiBrightRed    = 9;
inline constexpr size_t kAnsiBrightYellow = 11;

// What a one-line status is saying about itself, apart from its words.
//
// The distinction the tones carry is who the line is waiting on. Busy means
// this window is working and there is nothing to do but wait — which is exactly
// when the status line has to be findable, because it is the only thing on
// screen that knows anything is happening. Error means it stopped and the user
// has to decide something.
//
// Red is deliberately not the busy colour. This window already spends red on
// what is broken — dangling links in the listing, a directory that would not
// open — and a colour meaning both "failed" and "working" means neither.
enum class StatusTone { Normal, Busy, Error };

// The colour a toned status line is drawn in, taken from the active theme so it
// reads as part of the palette rather than painted over it.
//
// `background` is the surface the text actually sits on and `normal` what it
// wears untoned. Both are passed in rather than derived here because the status
// lines this serves belong to different widgets, and a foreground derived for
// one surface lands near-invisible on another.
inline wxColour StatusToneColour(StatusTone tone, const AppConfig& cfg,
                                 wxColour background, wxColour normal)
{
    const auto themed = [&](size_t plain, size_t bright) {
        return pickContrasting(background, toWx(cfg.ansiColors[plain]),
                                           toWx(cfg.ansiColors[bright]));
    };
    switch (tone) {
        case StatusTone::Busy:   return themed(kAnsiYellow, kAnsiBrightYellow);
        case StatusTone::Error:  return themed(kAnsiRed,    kAnsiBrightRed);
        case StatusTone::Normal: break;
    }
    return normal;
}

// The spinner that accompanies a toned status line.
//
// Running and visible exactly while the line is Busy, and gone otherwise —
// Error included, where a spinner would claim work is still going on. Kept
// beside the colour because the two are one decision: a spinner outliving the
// state that started it is the same stale-state bug in a louder form.
//
// Motion is here because colour alone is not enough to be noticed. A status
// line is small, static and at the edge of the window, and peripheral vision
// answers to movement long before it answers to hue.
//
// Returns true when the widget appeared or vanished, so a caller re-lays its
// sizer only on the occasions the layout actually changed.
inline bool ApplyToneToSpinner(wxActivityIndicator* spinner, StatusTone tone)
{
    if (!spinner) return false;

    const bool busy = tone == StatusTone::Busy;
    if (busy) spinner->Start();
    else      spinner->Stop();
    // Show() reports whether the visibility changed, which is exactly the
    // question the caller is asking.
    return spinner->Show(busy);
}

// A spinner sized to sit on a line of text without changing its height.
//
// wx would otherwise give it its own idea of a good size, which on GTK is
// taller than a status line and makes the row grow the moment work starts.
inline wxActivityIndicator* MakeStatusSpinner(wxWindow* parent, wxWindow* line)
{
    auto* spinner = new wxActivityIndicator(parent);
    const int side = line->GetCharHeight();
    spinner->SetMinSize(wxSize(side, side));
    spinner->Hide();
    return spinner;
}

} // namespace ui
