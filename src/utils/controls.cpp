/* Copyright (C) 2007-2009 The SpringLobby Team. All rights reserved. */
#include "controls.h"
#include "../settings.h"
#include "../defines.h"
#include <wx/gdicmn.h>
#include <wx/window.h>
#include <wx/tooltip.h>

bool main_app_has_focus;

void UpdateMainAppHasFocus( bool focus )
{
	main_app_has_focus = focus;
	wxToolTip::Enable(sett().GetShowTooltips()&&main_app_has_focus);
}

const wxChar* TooltipEnable(const wxChar* input)
{
	#if !defined(HAVE_WX29) || defined(__WXOSX_COCOA__)
			if (!main_app_has_focus) return _T("");
			return sett().GetShowTooltips() ? input : _T("");
    #else
			wxString dummy = wxEmptyString;
			if (!main_app_has_focus) return dummy.wc_str();
			return sett().GetShowTooltips() ? input : dummy.wc_str();
    #endif
}
const wxChar* TooltipEnable(const wxString input)
{
	return TooltipEnable( input.c_str() );
}

int GetMaxStringWidth( const wxWindow& win, const wxArrayString& strings )
{
    int max = -1;
    for (size_t i = 0; i < strings.Count(); ++i ) {
        wxSize size;
        win.GetTextExtent( strings[i], &size.x, &size.y );
        max = std::max( max, size.x );
    }

    return max;
}
