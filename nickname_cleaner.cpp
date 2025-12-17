#include <stdio.h>
#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include "filesystem.h"
#include "utlbuffer.h"
#include "nickname_cleaner.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"

nickname_cleaner g_nickname_cleaner;
PLUGIN_EXPOSE(nickname_cleaner, g_nickname_cleaner);
IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars *gpGlobals = nullptr;

IUtilsApi* g_pUtils;
IPlayersApi* g_pPlayers = nullptr;

static const char* kWhitelistPath = "addons/configs/nickname_cleaner/whitelist.txt";
static const char* kBlocklistPath = "addons/configs/nickname_cleaner/blocklist.txt";
static std::string g_sDefaultName = "Player";

std::unordered_set<std::string> g_Whitelist;
std::unordered_set<std::string> g_Blocklist;

CGameEntitySystem* GameEntitySystem()
{
	return g_pUtils->GetCGameEntitySystem();
}

void StartupServer()
{
	g_pGameEntitySystem = GameEntitySystem();
	g_pEntitySystem = g_pUtils->GetCEntitySystem();
	gpGlobals = g_pUtils->GetCGlobalVars();
}

static std::string ToLower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

static std::string Trim(const std::string& text)
{
	const auto first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return "";
	const auto last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

static void LoadList(const char* path, std::unordered_set<std::string>& out, const char* label)
{
	out.clear();
	if (!g_pFullFileSystem)
	{
		if (g_pUtils) g_pUtils->ErrorLog("[nickname_cleaner] No filesystem, cannot load %s", path);
		return;
	}
	CUtlBuffer buf;
	if (!g_pFullFileSystem->ReadFile(path, "GAME", buf))
	{
		if (g_pUtils) g_pUtils->ErrorLog("[nickname_cleaner] Failed to open %s, list empty", path);
		return;
	}
	std::string content;
	content.assign(static_cast<const char*>(buf.Base()), buf.TellPut());
	std::istringstream iss(content);
	std::string line;
	while (std::getline(iss, line))
	{
		line = Trim(line);
		if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
			line = line.substr(3);
		if (line.empty())
			continue;
		if (line.rfind("#", 0) == 0 || line.rfind("//", 0) == 0)
			continue;
		line = ToLower(line);
		out.insert(line);
	}
}

static void LoadConfig()
{
	KeyValues* hKv = new KeyValues("NicknameCleaner");
	const char *pszPath = "addons/configs/nickname_cleaner/settings.ini";

	if (!hKv->LoadFromFile(g_pFullFileSystem, pszPath))
	{
		if (g_pUtils) g_pUtils->ErrorLog("[nickname_cleaner] Failed to load %s", pszPath);
		hKv->deleteThis();
		return;
	}

	g_sDefaultName = hKv->GetString("DefaultName", "Player");
	hKv->deleteThis();
}

static void LoadWhitelist()
{
	LoadList(kWhitelistPath, g_Whitelist, "Whitelist");
}

static void LoadBlocklist()
{
	LoadList(kBlocklistPath, g_Blocklist, "Blocklist");
}

static bool IsWhitelistedToken(const std::string& token)
{
	const std::string lower = ToLower(token);
	for (const auto& allowed : g_Whitelist)
	{
		if (lower.find(allowed) != std::string::npos)
			return true;
	}
	return false;
}

static bool IsBlockedToken(const std::string& token)
{
	const std::string lower = ToLower(token);
	for (const auto& blocked : g_Blocklist)
	{
		if (blocked.size() > 2 && blocked[0] == '*' && blocked[1] == '.')
		{
			std::string suffix = blocked.substr(1);
			if (lower.size() >= suffix.size() && 
				lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0)
			{
				return true;
			}
		}
		else
		{
			if (lower.find(blocked) != std::string::npos)
				return true;
		}
	}
	return false;
}

static std::string StripPunctuation(std::string token)
{
	token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char c) { return std::isspace(c); }), token.end());
	while (!token.empty() && std::ispunct(static_cast<unsigned char>(token.front())))
		token.erase(token.begin());
	while (!token.empty() && std::ispunct(static_cast<unsigned char>(token.back())))
		token.pop_back();
	return token;
}

static bool LooksLikeAddress(const std::string& token)
{
	const std::string lower = ToLower(token);
	if (lower.find("http://") != std::string::npos || lower.find("https://") != std::string::npos || lower.find("www.") != std::string::npos)
		return true;

	return false;
}

static std::string CleanNickname(const std::string& rawName)
{
	std::istringstream iss(rawName);
	std::vector<std::string> keptTokens;
	std::vector<std::string> blockedTokens;
	std::string token;
	while (iss >> token)
	{
		std::string simplified = StripPunctuation(token);
		if (simplified.empty())
			continue;

		if (IsWhitelistedToken(simplified))
		{
			keptTokens.push_back(simplified);
			continue;
		}

		if (IsBlockedToken(simplified))
		{
			blockedTokens.push_back(simplified);
			continue;
		}

		if (LooksLikeAddress(simplified))
			continue;

		keptTokens.push_back(simplified);
	}

	if (!blockedTokens.empty())
	{
		std::ostringstream bl;
		for (size_t i = 0; i < blockedTokens.size(); ++i)
		{
			if (i > 0) bl << ',';
			bl << blockedTokens[i];
		}
	}

	std::ostringstream result;
	for (size_t i = 0; i < keptTokens.size(); ++i)
	{
		if (i > 0) result << ' ';
		result << keptTokens[i];
	}

	const std::string cleaned = result.str();
	return cleaned.empty() ? g_sDefaultName : cleaned;
}

static bool ApplyCleanName(int iSlot, uint64 iSteamID64)
{
	if (!g_pPlayers)
		return false;
	if (iSlot < 0 || iSlot >= 64)
		return false;
	if (g_pPlayers->IsFakeClient(iSlot))
		return false;

	const uint64 currentSteamId = g_pPlayers->GetSteamID64(iSlot);
	if (currentSteamId != 0 && iSteamID64 != 0 && currentSteamId != iSteamID64)
		return false;

	const char* szName = g_pPlayers->GetPlayerName(iSlot);
	if (!szName || !szName[0])
		return false;

	const std::string cleaned = CleanNickname(szName);
	if (cleaned == szName)
		return false;
	g_pPlayers->SetPlayerName(iSlot, cleaned.c_str());
	return true;
}

static void ApplyCleanNameForAllPlayers()
{
	if (!g_pPlayers)
		return;
	for (int iSlot = 0; iSlot < 64; iSlot++)
	{
		ApplyCleanName(iSlot, 0);
	}
}

static void StartBurstEnforce(float intervalSeconds, int tries)
{
	if (!g_pUtils)
		return;
	int triesLeft = tries;
	g_pUtils->CreateTimer(intervalSeconds, [intervalSeconds, triesLeft]() mutable -> float
	{
		if (!g_pPlayers || !g_pUtils)
			return -1.0f;

		ApplyCleanNameForAllPlayers();

		if (--triesLeft <= 0)
			return -1.0f;

		return intervalSeconds;
	});
}

bool nickname_cleaner::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);

	g_SMAPI->AddListener( this, this );

	return true;
}

bool nickname_cleaner::Unload(char *error, size_t maxlen)
{
	ConVar_Unregister();
	
	return true;
}

void nickname_cleaner::AllPluginsLoaded()
{
	char error[64];
	int ret;
	g_pUtils = (IUtilsApi *)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_SMAPI->Format(error, sizeof(error), "Missing Utils system plugin");
		ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}

	g_pPlayers = (IPlayersApi *)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_SMAPI->Format(error, sizeof(error), "Missing Players system plugin");
		ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}

	LoadConfig();
	LoadWhitelist();
	LoadBlocklist();
	g_pUtils->StartupServer(g_PLID, StartupServer);

	g_pUtils->HookEvent(g_PLID, "player_changename", [](const char* /*szEventName*/, IGameEvent* pEvent, bool /*bDontBroadcast*/)
	{
		if (!g_pPlayers || !pEvent)
			return;

		StartBurstEnforce(0.25f, 12);
	});

	auto roundEnforce = [](const char* /*szEventName*/, IGameEvent* pEvent, bool /*bDontBroadcast*/)
	{
		if (!g_pPlayers || !pEvent)
			return;

		StartBurstEnforce(0.5f, 12);
	};
	g_pUtils->HookEvent(g_PLID, "round_start", roundEnforce);
	g_pUtils->HookEvent(g_PLID, "round_prestart", roundEnforce);
	g_pUtils->HookEvent(g_PLID, "round_freeze_end", roundEnforce);
	g_pUtils->HookEvent(g_PLID, "round_end", roundEnforce);
	g_pUtils->HookEvent(g_PLID, "round_poststart", roundEnforce);

	g_pUtils->CreateTimer(5.0f, []() -> float
	{
		if (!g_pPlayers || !g_pUtils)
			return -1.0f;
		ApplyCleanNameForAllPlayers();
		return 5.0f;
	});

	g_pPlayers->HookOnClientAuthorized(g_PLID, [](int iSlot, uint64 iSteamID64)
	{
		if (!g_pPlayers || !g_pUtils)
			return;

		ApplyCleanName(iSlot, iSteamID64);
		int triesLeft = 20;
		g_pUtils->CreateTimer(0.25f, [iSlot, iSteamID64, triesLeft]() mutable -> float
		{
			if (!g_pPlayers || !g_pUtils)
				return -1.0f;

			if (ApplyCleanName(iSlot, iSteamID64))
				return -1.0f;

			if (--triesLeft <= 0)
				return -1.0f;

			return 0.25f;
		});
	});
}

const char* nickname_cleaner::GetLicense()
{
	return "GPL";
}

const char* nickname_cleaner::GetVersion()
{
	return "1.0.2";
}

const char* nickname_cleaner::GetDate()
{
	return __DATE__;
}

const char *nickname_cleaner::GetLogTag()
{
	return "[nickname_cleaner]";
}

const char* nickname_cleaner::GetAuthor()
{
	return "ABKAM";
}

const char* nickname_cleaner::GetDescription()
{
	return "Nickname Cleaner";
}

const char* nickname_cleaner::GetName()
{
	return "Nickname Cleaner";
}

const char* nickname_cleaner::GetURL()
{
	return "https://discord.gg/ChYfTtrtmS";
}
