/* This file is part of the Springlobby (GPL v2 or later), see COPYING */

#ifndef SPRINGLOBBY_HEADERGUARD_CHANNELLISTCTRL_H
#define SPRINGLOBBY_HEADERGUARD_CHANNELLISTCTRL_H

#include "gui/customvirtlistctrl.h"

struct ChannelInfo
{
	ChannelInfo()
	    : name(wxEmptyString)
	    , usercount(0)
	    , topic(wxEmptyString)
	{
	}
	ChannelInfo(const wxString& name_, int usercount_, const wxString& topic_ = wxEmptyString)
	    : name(name_)
	    , usercount(usercount_)
	    , topic(topic_)
	{
	}

	wxString name;
	int usercount;
	wxString topic;
};

class ChannelListctrl : public CustomVirtListCtrl<ChannelInfo, ChannelListctrl>
{
public:
	ChannelListctrl(wxWindow* parent, wxWindowID id, const wxString& name = _T("ChannelListCtrl"),
			long style = wxSUNKEN_BORDER | wxLC_REPORT | wxLC_ALIGN_LEFT, const wxPoint& pt = wxDefaultPosition,
			const wxSize& sz = wxDefaultSize);
	virtual ~ChannelListctrl();

	void AddChannel(const wxString& channel, unsigned int num_users, const wxString& topic);
	void ClearChannels();
	wxString GetInfo();
	void FilterChannel(const wxString& partial);

	//these are overloaded to use list in virtual style
	wxString GetItemText(long item, long column) const;
	int GetItemImage(long item) const;
	int GetItemColumnImage(long item, long column) const;
	wxListItemAttr* GetItemAttr(long /*unused*/) const
	{
		return 0;
	}

private:
	void Sort();
	void SetTipWindowText(const long item_hit, const wxPoint& position);

	void OnActivateItem(wxListEvent& event);

	void HighlightItem(long item);

	enum {
		CHANNELLIST = wxID_HIGHEST

	};

	int GetIndexFromData(const DataType& data) const;

	//! passed as callback to generic ItemComparator, returns -1,0,1 as per defined ordering
	int CompareOneCrit(DataType u1, DataType u2, int col, int dir) const;

	wxString m_last_filter_value;

	DECLARE_EVENT_TABLE()
};

#endif // SPRINGLOBBY_HEADERGUARD_CHANNELLISTCTRL_H
