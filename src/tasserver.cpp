/* This file is part of the Springlobby (GPL v2 or later), see COPYING */

/**
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

DO NOT CHANGE THIS FILE!

this file is deprecated and will be replaced with

lsl/networking/tasserver.cpp

!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
**/


#include <wx/string.h>
#include <wx/intl.h>
#include <wx/protocol/http.h>
#include <wx/socket.h>
#include <wx/log.h>
#include <wx/timer.h>

#include <stdexcept>
#include <algorithm>
#include <map>

#include "utils/base64.h"
#include "utils/md5.h"
#include "tasserver.h"
#include "utils/tasutil.h"
#include "utils/conversion.h"
#include "serverevents.h"
#include "socket.h"
#include "log.h"
#include "utils/slconfig.h"
#include "utils/version.h"
#include "settings.h"


SLCONFIG("/Server/ExitMessage", "Using http://springlobby.info/", "Message which is send when leaving server");
SLCONFIG("/General/CpuID", 0L, "CPUId to be used when logging int to lobbyserver");

// times in milliseconds
#define PING_TIME 30000
#define PING_TIMEOUT 90000
#define PING_DELAY 5000 //first ping is sent after PING_DELAY
#define UDP_KEEP_ALIVE 15000
#define UDP_REPLY_TIMEOUT 10000


#define CHECK_BATTLE_ID()                                                         \
	try {                                                                     \
		ASSERT_LOGIC(m_battle_id != -1, "Invalid battle id");             \
		ASSERT_LOGIC(BattleExists(m_battle_id), "battle doesn't exists"); \
	} catch (...) {                                                           \
		return;                                                           \
	}

#define CHECK_CURRENT_BATTLE_ID(battle_id)                                   \
	slLogDebugFunc("");                                                  \
	CHECK_BATTLE_ID();                                                   \
	try {                                                                \
		CHECK_BATTLE_ID();                                           \
		ASSERT_LOGIC(battleid == m_battle_id, "Not current battle"); \
	} catch (...) {                                                      \
		return;                                                      \
	}


/*

myteamcolor:  Should be 32-bit signed integer in decimal form (e.g. 255 and not FF) where each color channel should occupy 1 byte (e.g. in hexdecimal: $00BBGGRR, B = blue, G = green, R = red). Example: 255 stands for $000000FF.

*/
int ConvUserStatus(const UserStatus& us);
UserStatus ConvTasclientstatus(const int);

UserBattleStatus ConvTasbattlestatus(const int);
int ConvTasbattlestatus(const UserBattleStatus);

IBattle::StartType IntToStartType(int start);
NatType IntToNatType(int nat);
IBattle::GameType IntToGameType(int gt);


TASServer::TASServer()
    : m_sock(NULL)
    , m_ser_ver(0)
    , m_connected(false)
    , m_online(false)
    , m_debug_dont_catch(false)
    , m_id_transmission(true)
    , m_redirecting(false)
    , m_buffer("")
    , m_last_udp_ping(0)
    , m_last_ping(PING_DELAY)
    , //no instant ping, delay first ping for PING_DELAY seconds
    m_last_net_packet(0)
    , m_lastnotify(wxGetLocalTimeMillis())
    , m_last_id(0)
    , m_udp_private_port(0)
    , m_nat_helper_port(0)
    , m_battle_id(-1)
    , m_server_lanmode(false)
    , m_account_id_count(0)
    , m_do_finalize_join_battle(false)
    , m_finalize_join_battle_id(-1)
{
	m_se = new ServerEvents(*this);
	m_relay_host_manager_list.clear();

	Start(100); // call Update every 100ms
}

TASServer::~TASServer()
{
	Stop();
	Disconnect();
	delete m_se;
	m_se = NULL;
}

bool TASServer::ExecuteSayCommand(const std::string& cmd)
{
	LSL::StringVector arrayparams = LSL::Util::StringTokenize(cmd, " ");
	if (arrayparams.empty())
		return false;
	const std::string subcmd = arrayparams[0];
	const std::string params = LSL::Util::AfterFirst(cmd, " ");
	if (subcmd == "/ingame") {
		if (arrayparams.size() != 2)
			return false;
		SendCmd("GETINGAMETIME", arrayparams[1]);
		return true;
	} else if (subcmd == "/kick") {
		if (arrayparams.size() < 2)
			return false;
		SendCmd("KICKUSER", params);
		return true;
	} else if (subcmd == "/ban") {
		if (arrayparams.size() < 2)
			return false;
		SendCmd("BAN", params);
		return true;
	} else if (subcmd == "/unban") {
		if (arrayparams.size() != 2)
			return false;
		SendCmd("UNBAN", arrayparams[1]);
		return true;
	} else if (subcmd == "/banlist") {
		SendCmd("BANLIST");
		return true;
	} else if (subcmd == "/topic") {
		std::string cmd = params;
		LSL::Util::Replace(cmd, "\n", "\\n");
		SendCmd("CHANNELTOPIC", cmd);
		return true;
	} else if (subcmd == "/chanmsg") {
		if (arrayparams.size() < 2)
			return false;
		SendCmd("CHANNELMESSAGE", params);
		return true;
	} else if (subcmd == "/ring") {
		if (arrayparams.size() != 2)
			return false;
		SendCmd("RING", arrayparams[1]);
		return true;
	} else if (subcmd == "/ip") {
		if (arrayparams.size() != 2)
			return false;
		SendCmd("GETIP", arrayparams[1]);
		return true;
	} else if (subcmd == "/mute") {
		if (arrayparams.size() < 4)
			return false;
		if (arrayparams.size() > 5)
			return false;
		SendCmd("MUTE", params);
		return true;
	} else if (subcmd == "/unmute") {
		if (arrayparams.size() != 3)
			return false;
		SendCmd("UNMUTE", arrayparams[1] + std::string(" ") + arrayparams[2]);
		return true;
	} else if (subcmd == "/mutelist") {
		if (arrayparams.size() != 2)
			return false;
		SendCmd("MUTELIST", arrayparams[1]);
		return true;
	} else if (subcmd == "/lastlogin") {
		if (arrayparams.size() != 2)
			return false;
		SendCmd("GETLASTLOGINTIME", arrayparams[1]);
		return true;
	} else if (subcmd == "/findip") {
		if (arrayparams.size() != 2)
			return false;
		SendCmd("FINDIP", arrayparams[1]);
		return true;
	} else if (subcmd == "/lastip") {
		if (arrayparams.size() != 2)
			return false;
		SendCmd("GETLASTIP", arrayparams[1]);
		return true;
	} else if (subcmd == "/rename") {
		if (arrayparams.size() != 2)
			return false;
		SendCmd("RENAMEACsize", arrayparams[1]);
		sett().SetServerAccountNick(sett().GetDefaultServer(), TowxString(arrayparams[1])); // this code assumes that default server hasn't changed since login ( like it should atm )
		return true;
	} else if (subcmd == "/testmd5") {
		ExecuteCommand("SERVERMSG", GetPasswordHash(params));
		return true;
	} else if (subcmd == "/hook") {
		SendCmd("HOOK", params);
		return true;
	} else if (subcmd == "/quit") {
		Disconnect();
		return true;
	} else if (subcmd == "/changepassword2") {
		if (arrayparams.size() < 2) {
			m_se->OnServerMessage("Missing parameter, usage: /changepassword2 newpassword");
			return true;
		}
		const std::string oldpassword = STD_STRING(sett().GetServerAccountPass(TowxString(GetServerName())));
		const std::string newpassword = GetPasswordHash(params);
		if (oldpassword.empty() || !sett().GetServerAccountSavePass(TowxString(GetServerName()))) {
			m_se->OnServerMessage("There is no saved password for this acsize, please use /changepassword oldpassword newpassword");
			return true;
		}
		SendCmd("CHANGEPASSWORD", oldpassword + std::string(" ") + newpassword);
		return true;
	} else if (subcmd == "/changepassword") {
		if (arrayparams.size() != 3) {
			m_se->OnServerMessage("Invalid parameter size, usage: /changepassword oldpassword newpassword");
			return true;
		}
		const std::string oldpassword = GetPasswordHash(arrayparams[1]);
		const std::string newpassword = GetPasswordHash(arrayparams[2]);
		SendCmd("CHANGEPASSWORD", oldpassword + std::string(" ") + newpassword);
		return true;
	} else if (subcmd == "/ping") {
		Ping();
		return true;
	}
	return false;
}


void TASServer::Connect(const std::string& servername, const std::string& addr, const int port)
{
	m_server_name = servername;
	m_addr = addr;
	m_buffer.clear();
	m_subscriptions.clear();
	if (m_sock != NULL) {
		Disconnect();
	}
	m_sock = new Socket(*this);
	m_sock->Connect(TowxString(addr), port);
	m_sock->SetSendRateLimit(800); // 1250 is the server limit but 800 just to make sure :)
	m_connected = false;
	m_online = false;
	m_redirecting = false;
	m_agreement.clear();
	m_crc.ResetCRC();
	m_last_net_packet = 0;
	m_lastnotify = wxGetLocalTimeMillis();
	const std::string handle = m_sock->GetHandle();
	if (!handle.empty()) {
		m_crc.UpdateData(handle);
	}
}

void TASServer::Disconnect()
{
	if (m_sock == NULL) {
		return;
	}

	m_connected = false;
	m_battle_id = -1;
	SendCmd("EXIT " + STD_STRING(cfg().ReadString(_T("/Server/ExitMessage")))); // EXIT command for new protocol compatibility
	m_sock->Disconnect();
	delete m_sock;
	m_sock = NULL;
}

bool TASServer::IsConnected()
{
	return (m_sock != NULL) && (m_sock->State() == SS_Open) && (m_connected);
}


void TASServer::Register(const std::string& servername, const std::string& host, const int port, const std::string& nick, const std::string& password)
{
	SetUsername(nick);
	SetPassword(password);
	Connect(servername, host, port);
	SendCmd("REGISTER", nick + std::string(" ") + GetPasswordHash(password));
}


bool TASServer::IsPasswordHash(const std::string& pass) const
{
	return pass.length() == 24 && pass[22] == '=' && pass[23] == '=';
}


std::string TASServer::GetPasswordHash(const std::string& pass) const
{
	if (IsPasswordHash(pass))
		return pass;

	md5_state_t state;
	md5_byte_t digest[16];
	char hex_output[16 * 2 + 1];
	int di;

	std::string str = pass;
	char* cstr = new char[str.size() + 1];
	strcpy(cstr, str.c_str());

	md5_init(&state);
	md5_append(&state, (const md5_byte_t*)cstr, strlen(cstr));
	md5_finish(&state, digest);
	for (di = 0; di < 16; ++di)
		sprintf(hex_output + di * 2, "%02x", digest[di]);
	delete[] cstr;
	return base64_encode(digest, 16);
}


User& TASServer::GetMe()
{
	return GetUser(GetUserName());
}
const User& TASServer::GetMe() const
{
	return GetUser(GetUserName());
}

void TASServer::Login()
{
	slLogDebugFunc("");
	const std::string pass = GetPasswordHash(GetPassword());
	std::string localaddr = STD_STRING(m_sock->GetLocalAddress());
	if (localaddr.empty())
		localaddr = "*";
	m_id_transmission = false;
	const int cpuid = cfg().ReadLong(_T("/General/CpuID"));
	SendCmd("LOGIN", stdprintf("%s %s %d %s %s\t%u\ta m sp cl p",
				   GetUserName().c_str(), pass.c_str(), cpuid, localaddr.c_str(), getSpringlobbyAgent().c_str(), m_crc.GetCRC()));
	m_id_transmission = true;
}

void TASServer::Logout()
{
	slLogDebugFunc("");
	Disconnect();
}

bool TASServer::IsOnline() const
{
	if (!m_connected)
		return false;
	return m_online;
}


void TASServer::RequestChannels()
{
	SendCmd("CHANNELS");
}


void TASServer::AcceptAgreement()
{
	SendCmd("CONFIRMAGREEMENT");
}


void TASServer::Notify()
{
	if (m_sock == NULL)
		return;

	const wxLongLong now = wxGetLocalTimeMillis();
	const long diff = std::abs((now - m_lastnotify).ToLong());
	const int interval = std::max<long>(GetInterval(), diff);
	m_lastnotify = now;

	m_sock->Update(interval);
	m_last_ping += interval;
	m_last_net_packet += interval;
	m_last_udp_ping += interval;

	if (!m_connected) { // We are not formally connected yet, but might be.
		if (IsConnected()) {
			m_connected = true;
		}
		return;
	}
	if (!IsConnected())
		return;


	if (m_last_ping > PING_TIME) { //Send a PING every 30 seconds
		if (interval > PING_TIME) {
			m_last_net_packet = 0; //assume local clock is broken and we received a packed within time
			m_se->OnServerMessage(stdprintf("Springlobby hung or stale clock. Got no timer for %d msec", interval));
		}
		m_last_ping = 0;
		Ping();
		return;
	}

	if (m_last_net_packet > PING_TIMEOUT) {
		m_se->OnServerMessage(stdprintf("Timeout assumed, disconnecting. Received no data from server for %d seconds. Last ping send %d seconds ago.", (m_last_net_packet / 1000), (m_last_ping / 1000)));
		m_last_net_packet = 0;
		Disconnect();
		return;
	}

	// joining battle with nat traversal:
	// if we havent finalized joining yet, and udp_reply_timeout seconds has passed since
	// we did UdpPing(our name) , join battle anyway, but with warning message that nat failed.
	// (if we'd receive reply from server, we'd finalize already)
	//
	if (m_do_finalize_join_battle && (m_last_udp_ping > UDP_REPLY_TIMEOUT)) {
		m_se->OnServerMessage("Failed to punch through NAT, playing this battle might not work for you or for other players.");
		//wxMessageBox()
		FinalizeJoinBattle();
		//wxMessageBox(_("Failed to punch through NAT"), _("Error"), wxICON_INFORMATION, NULL/* m_ui.mw()*/ );
	}

	if ((m_last_udp_ping > UDP_KEEP_ALIVE)) {
		// Is it time for a nat traversal PING?
		m_last_udp_ping = 0;
		// Nat travelsal "ping"
		if (m_battle_id != -1) {
			IBattle* battle = GetCurrentBattle();
			if ((battle) &&
			    (battle->GetNatType() == NAT_Hole_punching || (battle->GetNatType() == NAT_Fixed_source_ports)) && !battle->GetInGame()) {
				UdpPingTheServer(GetUserName());
				if (battle->IsFounderMe()) {
					UdpPingAllClients();
				}
			}
		}
	}
}


void TASServer::ExecuteCommand(const std::string& in)
{
	wxLogMessage(_T("%s"), TowxString(in).c_str());
	wxString cmd;
	wxString params = TowxString(in);
	long replyid = 0;

	if (in.empty())
		return;
	try {
		ASSERT_LOGIC(params.AfterFirst('\n').IsEmpty(), "losing data");
	} catch (...) {
		return;
	}
	if (params[0] == '#') {
		wxString id = params.BeforeFirst(' ').AfterFirst('#');
		params = params.AfterFirst(' ');
		id.ToLong(&replyid);
	}
	cmd = params.BeforeFirst(' ').MakeUpper();
	params = params.AfterFirst(' ');

	if (m_debug_dont_catch) {
		ExecuteCommand(STD_STRING(cmd), STD_STRING(params), replyid);
	} else {
		try {
			ExecuteCommand(STD_STRING(cmd), STD_STRING(params), replyid);
		} catch (...) { // catch everything so the app doesn't crash, may makes odd beahviours but it's better than crashing randomly for normal users
		}
	}
}


static LSL::StringMap parseKeyValue(const std::string& str)
{
	const LSL::StringVector params = LSL::Util::StringTokenize(str, "\t");
	LSL::StringMap result;
	for (auto const param : params) {
		const LSL::StringVector keyvalue = LSL::Util::StringTokenize(param, "="); //FIXME: key=va=lue isn't supported
		if (keyvalue.size() != 2) {
			wxLogWarning(_T("Invalid keyvalue: %s"), TowxString(param).c_str());
			continue;
		}
		result[keyvalue[0]] = keyvalue[1];
	}
	return result;
}


void TASServer::ExecuteCommand(const std::string& cmd, const std::string& inparams, int replyid)
{
	wxString params = TowxString(inparams);
	bool haspass, lanmode = false;
	std::string nick, contry, host, map, title, channel, error, msg, owner, topic, engineName, engineVersion;
	//NatType ntype;
	UserStatus cstatus;
	int tasstatus;
	int tasbstatus;
	UserBattleStatus bstatus;

	if (cmd == "TASSERVER") {
		m_ser_ver = GetIntParam(params);
		const std::string supported_spring_version = GetWordParam(params);
		m_nat_helper_port = (unsigned long)GetIntParam(params);
		lanmode = GetBoolParam(params);
		m_server_lanmode = lanmode;
		m_se->OnConnected(m_server_name, "", (m_ser_ver > 0), supported_spring_version, lanmode);
	} else if (cmd == "ACCEPTED") {
		if (m_online)
			return; // in case is the server sends WTF
		m_online = true;
		SetUsername(STD_STRING(params));
		m_se->OnLogin();
	} else if (cmd == "MOTD") {
		m_se->OnMotd(STD_STRING(params));
	} else if (cmd == "ADDUSER") {
		int id;
		nick = GetWordParam(params);
		contry = GetWordParam(params);
		const int cpu = GetIntParam(params);
		if (params.IsEmpty()) {
			// if server didn't send any acsize id to us, fill with an always increasing number
			id = m_account_id_count;
			m_account_id_count++;
		} else {
			id = GetIntParam(params);
		}
		m_se->OnNewUser(nick, contry, cpu, id);
		if (nick == m_relay_host_bot) {
			RelayCmd("OPENBATTLE", m_delayed_open_command); // relay bot is deployed, send host command
			m_delayed_open_command = "";
		}
	} else if (cmd == "CLIENTSTATUS") {
		nick = GetWordParam(params);
		tasstatus = GetIntParam(params);
		cstatus = ConvTasclientstatus(tasstatus);
		m_se->OnUserStatus(nick, cstatus);
	} else if (cmd == "BATTLEOPENED") {
		const int id = GetIntParam(params);
		const int type = GetIntParam(params);
		const int nat = GetIntParam(params);
		nick = GetWordParam(params);
		host = GetWordParam(params);
		const int port = GetIntParam(params);
		const int maxplayers = GetIntParam(params);
		haspass = GetBoolParam(params);
		const int rank = GetIntParam(params);
		const std::string hash = LSL::Util::MakeHashUnsigned(GetWordParam(params));
		engineName = GetSentenceParam(params);
		engineVersion = GetSentenceParam(params);
		map = GetSentenceParam(params);
		title = GetSentenceParam(params);
		const std::string mod = GetSentenceParam(params);
		m_se->OnBattleOpened(id, (BattleType)type, IntToNatType(nat), nick, host, port, maxplayers,
				     haspass, rank, hash, engineName, engineVersion, map, title, mod);
		if (nick == m_relay_host_bot) {
			GetBattle(id).SetProxy(m_relay_host_bot);
			JoinBattle(id, STD_STRING(sett().GetLastHostPassword())); // autojoin relayed host battles
		}
	} else if (cmd == "JOINEDBATTLE") {
		const int id = GetIntParam(params);
		nick = GetWordParam(params);
		const std::string userScriptPassword = GetWordParam(params);
		m_se->OnUserJoinedBattle(id, nick, userScriptPassword);
	} else if (cmd == "UPDATEBATTLEINFO") {
		const int id = GetIntParam(params);
		const int specs = GetIntParam(params);
		haspass = GetBoolParam(params);
		const std::string hash = LSL::Util::MakeHashUnsigned(GetWordParam(params));
		map = GetSentenceParam(params);
		m_se->OnBattleInfoUpdated(id, specs, haspass, hash, map);
	} else if (cmd == "LOGININFOEND") {
		if (UserExists("RelayHostManagerList"))
			SayPrivate("RelayHostManagerList", "!lm");
		SendCmd("LISTSUBSCRIPTIONS", "");
		m_se->OnLoginInfoComplete();
	} else if (cmd == "REMOVEUSER") {
		nick = GetWordParam(params);
		if (nick == GetUserName())
			return; // to prevent peet doing nasty stuff to you, watch your back!
		m_se->OnUserQuit(nick);
	} else if (cmd == "BATTLECLOSED") {
		const int id = GetIntParam(params);
		if (m_battle_id == id) {
			m_relay_host_bot.clear();
			m_battle_id = -1;
		}
		m_se->OnBattleClosed(id);
	} else if (cmd == "LEFTBATTLE") {
		const int id = GetIntParam(params);
		nick = GetWordParam(params);
		if ((id == m_battle_id) && (nick == GetMe().GetNick())) {
			m_battle_id = -1;
		}
		m_se->OnUserLeftBattle(id, nick);
	} else if (cmd == "PONG") {
		HandlePong(replyid);
	} else if (cmd == "JOIN") {
		channel = GetWordParam(params);
		m_se->OnJoinChannelResult(true, channel, "");
	} else if (cmd == "JOIN") {
		channel = GetWordParam(params);
		error = GetSentenceParam(params);
		m_se->OnJoinChannelResult(false, channel, error);
	} else if (cmd == "SAID") {
		channel = GetWordParam(params);
		nick = GetWordParam(params);
		m_se->OnChannelSaid(channel, nick, STD_STRING(params));
	} else if (cmd == "JOINED") {
		channel = GetWordParam(params);
		nick = GetWordParam(params);
		m_se->OnUserJoinChannel(channel, nick);
	} else if (cmd == "LEFT") {
		channel = GetWordParam(params);
		nick = GetWordParam(params);
		msg = GetSentenceParam(params);
		m_se->OnChannelPart(channel, nick, msg);
	} else if (cmd == "CHANNELTOPIC") {
		channel = GetWordParam(params);
		nick = GetWordParam(params);
		int pos = GetIntParam(params);
		params.Replace(_T("\\n"), _T("\n"));
		m_se->OnChannelTopic(channel, nick, STD_STRING(params), pos / 1000);
	} else if (cmd == "SAIDEX") {
		channel = GetWordParam(params);
		nick = GetWordParam(params);
		m_se->OnChannelAction(channel, nick, STD_STRING(params));
	} else if (cmd == "CLIENTS") {
		channel = GetWordParam(params);
		while (!(nick = GetWordParam(params)).empty()) {
			m_se->OnChannelJoin(channel, nick);
		}
	} else if (cmd == "SAYPRIVATE") {
		nick = GetWordParam(params);
		if (((nick == m_relay_host_bot) || (nick == m_relay_host_manager)) && params.StartsWith(_T("!")))
			return; // drop the message
		if ((nick == "RelayHostManagerList") && (params == _T("!lm")))
			return; // drop the message
		if (nick == "SL_bot") {
			if (params.StartsWith(_T("stats.report")))
				return;
		}
		m_se->OnPrivateMessage(nick, STD_STRING(params), true);
	} else if (cmd == "SAYPRIVATEEX") {
		nick = GetWordParam(params);
		m_se->OnPrivateMessageEx(nick, STD_STRING(params), true);
	} else if (cmd == "SAIDPRIVATE") {
		nick = GetWordParam(params);
		if (nick == m_relay_host_bot) {
			if (params.StartsWith(_T("JOINEDBATTLE"))) {
				GetWordParam(params); // skip first word, it's the message itself
				/*id =*/
				GetIntParam(params);
				const std::string usernick = GetWordParam(params);
				const std::string userScriptPassword = GetWordParam(params);
				try {
					User& usr = GetUser(usernick);
					usr.BattleStatus().scriptPassword = userScriptPassword;
					IBattle* battle = GetCurrentBattle();
					if (battle) {
						if (battle->CheckBan(usr))
							return;
					}
					SetRelayIngamePassword(usr);
				} catch (...) {
				}
				return;
			}
		}
		if (nick == m_relay_host_manager) {
			if (params.StartsWith(_T("\001"))) { // error code
				m_se->OnServerMessageBox(STD_STRING(params.AfterFirst(_T(' '))));
			} else {
				m_relay_host_bot = STD_STRING(params);
			}
			m_relay_host_manager.clear();
			return;
		}
		if (nick == "RelayHostManagerList") {
			if (params.StartsWith(_T("list "))) {
				const std::string list = LSL::Util::AfterFirst(STD_STRING(params), " ");
				m_relay_host_manager_list = LSL::Util::StringTokenize(list, "\t");
				return;
			}
		}
		m_se->OnPrivateMessage(nick, STD_STRING(params), false);
	} else if (cmd == "SAIDPRIVATEEX") {
		nick = GetWordParam(params);
		m_se->OnPrivateMessageEx(nick, STD_STRING(params), false);
	} else if (cmd == "JOINBATTLE") {
		const int id = GetIntParam(params);
		const std::string hash = LSL::Util::MakeHashUnsigned(GetWordParam(params));
		m_battle_id = id;
		m_se->OnJoinedBattle(id, hash);
		m_se->OnBattleInfoUpdated(m_battle_id);
		try {
			if (GetBattle(id).IsProxy())
				RelayCmd("SUPPORTSCRIPTPASSWORD"); // send flag to relayhost marking we support script passwords
		} catch (...) {
		}
	} else if (cmd == "CLIENTBATTLESTATUS") {
		nick = GetWordParam(params);
		tasbstatus = GetIntParam(params);
		bstatus = ConvTasbattlestatus(tasbstatus);
		bstatus.colour = LSL::lslColor(GetIntParam(params));
		m_se->OnClientBattleStatus(m_battle_id, nick, bstatus);
	} else if (cmd == "ADDSTARTRECT") {
		//ADDSTARTRECT allyno left top right bottom
		const int ally = GetIntParam(params);
		const int left = GetIntParam(params);
		const int top = GetIntParam(params);
		const int right = GetIntParam(params);
		const int bottom = GetIntParam(params);
		;
		m_se->OnBattleStartRectAdd(m_battle_id, ally, left, top, right, bottom);
	} else if (cmd == "REMOVESTARTRECT") {
		//REMOVESTARTRECT allyno
		const int ally = GetIntParam(params);
		m_se->OnBattleStartRectRemove(m_battle_id, ally);
	} else if (cmd == "ENABLEALLUNITS") {
		//"ENABLEALLUNITS" params: "".
		m_se->OnBattleEnableAllUnits(m_battle_id);
	} else if (cmd == "ENABLEUNITS") {
		//ENABLEUNITS unitname1 unitname2
		while ((nick = GetWordParam(params)) != "") {
			m_se->OnBattleEnableUnit(m_battle_id, nick);
		}
	} else if (cmd == "DISABLEUNITS") {
		//"DISABLEUNITS" params: "arm_advanced_radar_tower arm_advanced_sonar_station arm_advanced_torpedo_launcher arm_dragons_teeth arm_energy_storage arm_eraser arm_fark arm_fart_mine arm_fibber arm_geothermal_powerplant arm_guardian"
		while ((nick = GetWordParam(params)) != "") {
			m_se->OnBattleDisableUnit(m_battle_id, nick);
		}
	} else if (cmd == "CHANNEL") {
		channel = GetWordParam(params);
		const int units = GetIntParam(params);
		topic = GetSentenceParam(params);
		m_se->OnChannelList(channel, units, topic);
	} else if (cmd == "ENDOFCHANNELS") {
		//Cmd: ENDOFCHANNELS params:
	} else if (cmd == "REQUESTBATTLESTATUS") {
		m_se->OnRequestBattleStatus(m_battle_id);
	} else if (cmd == "SAIDBATTLE") {
		nick = GetWordParam(params);
		m_se->OnSaidBattle(m_battle_id, nick, STD_STRING(params));
	} else if (cmd == "SAIDBATTLEEX") {
		nick = GetWordParam(params);
		m_se->OnBattleAction(m_battle_id, nick, STD_STRING(params));
	} else if (cmd == "AGREEMENT") {
		msg = GetSentenceParam(params);
		m_agreement += msg + "\n";
	} else if (cmd == "AGREEMENTEND") {
		m_se->OnAcceptAgreement(m_agreement);
		m_agreement.clear();
	} else if (cmd == "OPENBATTLE") {
		m_battle_id = GetIntParam(params);
		m_se->OnHostedBattle(m_battle_id);
	} else if (cmd == "ADDBOT") {
		// ADDBOT BATTLE_ID name owner battlestatus teamcolor {AIDLL}
		const int id = GetIntParam(params);
		nick = GetWordParam(params);
		owner = GetWordParam(params);
		tasbstatus = GetIntParam(params);
		bstatus = ConvTasbattlestatus(tasbstatus);
		bstatus.colour = LSL::lslColor(GetIntParam(params));
		wxString ai = TowxString(GetSentenceParam(params));
		if (ai.empty()) {
			wxLogWarning(wxString::Format(_T("Recieved illegal ADDBOT (empty dll field) from %s for battle %d"), nick.c_str(), id));
			ai = _T("INVALID|INVALID");
		}
		if (ai.Find(_T('|')) != -1) {
			bstatus.aiversion = STD_STRING(ai.AfterLast(_T('|')));
			ai = ai.BeforeLast(_T('|'));
		}
		bstatus.aishortname = STD_STRING(ai);
		bstatus.owner = owner;
		m_se->OnBattleAddBot(id, nick, bstatus);
	} else if (cmd == "UPDATEBOT") {
		const int id = GetIntParam(params);
		nick = GetWordParam(params);
		tasbstatus = GetIntParam(params);
		bstatus = ConvTasbattlestatus(tasbstatus);
		bstatus.colour = LSL::lslColor(GetIntParam(params));
		m_se->OnBattleUpdateBot(id, nick, bstatus);
		//UPDATEBOT BATTLE_ID name battlestatus teamcolor
	} else if (cmd == "REMOVEBOT") {
		const int id = GetIntParam(params);
		nick = GetWordParam(params);
		m_se->OnBattleRemoveBot(id, nick);
		//REMOVEBOT BATTLE_ID name
	} else if (cmd == "RING") {
		nick = GetWordParam(params);
		m_se->OnRing(nick);
		//RING username
	} else if (cmd == "SERVERMSG") {
		m_se->OnServerMessage(STD_STRING(params));
		//SERVERMSG {message}
	} else if (cmd == "JOINBATTLEFAILED") {
		msg = GetSentenceParam(params);
		m_se->OnServerMessage("Failed to join battle. " + msg);
		//JOINBATTLEFAILED {reason}
	} else if (cmd == "OPENBATTLEFAILED") {
		msg = GetSentenceParam(params);
		m_se->OnServerMessage("Failed to host new battle on server. " + msg);
		//OPENBATTLEFAILED {reason}
	} else if (cmd == "JOINFAILED") {
		channel = GetWordParam(params);
		msg = GetSentenceParam(params);
		m_se->OnServerMessage("Failed to join channel #" + channel + ". " + msg);
		//JOINFAILED channame {reason}
	} else if (cmd == "CHANNELMESSAGE") {
		channel = GetWordParam(params);
		m_se->OnChannelMessage(channel, STD_STRING(params));
		//CHANNELMESSAGE channame {message}
	} else if (cmd == "FORCELEAVECHANNEL") {
		channel = GetWordParam(params);
		nick = GetWordParam(params);
		msg = GetSentenceParam(params);
		m_se->OnChannelPart(channel, GetMe().GetNick(), "Kicked by <" + nick + "> " + msg);
		//FORCELEAVECHANNEL channame username [{reason}]
	} else if (cmd == "DENIED") {
		if (m_online)
			return;
		m_last_denied = msg = GetSentenceParam(params);
		m_se->OnLoginDenied(msg);
		Disconnect();
		//Command: "DENIED" params: "Already logged in".
	} else if (cmd == "HOSTPORT") {
		unsigned int tmp_port = (unsigned int)GetIntParam(params);
		m_se->OnHostExternalUdpPort(tmp_port);
		//HOSTPORT port
	} else if (cmd == "UDPSOURCEPORT") {
		unsigned int tmp_port = (unsigned int)GetIntParam(params);
		m_se->OnMyExternalUdpSourcePort(tmp_port);
		if (m_do_finalize_join_battle)
			FinalizeJoinBattle();
		//UDPSOURCEPORT port
	} else if (cmd == "CLIENTIPPORT") {
		// clientipport username ip port
		nick = GetWordParam(params);
		wxString ip = TowxString(GetWordParam(params));
		unsigned int u_port = (unsigned int)GetIntParam(params);
		m_se->OnClientIPPort(nick, STD_STRING(ip), u_port);
	} else if (cmd == "SETSCRIPTTAGS") {
		wxString command;
		while ((command = TowxString(GetSentenceParam(params))) != wxEmptyString) {
			const std::string key = STD_STRING(command.BeforeFirst('=').Lower());
			const std::string value = STD_STRING(command.AfterFirst('='));
			m_se->OnSetBattleInfo(m_battle_id, key, value);
		}
		m_se->OnBattleInfoUpdated(m_battle_id);
		// !! Command: "SETSCRIPTTAGS" params: "game/startpostype=0	game/maxunits=1000	game/limitdgun=0	game/startmetal=1000	game/gamemode=0	game/ghostedbuildings=-1	game/startenergy=1000	game/diminishingmms=0"
	} else if (cmd == "REMOVESCRIPTTAGS") {
		std::string key;
		while ((key = GetWordParam(params)) != "") {
			m_se->OnUnsetBattleInfo(m_battle_id, key);
		}
		m_se->OnBattleInfoUpdated(m_battle_id);
	} else if (cmd == "SCRIPTSTART") {
		m_se->OnScriptStart(m_battle_id);
		// !! Command: "SCRIPTSTART" params: ""
	} else if (cmd == "SCRIPTEND") {
		m_se->OnScriptEnd(m_battle_id);
		// !! Command: "SCRIPTEND" params: ""
	} else if (cmd == "SCRIPT") {
		m_se->OnScriptLine(m_battle_id, STD_STRING(params));
		// !! Command: "SCRIPT" params: "[game]"
	} else if (cmd == "FORCEQUITBATTLE") {
		m_relay_host_bot.clear();
		m_se->OnKickedFromBattle();
	} else if (cmd == "BROADCAST") {
		m_se->OnServerBroadcast(STD_STRING(params));
	} else if (cmd == "SERVERMSGBOX") {
		m_se->OnServerMessageBox(STD_STRING(params));
	} else if (cmd == "REDIRECT") {
		if (m_online)
			return;
		std::string address = GetWordParam(params);
		unsigned int u_port = GetIntParam(params);
		if (address.empty())
			return;
		if (u_port == 0)
			u_port = DEFSETT_DEFAULT_SERVER_PORT;
		m_redirecting = true;
		m_se->OnRedirect(address, u_port, GetUserName(), GetPassword());
	} else if (cmd == "MUTELISTBEGIN") {
		m_current_chan_name_mutelist = GetWordParam(params);
		m_se->OnMutelistBegin(m_current_chan_name_mutelist);

	} else if (cmd == "MUTELIST") {
		const std::string mutee = GetWordParam(params);
		const std::string description = GetSentenceParam(params);
		m_se->OnMutelistItem(m_current_chan_name_mutelist, mutee, description);
	} else if (cmd == "MUTELISTEND") {
		m_se->OnMutelistEnd(m_current_chan_name_mutelist);
		m_current_chan_name_mutelist.clear();
	} else if (cmd == "FORCEJOINBATTLE") {
		const int battleID = GetIntParam(params);
		const std::string scriptpw = GetWordParam(params);
		m_se->OnForceJoinBattle(battleID, scriptpw);
	} else if (cmd == "REGISTRATIONACCEPTED") {
		m_se->RegistrationAccepted(GetUserName(), GetPassword());
	} else if (cmd == "REGISTRATIONDENIED") {
		m_se->RegistrationDenied(STD_STRING(params));
	} else if (cmd == "LISTSUBSCRIPTION") {
		const LSL::StringMap keyvals = parseKeyValue(GetWordParam(params));
		const std::string keyname = "chanName";
		if (keyvals.find(keyname) != keyvals.end()) {
			m_subscriptions.insert(keyvals.at(keyname));
		}
	} else if (cmd == "STARTLISTSUBSCRIPTION") {
		m_subscriptions.clear();
	} else if (cmd == "ENDLISTSUBSCRIPTION") {
		//m_se->OnSubscriptons();
	} else {
		wxLogWarning(wxString::Format(_T("??? Cmd: %s params: %s"), TowxString(cmd).c_str(), params.c_str()));
		m_se->OnUnknownCommand(cmd, STD_STRING(params));
	}
}


void TASServer::RelayCmd(const std::string& command, const std::string& param)
{
	if (m_relay_host_bot.empty()) {
		wxLogWarning(_T("Trying to send relayed commands but no relay bot is set!"));
		return;
	}
	wxString wxcmd = TowxString(command);
	wxString wxparam = TowxString(param);
	wxString msg = _T("!"); // prefix commands with !
	if (param.empty())
		msg = wxcmd.Lower();
	else
		msg = wxcmd.Lower() + _T(" ") + wxparam;
	SayPrivate(m_relay_host_bot, STD_STRING(msg));
}


void TASServer::SendCmd(const std::string& command, const std::string& param, bool relay)
{
	if (relay) {
		RelayCmd(command, param);
		return;
	}

	wxString cmd, msg;
	if (m_id_transmission) {
		m_last_id++;
		msg = msg + _T("#") + TowxString(m_last_id) + _T(" ");
	}
	cmd = TowxString(command);
	if (param.empty())
		msg = msg + cmd + _T("\n");
	else
		msg = msg + cmd + _T(" ") + TowxString(param) + _T("\n");
	bool send_success = m_sock->Send(msg);
	if ((command == "LOGIN") || command == "CHANGEPASSWORD") {
		wxLogMessage(_T("sent: %s ... <password removed>"), TowxString(command).c_str());
		return;
	}

	if (command != "PING") { //don't log PING
		if (send_success)
			wxLogMessage(wxString::Format(_T("sent: %s"), msg.RemoveLast().c_str()));
		else
			wxLogMessage(wxString::Format(_T("sending: %s failed"), msg.RemoveLast().c_str()));
	}
}

void TASServer::SetRelayIngamePassword(const User& user)
{
	IBattle* battle = GetCurrentBattle();
	if (battle) {
		if (!battle->GetInGame())
			return;
	}
	RelayCmd("SETINGAMEPASSWORD", user.GetNick() + std::string(" ") + user.BattleStatus().scriptPassword);
}

void TASServer::Ping()
{
	SendCmd("PING");
	TASPingListItem pli;
	pli.id = m_last_id;
	pli.t = wxGetLocalTimeMillis();
	m_pinglist.push_back(pli);
}

void TASServer::HandlePong(int replyid)
{
	if (m_pinglist.empty()) //safety, shouldn't happen
		return;

	// server connection is tcp, we assume packets are received in order
	TASPingListItem pli = m_pinglist.back();
	m_pinglist.pop_back();
	if (pli.id == replyid) {
		m_se->OnPong((wxGetLocalTimeMillis() - pli.t));
	}
}


void TASServer::JoinChannel(const std::string& channel, const std::string& key)
{
	//JOIN channame [key]
	slLogDebugFunc("");

	m_channel_pw[channel] = key;
	m_id_transmission = false; // workaround a retarded server bug
	SendCmd("JOIN", channel + std::string(" ") + key);
	m_id_transmission = true;
}


void TASServer::PartChannel(const std::string& channel)
{
	//LEAVE channame
	slLogDebugFunc("");

	SendCmd("LEAVE", channel);
}


void TASServer::DoActionChannel(const std::string& channel, const std::string& msg)
{
	//SAYEX channame {message}
	slLogDebugFunc("");

	SendCmd("SAYEX", channel + std::string(" ") + msg);
}


void TASServer::SayChannel(const std::string& channel, const std::string& msg)
{
	//SAY channame {message}
	slLogDebugFunc("");

	SendCmd("SAY", channel + std::string(" ") + msg);
}


void TASServer::SayPrivate(const std::string& nick, const std::string& msg)
{
	//SAYPRIVATE username {message}
	slLogDebugFunc("");

	SendCmd("SAYPRIVATE", nick + std::string(" ") + msg);
}


void TASServer::DoActionPrivate(const std::string& nick, const std::string& msg)
{
	slLogDebugFunc("");
	SendCmd("SAYPRIVATEEX", nick + std::string(" ") + msg);
}


void TASServer::SayBattle(int /*unused*/, const std::string& msg)
{
	slLogDebugFunc("");
	SendCmd("SAYBATTLE", msg);
}


void TASServer::DoActionBattle(int /*unused*/, const std::string& msg)
{
	slLogDebugFunc("");
	SendCmd("SAYBATTLEEX", msg);
}


void TASServer::Ring(const std::string& nick)
{
	slLogDebugFunc("");
	try {
		ASSERT_EXCEPTION(m_battle_id != -1, _T("invalid m_battle_id value"));
		ASSERT_EXCEPTION(BattleExists(m_battle_id), _T("battle doesn't exists"));

		IBattle& battle = GetBattle(m_battle_id);
		ASSERT_EXCEPTION(battle.IsFounderMe(), _T("I'm not founder"));

		SendCmd("RING", nick, battle.IsProxy());

	} catch (...) {
		SendCmd("RING", nick);
	}
}


void TASServer::ModeratorSetChannelTopic(const std::string& channel, const std::string& topic)
{
	wxString msgcopy = TowxString(topic);
	msgcopy.Replace(_T("\n"), _T("\\n"));
	SendCmd("CHANNELTOPIC", channel + std::string(" ") + STD_STRING(msgcopy));
}


void TASServer::ModeratorSetChannelKey(const std::string& channel, const std::string& key)
{
	SendCmd("SETCHANNELKEY", channel + std::string(" ") + key);
}


void TASServer::ModeratorMute(const std::string& channel, const std::string& nick, int duration, bool byip)
{
	std::string mutebyip = byip ? " ip" : "";
	SendCmd("MUTE", stdprintf("%s %s %d %s", channel.c_str(), nick.c_str(), duration, mutebyip.c_str()));
}


void TASServer::ModeratorUnmute(const std::string& channel, const std::string& nick)
{
	SendCmd("UNMUTE", channel + std::string(" ") + nick);
}


void TASServer::ModeratorKick(const std::string& channel, const std::string& reason)
{
	SendCmd("KICKUSER", channel + std::string(" ") + reason);
}


void TASServer::ModeratorBan(const std::string& /*unused*/, bool /*unused*/)
{
	// FIXME TASServer::ModeratorBan not yet implemented
}


void TASServer::ModeratorUnban(const std::string& /*unused*/)
{
	// FIXME TASServer::ModeratorUnban not yet implemented
}


void TASServer::ModeratorGetIP(const std::string& nick)
{
	SendCmd("GETIP", nick);
}


void TASServer::ModeratorGetLastLogin(const std::string& nick)
{
	SendCmd("GETLASTLOGINTIME", nick);
}


void TASServer::ModeratorGetLastIP(const std::string& nick)
{
	SendCmd("GETLASTIP", nick);
}


void TASServer::ModeratorFindByIP(const std::string& ipadress)
{
	SendCmd("FINDIP", ipadress);
}


void TASServer::AdminGetAccountAccess(const std::string& /*unused*/)
{
	// FIXME TASServer::AdminGetAccountAccess not yet implemented
}


void TASServer::AdminChangeAccountAccess(const std::string& /*unused*/, const std::string& /*unused*/)
{
	// FIXME TASServer::AdminChangeAccountAccess not yet implemented
}


void TASServer::AdminSetBotMode(const std::string& nick, bool isbot)
{
	SendCmd("SETBOTMODE", nick + std::string(" ") + (isbot ? "1" : "0"));
}


void TASServer::HostBattle(BattleOptions bo, const std::string& password)
{
	slLogDebugFunc("");

	// to see ip addresses of users as they join (in the log), pretend you're hosting with NAT.
	int nat_type = bo.nattype;
	/*
	if(nat_type==0 && sett().GetShowIPAddresses()){
	nat_type=1;
	}*/
	wxLogMessage(_T("hosting with nat type %d"), nat_type);
	wxString wxpassword = TowxString(password);
	wxString cmd = wxString::Format(_T("0 %d "), nat_type);
	cmd += (wxpassword.IsEmpty()) ? _T("*") : wxpassword;
	cmd += wxString::Format(_T(" %d %d "), bo.port, bo.maxplayers);
	cmd += TowxString(LSL::Util::MakeHashSigned(bo.modhash));
	cmd += wxString::Format(_T(" %d "), bo.rankneeded);
	cmd += TowxString(LSL::Util::MakeHashSigned(bo.maphash) + std::string(" "));
	if (!bo.userelayhost) { //FIXME: relay host hasn't multiversion support yet, see https://github.com/springlobby/springlobby/issues/98
		cmd += TowxString(bo.engineName) + _T("\t");
		cmd += TowxString(bo.engineVersion) + _T("\t");
	}
	cmd += TowxString(bo.mapname) + _T("\t");
	cmd += TowxString(bo.description) + _T("\t");
	cmd += TowxString(bo.modname);

	m_delayed_open_command = "";
	if (!bo.userelayhost) {
		SendCmd("OPENBATTLE", STD_STRING(cmd));
	} else {
		if (bo.relayhost.empty()) {
			LSL::StringVector relaylist = GetRelayHostList();
			unsigned int numbots = relaylist.size();
			if (numbots > 0) {
				srand(time(NULL));
				unsigned int choice = rand() % numbots;
				m_relay_host_manager = relaylist[choice];
				m_delayed_open_command = STD_STRING(cmd);
				SayPrivate(m_relay_host_manager, "!spawn");
			}
		} else {
			m_relay_host_manager = bo.relayhost;
			m_delayed_open_command = STD_STRING(cmd);
			SayPrivate(bo.relayhost, "!spawn");
		}
	}

	if (bo.nattype > 0)
		UdpPingTheServer(GetUserName());

	// OPENBATTLE type natType password port maphash {map} {title} {modname}
}


void TASServer::JoinBattle(const int& battleid, const std::string& password)
{
	//JOINBATTLE BATTLE_ID [parameter]
	slLogDebugFunc("");

	m_finalize_join_battle_pw = password;
	m_finalize_join_battle_id = battleid;

	if (BattleExists(battleid)) {
		IBattle* battle = &GetBattle(battleid);

		if (battle) {
			if ((battle->GetNatType() == NAT_Hole_punching) || (battle->GetNatType() == NAT_Fixed_source_ports)) {
				m_udp_private_port = sett().GetClientPort();

				m_last_udp_ping = 0;
				// its important to set time now, to prevent Update()
				// from calling FinalizeJoinBattle() on timeout.
				// m_do_finalize_join_battle must be set to true after setting time, not before.
				m_do_finalize_join_battle = true;
				for (int n = 0; n < 5; ++n) { // do 5 udp pings with tiny interval
					UdpPingTheServer(GetUserName());
					// sleep(0);// sleep until end of timeslice.
				}
				m_last_udp_ping = 0; // set time again
			} else {
				// if not using nat, finalize now.
				m_do_finalize_join_battle = true;
				FinalizeJoinBattle();
			}
		} else {
			wxLogMessage(_T("battle doesnt exist (null)"));
		}
	} else {
		wxLogMessage(_T("battle doesnt exist"));
	}
	//SendCmd( _T("JOINBATTLE"), wxString::Format( _T("%d"), battleid ) + std::string(" ") + password );
}


void TASServer::FinalizeJoinBattle()
{
	if (m_do_finalize_join_battle) {
		// save scriptPassword so user can rejoin if SL crashes or
		// loosed connection
		std::string scriptPassword;
		if (sett().GetLastBattleId() == m_finalize_join_battle_id) {
			scriptPassword = STD_STRING(sett().GetLastScriptPassword());
		} else {
			srand(time(NULL));
			scriptPassword = stdprintf("%04x%04x", rand() & 0xFFFF, rand() & 0xFFFF);
			// save assword and write settings file
			sett().SetLastBattleId(m_finalize_join_battle_id);
			sett().SetLastScriptPassword(TowxString(scriptPassword));
			sett().SaveSettings();
		}
		SendCmd("JOINBATTLE", stdprintf("%d %s %s", m_finalize_join_battle_id, m_finalize_join_battle_pw.c_str(), scriptPassword.c_str()));
		m_do_finalize_join_battle = false;
	}
}


void TASServer::LeaveBattle(const int& /*unused*/)
{
	//LEAVEBATTLE
	slLogDebugFunc("");
	m_relay_host_bot.clear();
	SendCmd("LEAVEBATTLE");
}


void TASServer::SendHostInfo(HostInfo update)
{
	slLogDebugFunc("");

	CHECK_BATTLE_ID();

	IBattle& battle = GetBattle(m_battle_id);
	try {
		ASSERT_LOGIC(battle.IsFounderMe(), "I'm not founder");
	} catch (...) {
		return;
	}

	//BattleOptions bo = battle.opts();

	if ((update & (IBattle::HI_Map | IBattle::HI_Locked | IBattle::HI_Spectators)) > 0) {
		// UPDATEBATTLEINFO Spectatorsize locked maphash {mapname}
		wxString cmd = wxString::Format(_T("%d %d "), battle.GetSpectators(), battle.IsLocked());
		cmd += TowxString(LSL::Util::MakeHashSigned(battle.LoadMap().hash) + " ");
		cmd += TowxString(battle.LoadMap().name);

		SendCmd("UPDATEBATTLEINFO", STD_STRING(cmd), battle.IsProxy());
	}
	if ((update & IBattle::HI_Send_All_opts) > 0) {
		std::string cmd;
		for (const auto& it : battle.CustomBattleOptions().getOptions(LSL::Enum::MapOption)) {
			const std::string newcmd = "game/mapoptions/" + it.first + "=" + it.second.second + "\t";
			if (cmd.size() + newcmd.size() > 900) { // should be 1024 add margin for relayhost name and command itself
				SendCmd("SETSCRIPTTAGS", cmd, battle.IsProxy());
				cmd.clear();
			}
			cmd += newcmd;
		}
		for (const auto& it : battle.CustomBattleOptions().getOptions(LSL::Enum::ModOption)) {
			const std::string newcmd = "game/modoptions/" + it.first + "=" + it.second.second + "\t";
			if (cmd.size() + newcmd.size() > 900) { // should be 1024 add margin for relayhost name and command itself
				SendCmd("SETSCRIPTTAGS", cmd, battle.IsProxy());
				cmd.clear();
			}
			cmd += newcmd;
		}
		for (const auto& it : battle.CustomBattleOptions().getOptions(LSL::Enum::EngineOption)) {
			const std::string newcmd = "game/" + it.first + "=" + it.second.second + "\t";
			if (cmd.size() + newcmd.size() > 900) { // should be 1024 add margin for relayhost name and command itself
				SendCmd("SETSCRIPTTAGS", cmd, battle.IsProxy());
				cmd.clear();
			}
			cmd += newcmd;
		}
		SendCmd("SETSCRIPTTAGS", cmd, battle.IsProxy());
	}

	if ((update & IBattle::HI_StartRects) > 0) { // Startrects should be updated.
		unsigned int numrects = battle.GetLastRectIdx();
		for (unsigned int i = 0; i <= numrects; i++) { // Loop through all, and remove updated or deleted.
			wxString cmd;
			BattleStartRect sr = battle.GetStartRect(i);
			if (!sr.exist)
				continue;
			if (sr.todelete) {
				SendCmd("REMOVESTARTRECT", stdprintf("%d", i), battle.IsProxy());
				battle.StartRectRemoved(i);
			} else if (sr.toadd) {
				SendCmd("ADDSTARTRECT", stdprintf("%d %d %d %d %d", sr.ally, sr.left, sr.top, sr.right, sr.bottom), battle.IsProxy());
				battle.StartRectAdded(i);
			} else if (sr.toresize) {
				SendCmd("REMOVESTARTRECT", stdprintf("%d", i), battle.IsProxy());
				SendCmd("ADDSTARTRECT", stdprintf("%d %d %d %d %d", sr.ally, sr.left, sr.top, sr.right, sr.bottom), battle.IsProxy());
				battle.StartRectResized(i);
			}
		}
	}
	if ((update & IBattle::HI_Restrictions) > 0) {
		std::map<std::string, int> units = battle.RestrictedUnits();
		SendCmd("ENABLEALLUNITS", "", battle.IsProxy());
		if (!units.empty()) {
			wxString msg;
			wxString scriptmsg;
			for (std::map<std::string, int>::const_iterator itor = units.begin(); itor != units.end(); ++itor) {
				msg << TowxString(itor->first) + _T(" ");
				scriptmsg << _T("game/restrict/") + TowxString(itor->first) + _T("=") + TowxString(itor->second) + _T('\t'); // this is a serious protocol abuse, but on the other hand, the protocol fucking suck and it's unmaintained so it will do for now
			}
			SendCmd("DISABLEUNITS", STD_STRING(msg), battle.IsProxy());
			SendCmd("SETSCRIPTTAGS", STD_STRING(scriptmsg), battle.IsProxy());
		}
	}
}


void TASServer::SendHostInfo(const std::string& Tag)
{
	slLogDebugFunc("");

	CHECK_BATTLE_ID();

	IBattle& battle = GetBattle(m_battle_id);

	try {
		ASSERT_LOGIC(battle.IsFounderMe(), "I'm not founder");
	} catch (...) {
		return;
	}

	std::string cmd;
	const long type = LSL::Util::FromString<long>(LSL::Util::BeforeFirst(Tag, "_"));
	const std::string key = LSL::Util::AfterFirst(Tag, "_");

	switch (type) {
		case LSL::Enum::MapOption:
			cmd = "game/mapoptions/" + key + "=" + battle.CustomBattleOptions().getSingleValue(key, LSL::Enum::MapOption);
			break;
		case LSL::Enum::ModOption:
			cmd = "game/modoptions/" + key + "=" + battle.CustomBattleOptions().getSingleValue(key, LSL::Enum::ModOption);
			break;
		case LSL::Enum::EngineOption:
			cmd = "game/" + key + "=" + battle.CustomBattleOptions().getSingleValue(key, LSL::Enum::EngineOption);
			break;
	}
	SendCmd("SETSCRIPTTAGS", cmd, battle.IsProxy());
}


void TASServer::SendUserPosition(const User& user)
{
	slLogDebugFunc("");

	try {
		ASSERT_LOGIC(m_battle_id != -1, "invalid m_battle_id value");
		ASSERT_LOGIC(BattleExists(m_battle_id), "battle doesn't exists");

		IBattle& battle = GetBattle(m_battle_id);
		ASSERT_LOGIC(battle.IsFounderMe(), "I'm not founder");

		UserBattleStatus status = user.BattleStatus();
		std::string msgx = stdprintf("game/Team%d/StartPosX=%d", status.team, status.pos.x);
		std::string msgy = stdprintf("game/Team%d/StartPosY=%d", status.team, status.pos.y);
		std::string netmessage = msgx + "\t" + msgy;
		SendCmd("SETSCRIPTTAGS", netmessage, battle.IsProxy());
	} catch (...) {
		return;
	}
}

void TASServer::RequestInGameTime(const std::string& nick)
{
	SendCmd("GETINGAMETIME", nick);
}


IBattle* TASServer::GetCurrentBattle()
{
	try {
		ASSERT_EXCEPTION(m_battle_id != -1, _T("invalid m_battle_id value"));
		ASSERT_LOGIC(BattleExists(m_battle_id), "battle doesn't exists");
	} catch (...) {
		return NULL;
	}

	return &GetBattle(m_battle_id);
}


void TASServer::SendMyBattleStatus(UserBattleStatus& bs)
{
	slLogDebugFunc("");
	const int tasbs = ConvTasbattlestatus(bs);
	//MYBATTLESTATUS battlestatus myteamcolor
	SendCmd("MYBATTLESTATUS", stdprintf("%d %d", tasbs, bs.colour.GetLobbyColor()));
}


void TASServer::SendMyUserStatus(const UserStatus& us)
{
	slLogDebugFunc("");
	const int taus = ConvUserStatus(us);
	SendCmd("MYSTATUS", stdprintf("%d", taus));
}


void TASServer::StartHostedBattle()
{
	slLogDebugFunc("");
	CHECK_BATTLE_ID();

	IBattle* battle = GetCurrentBattle();
	if (battle) {
		if ((battle->GetNatType() == NAT_Hole_punching) || (battle->GetNatType() == NAT_Fixed_source_ports)) {
			UdpPingTheServer(GetUserName());
			for (int i = 0; i < 5; ++i)
				UdpPingAllClients();
		}
	}

	m_se->OnStartHostedBattle(m_battle_id);
}


void TASServer::ForceSide(int battleid, User& user, int side)
{
	CHECK_CURRENT_BATTLE_ID(battleid);

	UserBattleStatus status = user.BattleStatus();

	if (&user == &GetMe()) {
		status.side = side;
		SendMyBattleStatus(status);
		return;
	}

	if (status.IsBot()) {
		status.side = side;
		UpdateBot(battleid, user, status);
	}
}


void TASServer::ForceTeam(int battleid, User& user, int team)
{
	CHECK_CURRENT_BATTLE_ID(battleid);

	UserBattleStatus status = user.BattleStatus();
	if (status.IsBot()) {
		status.team = team;
		UpdateBot(battleid, user, status);
		return;
	}
	if (&user == &GetMe()) {
		status.team = team;
		SendMyBattleStatus(status);
		return;
	}
	if (!GetBattle(battleid).IsFounderMe()) {
		DoActionBattle(battleid, "suggests that " + user.GetNick() + " changes to team #" + stdprintf("%d", team + 1) + ".");
		return;
	}

	//FORCETEAMNO username teamno
	SendCmd("FORCETEAMNO", stdprintf("%s %d", user.GetNick().c_str(), team), GetBattle(battleid).IsProxy());
}


void TASServer::ForceAlly(int battleid, User& user, int ally)
{
	CHECK_CURRENT_BATTLE_ID(battleid);

	UserBattleStatus status = user.BattleStatus();
	if (status.IsBot()) {
		status.ally = ally;
		UpdateBot(battleid, user, status);
		return;
	}

	if (&user == &GetMe()) {
		status.ally = ally;
		SendMyBattleStatus(status);
		return;
	}

	if (!GetBattle(battleid).IsFounderMe()) {
		DoActionBattle(battleid, "suggests that " + user.GetNick() + " changes to ally #" + stdprintf("%d", ally + 1) + ".");
		return;
	}

	//FORCEALLYNO username teamno
	SendCmd("FORCEALLYNO", user.GetNick() + stdprintf(" %d", ally), GetBattle(battleid).IsProxy());
}


void TASServer::ForceColour(int battleid, User& user, const LSL::lslColor& col)
{
	CHECK_CURRENT_BATTLE_ID(battleid);

	UserBattleStatus status = user.BattleStatus();
	if (status.IsBot()) {
		status.colour = col;
		UpdateBot(battleid, user, status);
		return;
	}
	if (&user == &GetMe()) {
		status.colour = col;
		SendMyBattleStatus(status);
		return;
	}
	if (!GetBattle(battleid).IsFounderMe()) {
		DoActionBattle(battleid, "sugests that " + user.GetNick() + " changes colour.");
		return;
	}

	//FORCETEAMCOLOR username color
	SendCmd("FORCETEAMCOLOR", user.GetNick() + std::string(" ") + stdprintf("%d", col.GetLobbyColor()), GetBattle(battleid).IsProxy());
}


void TASServer::ForceSpectator(int battleid, User& user, bool spectator)
{
	CHECK_CURRENT_BATTLE_ID(battleid);

	UserBattleStatus status = user.BattleStatus();
	if (status.IsBot()) {
		status.spectator = spectator;
		UpdateBot(battleid, user, status);
		return;
	}
	if (&user == &GetMe()) {
		status.spectator = spectator;
		SendMyBattleStatus(status);
		return;
	}
	if (!GetBattle(battleid).IsFounderMe()) {
		if (spectator)
			DoActionBattle(battleid, "suggests that " + user.GetNick() + " becomes a spectator.");
		else
			DoActionBattle(battleid, "suggests that " + user.GetNick() + " plays.");
		return;
	}

	//FORCESPECTATORMODE username
	SendCmd("FORCESPECTATORMODE", user.GetNick(), GetBattle(battleid).IsProxy());
}


void TASServer::BattleKickPlayer(int battleid, User& user)
{
	CHECK_CURRENT_BATTLE_ID(battlid)

	UserBattleStatus status = user.BattleStatus();
	if (status.IsBot()) {
		RemoveBot(battleid, user);
		return;
	}
	if (&user == &GetMe()) {
		LeaveBattle(battleid);
		return;
	}
	if (!GetBattle(battleid).IsFounderMe()) {
		DoActionBattle(battleid, "thinks " + user.GetNick() + " should leave.");
		return;
	}

	//KICKFROMBATTLE username
	if (!GetBattle(battleid).IsProxy()) {
		user.BattleStatus().scriptPassword = stdprintf("%04x%04x", rand() & 0xFFFF, rand() & 0xFFFF); // reset his password to something random, so he can't rejoin
		SetRelayIngamePassword(user);
	}
	SendCmd("KICKFROMBATTLE", user.GetNick(), GetBattle(battleid).IsProxy());
}

void TASServer::SetHandicap(int battleid, User& user, int handicap)
{
	CHECK_CURRENT_BATTLE_ID(battleid)

	UserBattleStatus status = user.BattleStatus();
	if (status.IsBot()) {
		status.handicap = handicap;
		UpdateBot(battleid, user, status);
		return;
	}

	if (!GetBattle(battleid).IsFounderMe()) {
		DoActionBattle(battleid, stdprintf("thinks %s should get a %d% resource bonus", user.GetNick().c_str(), handicap));
		return;
	}

	//HANDICAP username value
	SendCmd("HANDICAP", user.GetNick() + stdprintf(" %d", handicap), GetBattle(battleid).IsProxy());
}


void TASServer::AddBot(int battleid, const std::string& nick, UserBattleStatus& status)
{
	CHECK_CURRENT_BATTLE_ID(battleid)

	const int tasbs = ConvTasbattlestatus(status);
	//ADDBOT name battlestatus teamcolor {AIDLL}
	wxString msg;
	wxString ailib;
	ailib += TowxString(status.aishortname);
	ailib += _T("|") + TowxString(status.aiversion);
	SendCmd("ADDBOT", nick + stdprintf(" %d %d ", tasbs, status.colour.GetLobbyColor()) + STD_STRING(ailib));
}


void TASServer::RemoveBot(int battleid, User& bot)
{
	CHECK_CURRENT_BATTLE_ID(battleid)

	IBattle& battle = GetBattle(battleid);
	ASSERT_LOGIC(&bot != 0, "Bot does not exist.");

	if (!(battle.IsFounderMe() || (bot.BattleStatus().owner == GetMe().GetNick()))) {
		DoActionBattle(battleid, "thinks the bot " + bot.GetNick() + " should be removed.");
		return;
	}

	//REMOVEBOT name
	SendCmd("REMOVEBOT", bot.GetNick(), GetBattle(battleid).IsProxy());
}


void TASServer::UpdateBot(int battleid, User& bot, UserBattleStatus& status)
{
	CHECK_CURRENT_BATTLE_ID(battleid)
	const int tasbs = ConvTasbattlestatus(status);
	//UPDATEBOT name battlestatus teamcolor
	SendCmd("UPDATEBOT", bot.GetNick() + stdprintf(" %d %d", tasbs, status.colour.GetLobbyColor()), GetBattle(battleid).IsProxy());
}

void TASServer::OnConnected()
{
	slLogDebugFunc("");
	//TASServer* serv = (TASServer*)sock->GetUserdata();
	m_connected = true;
	m_online = false;
	m_relay_host_manager_list.clear();
	m_last_denied.clear();
	m_last_id = 0;
	m_pinglist.clear();
}


void TASServer::OnDisconnected()
{
	slLogDebugFunc("%d", m_connected);
	const bool connectionwaspresent = m_online || !m_last_denied.empty() || m_redirecting;
	m_last_denied.clear();
	m_connected = false;
	m_online = false;
	m_redirecting = false;
	m_buffer.clear();
	m_relay_host_manager_list.clear();
	m_last_id = 0;
	m_pinglist.clear();
	if (m_se != NULL) {
		IServer::Reset();
		m_se->OnDisconnected(connectionwaspresent);
	}
}


void TASServer::OnDataReceived()
{
	if (m_sock == NULL)
		return;

	m_last_net_packet = 0;
	wxString data = m_sock->Receive();
	m_buffer += STD_STRING(data);
	LSL::Util::Replace(m_buffer, "\r\n", "\n");

	size_t returnpos = m_buffer.find("\n");
	while (returnpos != std::string::npos) {
		const std::string cmd = m_buffer.substr(0, returnpos);
		m_buffer = m_buffer.substr(returnpos + 1, m_buffer.size() - (returnpos + 1));
		ExecuteCommand(cmd);
		returnpos = m_buffer.find("\n");
	}
}

void TASServer::OnError(const std::string& err)
{
	wxLogWarning(TowxString(err));
	if (m_se != NULL) {
		m_se->OnServerMessage(err);
	}
}


//! @brief Send udp ping.
//! @note used for nat travelsal.

unsigned int TASServer::UdpPing(unsigned int src_port, const std::string& target, unsigned int target_port, const std::string& message) // full parameters version, used to ping all clients when hosting.
{
	int result = 0;
	wxLogMessage(_T("UdpPing src_port=%d , target='%s' , target_port=%d , message='%s'"), src_port, target.c_str(), target_port, message.c_str());
	wxIPV4address local_addr;
	local_addr.AnyAddress(); // <--- THATS ESSENTIAL!
	local_addr.Service(src_port);

	wxDatagramSocket udp_socket(local_addr, /* wxSOCKET_WAITALL*/ wxSOCKET_NONE);

	wxIPV4address wxaddr;
	wxaddr.Hostname(TowxString(target));
	wxaddr.Service(target_port);

	if (udp_socket.IsOk() && !udp_socket.Error()) {
		udp_socket.SendTo(wxaddr, message.c_str(), message.length());
		wxIPV4address true_local_addr;
		if (udp_socket.GetLocal(true_local_addr)) {
			result = true_local_addr.Service();
		}
	} else {
		wxLogMessage(_T("socket's IsOk() is false, no UDP ping done."));
	}

	if (udp_socket.Error()) {
		wxLogWarning(_T("wxDatagramSocket Error=%d"), udp_socket.LastError());
	}
	return result;
}

void TASServer::UdpPingTheServer(const std::string& message)
{
	unsigned int port = UdpPing(m_udp_private_port, m_addr, m_nat_helper_port, message);
	if (port > 0) {
		m_udp_private_port = port;
		m_se->OnMyInternalUdpSourcePort(m_udp_private_port);
	}
}


// copypasta from spring.cpp , to get users ordered same way as in tasclient.
struct UserOrder
{
	int index; // user number for GetUser
	int order; // user order (we'll sort by it)
	bool operator<(UserOrder b) const
	{ // comparison function for sorting
		return order < b.order;
	}
};


void TASServer::UdpPingAllClients() // used when hosting with nat holepunching. has some rudimentary support for fixed source ports.
{
	IBattle* battle = GetCurrentBattle();
	if (!battle)
		return;
	if (!battle->IsFounderMe())
		return;
	wxLogMessage(_T("UdpPingAllClients()"));

	// I'm gonna mimic tasclient's behavior.
	// It of course doesnt matter in which order pings are sent,
	// but when doing "fixed source ports", the port must be
	// FIRST_UDP_SOURCEPORT + index of user excluding myself
	// so users must be reindexed in same way as in tasclient
	// to get same source ports for pings.


	// copypasta from spring.cpp
	std::vector<UserOrder> ordered_users;


	for (UserList::user_map_t::size_type i = 0; i < battle->GetNumUsers(); i++) {
		User& user = battle->GetUser(i);
		if (&user == &(battle->GetMe()))
			continue; // dont include myself (change in copypasta)

		UserOrder tmp;
		tmp.index = i;
		ordered_users.push_back(tmp);
	}
	std::sort(ordered_users.begin(), ordered_users.end());


	for (int i = 0; i < int(ordered_users.size()); ++i) {
		User& user = battle->GetUser(ordered_users[i].index);

		wxString ip = TowxString(user.BattleStatus().ip);
		unsigned int port = user.BattleStatus().udpport;

		unsigned int src_port = m_udp_private_port;
		if (battle->GetNatType() == NAT_Fixed_source_ports) {
			port = FIRST_UDP_SOURCEPORT + i;
		}

		wxLogMessage(_T(" pinging nick=%s , ip=%s , port=%u"), user.GetNick().c_str(), ip.c_str(), port);

		if (port != 0 && !ip.empty()) {
			UdpPing(src_port, STD_STRING(ip), port, "hai!");
		}
	}
}


//! @brief used to check if the NAT is done properly when hosting
int TASServer::TestOpenPort(unsigned int port) const
{
	wxIPV4address local_addr;
	local_addr.AnyAddress(); // <--- THATS ESSENTIAL!
	local_addr.Service(port);

	wxSocketServer udp_socket(local_addr, wxSOCKET_NONE);

	wxHTTP connect_to_server;
	connect_to_server.SetTimeout(10);

	if (!connect_to_server.Connect(_T("zjt3.com")))
		return porttest_unreachable;
	connect_to_server.GetInputStream(wxString::Format(_T("/porttest.php?port=%u"), port));

	if (udp_socket.IsOk()) {
		if (!udp_socket.WaitForAccept(10))
			return porttest_timeout;
	} else {
		wxLogMessage(_T("socket's IsOk() is false, no UDP packets can be checked"));
		return porttest_socketNotOk;
	}
	if (udp_socket.Error()) {
		wxLogMessage(_T("Error=%d"), udp_socket.LastError());
		return porttest_socketError;
	}
	return porttest_pass;
}

LSL::StringVector TASServer::GetRelayHostList()
{
	if (UserExists("RelayHostManagerList"))
		SayPrivate("RelayHostManagerList", "!lm");
	LSL::StringVector ret;
	for (unsigned int i = 0; i < m_relay_host_manager_list.size(); i++) {
		try {
			User& manager = GetUser(m_relay_host_manager_list[i]);
			if (manager.Status().in_game)
				continue; // skip the manager is not connected or reports it's ingame ( no slots available ), or it's away ( functionality disabled )
			if (manager.Status().away)
				continue;
			ret.push_back(m_relay_host_manager_list[i]);
		} catch (...) {
		}
	}
	return ret;
}

////////////////////////
// Utility functions
//////////////////////

int ConvUserStatus(const UserStatus& us)
{
	int taus = 0;
	taus += (us.in_game ? 1 : 0);
	taus += (us.away ? 1 : 0) << 1;
	taus += (us.rank % 16) << 2;
	taus += (us.moderator ? 1 : 0) << 5;
	taus += (us.bot ? 1 : 0) << 6;
	return taus;
}


UserStatus ConvTasclientstatus(const int tas)
{
	//http://springrts.com/dl/LobbyProtocol/ProtocolDescription.html#MYSTATUS:client
	UserStatus stat;
	stat.in_game = (tas >> 0) & 1;
	stat.away = (tas >> 1) & 1;
	stat.rank = (UserStatus::RankContainer)((tas >> 2) % 8);
	stat.moderator = (tas >> 5) & 1;
	stat.bot = (tas >> 6) & 1;
	assert(ConvUserStatus(stat) == tas);
	return stat;
}

UserBattleStatus ConvTasbattlestatus(const int tas)
{
	UserBattleStatus stat;
	//http://springrts.com/dl/LobbyProtocol/ProtocolDescription.html#MYBATTLESTATUS:client
	stat.ready = (tas >> 1) & 1;
	stat.team = (tas >> 2) & 15;
	stat.ally = (tas >> 6) & 15;
	stat.spectator = ((tas >> 10) & 1) == 0;
	stat.handicap = ((tas >> 11) & 127) % 101;
	stat.sync = ((tas >> 22) & 3) % 3;
	stat.side = (tas >> 24) & 15;
	assert(tas == ConvTasbattlestatus(stat));
	return stat;
}


int ConvTasbattlestatus(UserBattleStatus bs)
{
	int ret = 0;			     // b0 is reserved
	ret += (bs.ready ? 1 : 0) << 1;      // b1
	ret += (bs.team % 16) << 2;	  //b2..b5
	ret += (bs.ally % 16) << 6;	  //b6..b9
	ret += (bs.spectator ? 0 : 1) << 10; //b10
	ret += (bs.handicap % 101) << 11;    //b11..b17
	//b18..b21 reserverd
	ret += (bs.sync % 3) << 22;  //b22..b23
	ret += (bs.side % 16) << 24; //b24..b27
	//b28..31 is unused
	return ret;
}


IBattle::StartType IntToStartType(int start)
{
	switch (start) {
		case 0:
			return IBattle::ST_Fixed;
		case 1:
			return IBattle::ST_Random;
		case 2:
			return IBattle::ST_Choose;
		default:
			ASSERT_EXCEPTION(false, _T("invalid value"));
	};
	return IBattle::ST_Fixed;
}


NatType IntToNatType(int nat)
{
	switch (nat) {
		case 0:
			return NAT_None;
		case 1:
			return NAT_Hole_punching;
		case 2:
			return NAT_Fixed_source_ports;
		default:
			ASSERT_EXCEPTION(false, _T("invalid value"));
	};
	return NAT_None;
}


IBattle::GameType IntToGameType(int gt)
{
	switch (gt) {
		case 0:
			return IBattle::GT_ComContinue;
		case 1:
			return IBattle::GT_ComEnds;
		case 2:
			return IBattle::GT_Lineage;
		default:
			ASSERT_EXCEPTION(false, _T("invalid value"));
	};
	return IBattle::GT_ComContinue;
}
