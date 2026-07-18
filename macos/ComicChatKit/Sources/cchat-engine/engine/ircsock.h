// ircsock.h — PARTIAL LIFT from v2.5-beta-1-modern/ircsock.h. Plan 3 Task 5b:
// the INBOUND PARSER. Lifts ParseIt/NGetCmd/ParseChannelMode (pure parse,
// byte-identical) + the numeric constant tables + the IRCPARSE struct +
// g_rgIrcCmd + enumCmdId, and declares the parse-dispatch entry points.
//
// STRUCTURAL DEVIATIONS from the original ircsock.h (all documented in
// p3-task-5b-report.md):
//   * CIrcSocket does NOT derive from CAsyncSocket (R21: Swift owns the socket)
//     -- the CIrcSocket class itself lives in ircproto.h (grown by Task 5b to
//     hold the re-introduced inbound state); this header only carries the parse
//     tables + free-function parse layer.
//   * The three Handle* dispatch methods + ProcessMessage become FREE FUNCTIONS
//     taking a CCSession& (the engine holds one CIrcProto per session, so there
//     is no per-doc `this` to be a method of; currentRoom == the session's one
//     CIrcProto). Every direct app-callee inside them becomes a
//     ccEmitProtoEvent (R18) -- see ircsock.cpp for the per-call-site map.
//   * The SSPI/NTLM auth block (original :413-998) is DROPPED (R21); the login
//     helpers reduce to HrIrcLogin (PASS/NICK/USER) + HrModeIsIrcXFailure +
//     the 800/IRCX capability parse. AUTH emits CC_EV_AUTH_UNSUPPORTED.
//   * CIrcPrint (status-window formatting) is DROPPED -- every SetFormat call
//     was "how should this line appear in the Status Window"; that is a Swift
//     UI concern. Status-worthy lines emit CC_EV_STATUS_LINE instead.
#ifndef __IRCSOCK_ENGINE_H__
#define __IRCSOCK_ENGINE_H__

#include "mfc_compat.h"
#include "defines.h"    // MAX_TOKEN/MAX_NICK (MODECACH) + CM_*/MT_*/CHANNELPREFIX
#include "query.h"
#include "ircproto.h"   // CIrcSocket/CIrcProto (grown by Task 5b)

// --- constants (verbatim, ircsock.h:12-55) ---------------------------------
const short g_nDefaultIOBuff = 512;   // By default, for IRC servers

#define MAXARGS							10

#define MC_NONE							0
#define MC_HOSTLOST						1
#define MC_OWNERLOST					2

// --- IRC Result Codes (verbatim, ircsock.h:57-161) -------------------------
const INT RPL_IRCSTART					= 1;
const INT RPL_WELCOME					= 1;
const INT RPL_YOURHOST					= 2;
const INT RPL_CREATED					= 3;
const INT RPL_MYINFO					= 4;
const INT RPL_FOOFORNOW					= 5;

const INT RPL_TRACELINK					= 200;
const INT RPL_TRACECONNECTING			= 201;
const INT RPL_TRACEHANDSHAKE			= 202;
const INT RPL_TRACEUNKNOWN				= 203;
const INT RPL_TRACEOPERATOR				= 204;
const INT RPL_TRACEUSER					= 205;
const INT RPL_TRACESERVER				= 206;
const INT RPL_TRACENEWTYPE				= 208;
const INT RPL_TRACELOG					= 261;

const INT RPL_STATSLINKINFO				= 211;
const INT RPL_STATSCOMMANDS				= 212;
const INT RPL_STATSCLINE				= 213;
const INT RPL_STATSNLINE				= 214;
const INT RPL_STATSILINE				= 215;
const INT RPL_STATSKLINE				= 216;
const INT RPL_STATSYLINE				= 218;
const INT RPL_ENDOFSTATS				= 219;
const INT RPL_STATSLLINE				= 241;
const INT RPL_STATSUPTIME				= 242;
const INT RPL_STATSOLINE				= 243;
const INT RPL_STATSHLINE				= 244;

const INT RPL_ADMINME					= 256;
const INT RPL_ADMINLOC1					= 257;
const INT RPL_ADMINLOC2					= 258;
const INT RPL_ADMINEMAIL				= 259;

const INT RPL_USERHOST					= 302;
const INT RPL_AWAY						= 301;
const INT RPL_UNAWAY					= 305;
const INT RPL_NOWAWAY					= 306;

const INT RPL_ISON						= 303;

const INT RPL_WHOISUSER					= 311;
const INT RPL_WHOISSERVER				= 312;
const INT RPL_WHOISOPERATOR				= 313;
const INT RPL_WHOISIDLE					= 317;
const INT RPL_ENDOFWHOIS				= 318;
const INT RPL_WHOISCHANNELS				= 319;
const INT RPL_WHOISIP					= 320;

const INT RPL_WHOWASUSER				= 314;
const INT RPL_ENDOFWHOWAS				= 369;

const INT RPL_LISTSTART					= 321;
const INT RPL_LIST						= 322;
const INT RPL_LISTEND					= 323;

const INT RPL_CHANNELMODEIS				= 324;

const INT RPL_NOTOPIC					= 331;
const INT RPL_TOPIC						= 332;

const INT RPL_INVITING					= 341;

const INT RPL_WHOREPLY					= 352;
const INT RPL_ENDOFWHO					= 315;

const INT RPL_VERSION					= 351;

const INT RPL_NAMEREPLY					= 353;
const INT RPL_ENDOFNAMES				= 366;

const INT RPL_LINKS						= 364;
const INT RPL_ENDOFLINKS				= 365;

const INT RPL_BANLIST					= 367;
const INT RPL_ENDOFBANLIST				= 368;

const INT RPL_INFO						= 371;
const INT RPL_ENDOFINFO					= 374;

const INT RPL_MOTDSTART					= 375;
const INT RPL_MOTD						= 372;
const INT RPL_MOTD2						= 377;
const INT RPL_ENDOFMOTD					= 376;

const INT RPL_YOUREOPER					= 381;
const INT RPL_YOUREADMIN				= 386;

const INT RPL_TIME						= 391;

const INT RPL_UMODEIS					= 221;

const INT RPL_LUSERCLIENT				= 251;
const INT RPL_LUSEROP					= 252;
const INT RPL_LUSERUNKNOWN				= 253;
const INT RPL_LUSERCHANNELS				= 254;
const INT RPL_LUSERME					= 255;

const INT RPL_LOCALUSERS				= 265;
const INT RPL_GLOBALUSERS				= 266;

const INT RPL_IRCEND					= 399;

// --- IRC Standard error codes (verbatim, ircsock.h:164-209) ----------------
const INT ERR_IRCSTART					= 401;

const INT ERR_NOSUCHSERVER				= 402;
const INT ERR_NEEDMOREPARAMS			= 461;
const INT ERR_NOTREGISTERED				= 451;
const INT ERR_ALREADYREGISTERED			= 462;
const INT ERR_TOOMANYTARGETS			= 407;
const INT ERR_NOORIGIN					= 409;
const INT ERR_UNKNOWNCOMMAND			= 421;
const INT ERR_NOMOTD					= 422;
const INT ERR_PASSWDMISMATCH			= 464;
const INT ERR_YOUREBANNEDCREEP			= 465;
const INT ERR_YOUWILLBEBANNED			= 466;
const INT ERR_NOSUCHNICK				= 401;
const INT ERR_NONICKNAMEGIVEN			= 431;
const INT ERR_ERRONEUSNICKNAME			= 432;
const INT ERR_NICKNAMEINUSE				= 433;
const INT ERR_NICKCOLLISION				= 436;
const INT ERR_NICKTOOFAST				= 438;
const INT ERR_NICKNOCHANGE				= 439;
const INT ERR_NOSUCHCHANNEL				= 403;
const INT ERR_TOOMANYCHANNELS			= 405;
const INT ERR_CHANNELISFULL				= 471;
const INT ERR_INVITEONLYCHAN			= 473;
const INT ERR_BANNEDFROMCHAN			= 474;
const INT ERR_BADCHANNELKEY				= 475;
const INT ERR_USERONCHANNEL				= 443;
const INT ERR_KEYSET					= 467;
const INT ERR_CANNOTSENDTOCHAN			= 404;
const INT ERR_NORECIPIENT				= 411;
const INT ERR_USERNOTINCHANNEL			= 441;
const INT ERR_NOTONCHANNEL				= 442;
const INT ERR_UNKNOWNMODE				= 472;
const INT ERR_NOPRIVILEGES				= 481;
const INT ERR_CHANOPRIVSNEEDED			= 482;
const INT ERR_CHANOWNPRIVNEEDED			= 485;

const INT ERR_UMODEUNKNOWNFLAG			= 501;
const INT ERR_USERSDONTMATCH			= 502;

const INT ERR_IRCEND					= 502;

// --- New IRCX replies (verbatim, ircsock.h:213-236) ------------------------
const INT RPL_IRCXSTART					= 800;

const INT RPL_IRCX						= 800;
const INT RPL_ACCESSADD					= 801;
const INT RPL_ACCESSDELETE				= 802;
const INT RPL_ACCESSSTART				= 803;
const INT RPL_ACCESSLIST				= 804;
const INT RPL_ACCESSEND					= 805;
const INT RPL_EVENTADD					= 806;
const INT RPL_EVENTDEL					= 807;
const INT RPL_EVENTSTART				= 808;
const INT RPL_EVENTLIST					= 809;
const INT RPL_EVENTEND					= 810;
const INT RPL_LISTXSTART				= 811;
const INT RPL_LISTXLIST					= 812;
const INT RPL_LISTXPICS					= 813;
const INT RPL_LISTXTRUNC				= 816;
const INT RPL_LISTXEND					= 817;

const INT RPL_PROPLIST					= 818;
const INT RPL_PROPEND					= 819;

const INT RPL_IRCXEND					= 899;

// --- New IRCX errors (verbatim, ircsock.h:240-285) -------------------------
const INT ERR_IRCXSTART1				= 503;

const INT ERR_NOJOINDYNAMIC				= 552;
const INT ERR_NODYNAMICCHANNELS			= 553;
const INT ERR_AUTHONLY					= 556;
const INT ERR_OVERFLOWABORT				= 557;

const INT ERR_IRCXEND1					= 557;

const INT ERR_IRCXSTART2				= 900;

const INT ERR_BADCOMMAND				= 900;
const INT ERR_TOOMANYARGUMENTS			= 901;
const INT ERR_BADFUNCTION				= 902;
const INT ERR_BADLEVEL					= 903;
const INT ERR_BADTAG					= 904;
const INT ERR_BADPROPERTY				= 905;
const INT ERR_BADVALUE					= 906;
const INT ERR_RESOURCE					= 907;
const INT ERR_SECURITY					= 908;
const INT ERR_ALREADYAUTHENTICATED		= 909;
const INT ERR_AUTHENTICATIONFAILED		= 910;
const INT ERR_AUTHENTICATIONSUSPENDED	= 911;
const INT ERR_UNKNOWNPACKAGE			= 912;
const INT ERR_NOACCESS					= 913;
const INT ERR_NOWHISPER					= 923;
const INT ERR_NOSUCHOBJECT				= 924;
const INT ERR_NOTSUPPORTED				= 925;
const INT ERR_CHANNELEXIST				= 926;

const INT ERR_INTERNALERROR				= 999;

// MIC 1.0 IRC2 Error codes (verbatim, ircsock.h:274-285)
const INT ERR_CANNOTJOINMICONLY			= 900;
const INT ERR_CANNOTJOINFROMREMOTE		= 901;
const INT ERR_CANNOTCREATEDYNAMIC		= 902;
const INT ERR_COMMANDNOTSUPPORTED		= 903;
const INT ERR_ONLYAUTHCANJOIN			= 904;
const INT ERR_CANNOTCHANGENICK			= 905;
const INT ERR_CANNOTMAKEHOST			= 906;
const INT ERR_CANNOTJOINDYNAMIC			= 907;
const INT ERR_UNKNOWNERROR				= 999;

const INT ERR_IRCXEND2					= 999;

// --- macros (verbatim, ircsock.h:290-291) ----------------------------------
#define bIsErrorCode(uCode)				((uCode >= ERR_IRCSTART && uCode <= ERR_IRCEND) || (uCode >= ERR_IRCXSTART1 && uCode <= ERR_IRCXEND1) || (uCode >= ERR_IRCXSTART2 && uCode <= ERR_IRCXEND2))
#define bIsReplyCode(uCode)				((uCode >= RPL_IRCSTART && uCode <= RPL_IRCEND) || (uCode >= RPL_IRCXSTART && uCode <= RPL_IRCXEND))

// --- structures (verbatim, ircsock.h:296-324) ------------------------------
typedef struct tagMODECACH
{
	CHAR	szChannelName[MAX_TOKEN];
	CHAR	szNickname[MAX_NICK];
	BYTE	byteStatus;
} MODECACH, *PMODECACH;

typedef struct tagPRIRCCMD
{
	CHAR	*szCmd;		// the command
	INT		cb;			// and its length
	UCHAR	uFlags;		// command flags: show Status Window | must be connected
	UCHAR	uMinArg;	// minimum number of arguments for this command
} PRIRCCMD, *PPRIRCCMD;

typedef struct tagPARSE
{
	BOOL	bHasPrefix;
	CHAR	nick[50];
	CHAR	user[50];
	CHAR	machine[50];
	UINT	uCode;
	SHORT	nArgs;
	CHAR	*args[MAXARGS];
	SHORT	nOffsets[MAXARGS];
	CHAR	*lastString;
} IRCPARSE, *PIRCPARSE;

// --- enumCmdId (verbatim, ircsock.h:331-381) -------------------------------
typedef enum
{
	cmdidAccess,
	cmdidAction,
	cmdidAuth,
	cmdidAway,
	cmdidClone,
	cmdidCreate,
	cmdidData,
	cmdidError,
	cmdidInfo,
	cmdidInvite,
	cmdidIsOn,
	cmdidJoin,
	cmdidKick,
	cmdidKill,
	cmdidKilled,
	cmdidKLine,
	cmdidKnock,
	cmdidList,
	cmdidListX,
	cmdidLUsers,
	cmdidMe,
	cmdidMode,
	cmdidMsg,
	cmdidNames,
	cmdidNick,
	cmdidNotice,
	cmdidPart,
	cmdidPass,
	cmdidPing,
	cmdidPong,
	cmdidPrivMsg,
	cmdidProp,
	cmdidQuit,
	cmdidQuote,
	cmdidRaw,
	cmdidReply,
	cmdidRequest,
	cmdidServer,
	cmdidSound,
	cmdidThink,
	cmdidTopic,
	cmdidUnKLine,
	cmdidUser,
	cmdidUserHost,
	cmdidWhisper,
	cmdidWho,
	cmdidWhoIs,
	cmdidMax	   // always one more - indicates how many cmds there are
} enumCmdId;

// --- pure-parse layer (verbatim, ircsock.cpp:36-400) -----------------------
extern PRIRCCMD	g_rgIrcCmd[];
extern void		ParseIt(const char *szMessage, PIRCPARSE pParse, BOOL bDoubleQuotes = FALSE);
extern void		FreeParse(PIRCPARSE pParse);
extern SHORT	NGetCmd(CHAR* szCmd);

// --- parse dispatch entry points (Task 5b) ---------------------------------
// Free functions over CCSession (fwd-declared; cc_session.h has the full def).
// cc_session_feed_bytes drives the OnReceive framer then ccProcessMessage per
// line; ccFireIsIrcXTimeout is the CC_TIMER_ISIRCX_PROBE handler.
struct CCSession;
void ccOnReceiveBytes(CCSession& sess, const uint8_t* data, size_t len);
void ccProcessMessage(CCSession& sess, char* szLine);
void ccFireIsIrcXTimeout(CCSession& sess);

#endif // __IRCSOCK_ENGINE_H__
