/* This file is part of the Springlobby (GPL v2 or later), see COPYING */

#define BOOST_TEST_MODULE lobbyid
#include <boost/test/unit_test.hpp>

#include <stdio.h>
#include "utils/lslconversion.h"
#include <lslutils/misc.h>
#include <wx/colour.h>


void test_wxColourTolsl(const unsigned char red, const unsigned char green, const unsigned char blue, const unsigned char alpha)
{
	const wxColor wxcol(red, green, blue, alpha);
	LSL::lslColor lslcol = wxColourTolsl(wxcol);
	BOOST_CHECK(lslcol.Red() == red);
	BOOST_CHECK(lslcol.Green() == green);
	BOOST_CHECK(lslcol.Blue() == blue);
	BOOST_CHECK(lslcol.Alpha() == alpha);
}

void test_lslTowxColour(const unsigned char red, const unsigned char green, const unsigned char blue, const unsigned char alpha)
{
	const LSL::lslColor col(red, green, blue, alpha);
	wxColor wxcol = lslTowxColour(col);
	BOOST_CHECK(wxcol.Red() == red);
	BOOST_CHECK(wxcol.Green() == green);
	BOOST_CHECK(wxcol.Blue() == blue);
	BOOST_CHECK(wxcol.Alpha() == alpha);
}

void test_lslToLobbyColour(const unsigned char red, const unsigned char green, const unsigned char blue, const unsigned char alpha)
{
	const LSL::lslColor col(red, green, blue, alpha);

	BOOST_CHECK(col.Red() == red);
	BOOST_CHECK(col.Green() == green);
	BOOST_CHECK(col.Blue() == blue);
	BOOST_CHECK(col.Alpha() == alpha);
	const LSL::lslColor col2(col.GetLobbyColor());

	BOOST_CHECK(col2.Red() == red);
	BOOST_CHECK(col2.Green() == green);
	BOOST_CHECK(col2.Blue() == blue);
	BOOST_CHECK(col2.Alpha() == 255);
}

BOOST_AUTO_TEST_CASE(lslconversion)
{
	test_wxColourTolsl(0, 0, 0, 0);
	test_wxColourTolsl(1, 2, 3, 4);
	test_wxColourTolsl(255, 255, 255, 255);

	test_lslTowxColour(0, 0, 0, 0);
	test_lslTowxColour(1, 2, 3, 4);
	test_lslTowxColour(255, 255, 255, 255);

	test_lslToLobbyColour(0, 0, 0, 0);
	test_lslToLobbyColour(1, 2, 3, 4);
	test_lslToLobbyColour(255, 255, 255, 255);

	LSL::StringVector test;
	test.push_back("line1");
	test.push_back("line2");
	test.push_back("line3");

	wxArrayString tmp = lslTowxArrayString(test);
	LSL::StringVector tmp2 = wxArrayStringToLSL(tmp);

	BOOST_CHECK(test.size() == tmp2.size());
	BOOST_CHECK(test[0] == tmp2[0]);
	BOOST_CHECK(test[1] == tmp2[1]);
	BOOST_CHECK(test[2] == tmp2[2]);
}
