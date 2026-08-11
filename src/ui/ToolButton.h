#pragma once

#include "ui/ColorUtils.h"
#include "ui/IconSet.h"

#include <wx/button.h>

namespace ui {

// How far a disabled glyph is faded toward the surface it sits on.
//
// Measured, not chosen: GTK already dims a disabled button's label and bitmap
// on its own, so anything applied here lands on top of that. This is the value
// at which the glyph and the label come out at the same weight — the two parts
// of one button should not look differently disabled.
inline constexpr double kDisabledGlyphFade = 0.25;

// Applies one theme to every visible aspect of a toolbar button at once.
//
// The three settings are one decision, not three: wxButton on GTK does not
// inherit its foreground from the parent panel, so a button given only a
// background goes invisible on a dark theme, and the glyph is drawn in the
// label colour, so setting one without the other lets them drift apart at the
// next theme change. Keeping them in a single call makes that impossible.
inline void StyleToolButton(wxButton* button, Icon icon,
                            const wxColour& background, const wxColour& foreground,
                            wxDirection glyphSide = wxLEFT)
{
    if (!button) return;

    button->SetBackgroundColour(background);
    button->SetForegroundColour(foreground);

    button->SetBitmap(IconBundle(icon, foreground), glyphSide);
    // wx dims a disabled button's glyph only if it was given one to dim: with
    // no disabled bitmap set, the normal one keeps being drawn at full strength
    // and a greyed-out Back button still looks live. Navigation buttons spend
    // much of their life disabled, so this is the common case, not the corner.
    button->SetBitmapDisabled(
        IconBundle(icon, blendWx(foreground, background, kDisabledGlyphFade)));
}

// Creates a button that carries nothing but its glyph.
//
// The construction lives here because the button has to be born label-less and
// stay that way: GTK decides at construction whether a button holds an image or
// a label, and a later SetLabel() on an image button swaps the image widget out
// for a GtkLabel that no subsequent SetBitmap() can fill — the glyph silently
// stops appearing. wxBU_NOTEXT states that intent to wx rather than leaving it
// implied by an empty string that some later caller could fill in.
//
// The name still has to live somewhere: it is what an assistive technology
// announces and what a user hovers for when the drawing alone is not enough.
// SetName() carries it to the accessibility layer, the tooltip to everyone else.
inline wxButton* MakeIconButton(wxWindow* parent, const wxString& name)
{
    auto* button = new wxButton(parent, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                wxDefaultSize, wxBU_EXACTFIT | wxBU_NOTEXT);
    button->SetName(name);
    button->SetToolTip(name);
    return button;
}

} // namespace ui
