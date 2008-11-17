/* Copyright (C) 2007 The SpringLobby Team. All rights reserved. */
//
// Class: User
//

#include "user.h"
#include "battle.h"
#include "server.h"
#include "utils.h"
#include "chatpanel.h"

#include <wx/string.h>
#include <wx/intl.h>

User::~User(){
  if(uidata.panel)uidata.panel->SetUser( 0 );
}

wxString UserStatus::GetDiffString ( const UserStatus& old )
{
    if ( old.away != away )
        return ( away ? _("away") : _("back") );
    if ( old.in_game != in_game )
        return ( in_game ? _("ingame") : _("back from game") );
    return
        wxEmptyString;
}

void User::Said( const wxString& message ) const
{
}


void User::Say( const wxString& message ) const
{
  m_serv.SayPrivate( m_nick, message );
}


void User::DoAction( const wxString& message ) const
{
  m_serv.DoActionPrivate( m_nick, message );
}


Battle* User::GetBattle() const
{
  return m_battle;
}


void User::SetBattle( Battle* battle )
{
  m_battle = battle;
}

void User::SetStatus( const UserStatus& status )
{
  m_status = status;
  // If user is host of a game, then his in_game status tells if the game is on!
  if ( m_battle != 0 ) {
    User& user = m_battle->GetFounder();
    if ( user.GetNick() == m_nick ) {
      m_battle->Update();
    }
  }

}
/*
void User::SetBattleStatus( const UserBattleStatus& status, bool setorder )
{
  int order = m_bstatus.order;
  m_bstatus = status;
  if ( !setorder ) m_bstatus.order = order;
}
*/

void CommonUser::UpdateBattleStatus( const UserBattleStatus& status, bool setorder )
{

  //int order = m_bstatus.order;
  //m_bstatus = status;
  /// total 12 members to update.

  if ( setorder ) m_bstatus.order=status.order; /// 1
  m_bstatus.team=status.team;
  m_bstatus.ally=status.ally;
  m_bstatus.colour=status.colour;
  m_bstatus.color_index=status.color_index;
  m_bstatus.handicap=status.handicap;
  m_bstatus.side=status.side;
  m_bstatus.sync=status.sync;
  m_bstatus.spectator=status.spectator;
  m_bstatus.ready=status.ready;

  /// update ip and port if those were set.
  if(!status.ip.empty())m_bstatus.ip=status.ip;
  if(status.udpport!=0)m_bstatus.udpport=status.udpport;/// 12

  //if ( !setorder ) m_bstatus.order = order;
}


void User::SendMyUserStatus()
{
  m_serv.SendMyUserStatus();
}


bool User::ExecuteSayCommand( const wxString& cmd ) const
{
  if ( cmd.BeforeFirst(' ').Lower() == _T("/me") ) {
    m_serv.DoActionPrivate( m_nick, cmd.AfterFirst(' ') );
    return true;
  }  else return false;
}

wxString User::GetRankName(UserStatus::RankContainer rank)
{
  //TODO: better interface to ranks?
      switch(rank) {
          case UserStatus::RANK_UNKNOWN: return _("Newbie");
          case UserStatus::RANK_1: return _("Newbie");
          case UserStatus::RANK_2: return _("Beginner");
          case UserStatus::RANK_3: return _("Average");
          case UserStatus::RANK_4: return _("Above average");
          case UserStatus::RANK_5: return _("Experienced");
          case UserStatus::RANK_6: return _("Highly experienced");
          case UserStatus::RANK_7: return _("Veteran");
      }
      return _("no rank");
}

float User::GetBalanceRank(){
  return 1.0+0.1*float(GetStatus().rank-UserStatus::RANK_1)/float(UserStatus::RANK_7-UserStatus::RANK_1);
}

wxString User::GetClan(){
  wxString tmp=m_nick.AfterFirst('[');
  if(tmp!=m_nick){
    wxString clan=tmp.BeforeFirst(']');
    if(clan!=tmp)return clan;
  }
  return wxString();
}

void CommonUser::SetStatus( const UserStatus& status )
{
  m_status = status;
}

User& User::operator= ( const User& other )
{
    if( this != &other ) {
        //m_serv = (other.GetServer());
        m_status = other.GetStatus();
        m_battle = other.GetBattle();
        m_nick = other.GetNick();
        m_cpu = other.GetCpu();
        m_country = other.GetCountry();
        m_bstatus = other.GetBattleStatus();
        uidata = other.uidata;


    }
    return *this;
}


