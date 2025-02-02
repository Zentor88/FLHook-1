/**
Connecticut Plugin by MadHunter
*/

// includes 

#include <windows.h>
#include <stdio.h>
#include <string>
#include <time.h>
#include <math.h>
#include <list>
#include <map>
#include <algorithm>
#include <FLHook.h>
#include <plugin.h>
#include <hookext_exports.h>
#include <PluginUtilities.h>
#include <unordered_set>
#include <math.h>

#define CLIENT_STATE_NONE		0
#define CLIENT_STATE_TRANSFER	1
#define CLIENT_STATE_RETURN		2

int transferFlags[MAX_CLIENT_ID + 1];

const std::wstring STR_INFO1 = L"Please dock at nearest base";
const std::wstring STR_INFO2 = L"Cargo hold is not empty";

int set_iPluginDebug = 0;
// Base to beam to.
uint set_iTargetBaseID = 0;

// Restricted systems, cannot jump out of here.
list<uint> set_lRestrictedSystemIDs;

// Target system, cannot jump out of here.
uint set_iTargetSystemID = 0;

// Base to use if player is trapped in the conn system.
uint set_iDefaultBaseID = 0;

unordered_set<uint> setForbiddenEquipment;

/// A return code to indicate to FLHook if we want the hook processing to continue.
PLUGIN_RETURNCODE returncode;

/// Clear client info when a client connects.
void ClearClientInfo(uint iClientID)
{
	transferFlags[iClientID] = CLIENT_STATE_NONE;
}

/// Load the configuration
void LoadSettings()
{
	returncode = DEFAULT_RETURNCODE;

	memset(transferFlags, 0, sizeof(int) * (MAX_CLIENT_ID + 1));

	// The path to the configuration file.
	char szCurDir[MAX_PATH];
	GetCurrentDirectory(sizeof(szCurDir), szCurDir);
	string scPluginCfgFile = string(szCurDir) + "\\flhook_plugins\\conn.cfg";

	// Load generic settings
	set_iPluginDebug = IniGetI(scPluginCfgFile, "General", "Debug", 0);
	set_iTargetBaseID = CreateID(IniGetS(scPluginCfgFile, "General", "TargetBase", "li06_05_base").c_str());
	set_iTargetSystemID = CreateID(IniGetS(scPluginCfgFile, "General", "TargetSystem", "li06").c_str());
	set_iDefaultBaseID = CreateID(IniGetS(scPluginCfgFile, "General", "DefaultBase", "li01_proxy_base").c_str());

	INI_Reader ini;
	set_lRestrictedSystemIDs.clear();
	if (ini.open(scPluginCfgFile.c_str(), false))
	{
		while (ini.read_header())
		{
			if (ini.is_header("General"))
			{
				while (ini.read_value())
				{
					if (ini.is_value("RestrictedSystem"))
					{
						string blockedSystem = ini.get_value_string();
						set_lRestrictedSystemIDs.push_back(CreateID(blockedSystem.c_str()));
						if (set_iPluginDebug)
							ConPrint(L"NOTICE: Adding conn restricted system %s\n", stows(blockedSystem).c_str());
					}
					else if (ini.is_value("ForbiddenEquipment"))
					{
						setForbiddenEquipment.insert(CreateID(ini.get_value_string(0)));
					}
				}
			}
		}
		ini.close();
	}
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	static bool patched = false;
	srand((uint)time(0));

	// If we're being loaded from the command line while FLHook is running then
	// set_scCfgFile will not be empty so load the settings as FLHook only
	// calls load settings on FLHook startup and .rehash.
	if (fdwReason == DLL_PROCESS_ATTACH)
	{
		if (set_scCfgFile.length() > 0)
			LoadSettings();
		HkLoadStringDLLs();
	}
	else if (fdwReason == DLL_PROCESS_DETACH)
	{
		HkUnloadStringDLLs();
	}
	return true;
}

bool IsDockedClient(unsigned int client)
{
	unsigned int base = 0;
	pub::Player::GetBase(client, base);
	if (base)
		return true;

	return false;
}

bool ValidateCargo(unsigned int client)
{
	std::wstring playerName = (const wchar_t*)Players.GetActiveCharacterName(client);
	std::list<CARGO_INFO> cargo;
	int holdSize = 0;

	HkEnumCargo(playerName, cargo, holdSize);

	for (std::list<CARGO_INFO>::const_iterator it = cargo.begin(); it != cargo.end(); ++it)
	{
		const CARGO_INFO & item = *it;

		bool flag = false;
		pub::IsCommodity(item.iArchID, flag);

		// Some commodity present.
		if (flag)
		{
			PrintUserCmdText(client, STR_INFO2);
			return false;
		}
		else if (setForbiddenEquipment.count(item.iArchID))
		{
			const GoodInfo* gi = GoodList::find_by_id(item.iArchID);
			PrintUserCmdText(client, L"Can't enter arena while holding %ls", HkGetWStringFromIDS(gi->iIDSName).c_str());
			return false;
		}
	}

	return true;
}

void StoreReturnPointForClient(unsigned int client)
{
	CUSTOM_BASE_IS_DOCKED_STRUCT info;
	info.iClientID = client;
	info.iDockedBaseID = 0;
	Plugin_Communication(CUSTOM_BASE_IS_DOCKED, &info);
	// It's not docked at a custom base, check for a regular base
	if (info.iDockedBaseID)
	{
		HookExt::IniSetI(client, "conn.retbase", info.iDockedBaseID);
		HookExt::IniSetI(client, "conn.retsystembackup", Players[client].iSystemID);
	}
	else
	{
		uint base;
		pub::Player::GetBase(client, base);
		HookExt::IniSetI(client, "conn.retbase", base);
	}
}

unsigned int ReadReturnPointForClient(unsigned int client)
{
	return HookExt::IniGetI(client, "conn.retbase");
}

void SimulateF1(uint client, uint baseId)
{
	Server.BaseEnter(baseId, client);
	Server.BaseExit(baseId, client);
	wstring wscCharFileName;
	HkGetCharFileName(ARG_CLIENTID(client), wscCharFileName);
	wscCharFileName += L".fl";
	CHARACTER_ID cID;
	strcpy(cID.szCharFilename, wstos(wscCharFileName.substr(0, 14)).c_str());
	Server.CharacterSelect(cID, client);
}

void MoveClient(unsigned int client, unsigned int targetBase)
{
	// Ask that another plugin handle the beam.
	CUSTOM_BASE_BEAM_STRUCT info;
	info.iClientID = client;
	info.iTargetBaseID = targetBase;
	info.bBeamed = false;
	Plugin_Communication(CUSTOM_BASE_BEAM, &info);
	if (info.bBeamed)
		return;

	uint system;
	pub::Player::GetSystem(client, system);

	// No plugin handled it, do it ourselves.
	Universe::IBase* base = Universe::get_base(targetBase);
	if (base)
	{
		pub::Player::ForceLand(client, targetBase); // beam	// if not in the same system, emulate F1 charload
		if (base->iSystemID != system)
		{
			SimulateF1(client, targetBase);
		}
	}
	else
	{
		//All else failed, move to proxy base
		uint returnSys = HookExt::IniGetI(client, "conn.retsystembackup");
		auto sysInfo = Universe::get_system(returnSys);
		string scProxyBase = (string)((const char*)sysInfo->nickname) + "_proxy_base";
		uint proxyBaseID;
		if (pub::GetBaseID(proxyBaseID, scProxyBase.c_str()) == -4)
		{
			PrintUserCmdText(client, L"Return impossible, contact staff");
			return;
		}
		PrintUserCmdText(client, L"Player base renamed/destroyed, ship redirected to a proxy base");
		pub::Player::ForceLand(client, proxyBaseID); // beam
		if (returnSys != system)
		{
			SimulateF1(client, proxyBaseID);
		}
	}

}

bool CheckReturnDock(unsigned int client, unsigned int target)
{
	unsigned int base = 0;
	pub::Player::GetBase(client, base);

	if (base == target)
		return true;

	return false;
}

bool UserCmd_Process(uint client, const wstring &cmd)
{
	returncode = DEFAULT_RETURNCODE;

	if (!cmd.compare(L"/conn"))
	{
		// Prohibit jump if in a restricted system or in the target system
		uint system = 0;
		pub::Player::GetSystem(client, system);
		if (find(set_lRestrictedSystemIDs.begin(), set_lRestrictedSystemIDs.end(), system) != set_lRestrictedSystemIDs.end()
			|| system == set_iTargetSystemID)
		{
			PrintUserCmdText(client, L"ERR Cannot use command in this system or base");
			return true;
		}

		if (!IsDockedClient(client))
		{
			PrintUserCmdText(client, STR_INFO1);
			return true;
		}

		if (!ValidateCargo(client))
		{
			return true;
		}

		StoreReturnPointForClient(client);
		PrintUserCmdText(client, L"Redirecting undock to Connecticut.");
		transferFlags[client] = CLIENT_STATE_TRANSFER;

		return true;
	}
	else if (!cmd.compare(L"/return"))
	{
		if (!ReadReturnPointForClient(client))
		{
			PrintUserCmdText(client, L"No return possible");
			return true;
		}

		if (!IsDockedClient(client))
		{
			PrintUserCmdText(client, STR_INFO1);
			return true;
		}

		if (!CheckReturnDock(client, set_iTargetBaseID))
		{
			PrintUserCmdText(client, L"Not in correct base");
			return true;
		}

		if (!ValidateCargo(client))
		{
			PrintUserCmdText(client, STR_INFO2);
			return true;
		}

		PrintUserCmdText(client, L"Redirecting undock to previous base");
		transferFlags[client] = CLIENT_STATE_RETURN;

		return true;
	}

	return false;
}

void __stdcall CharacterSelect(struct CHARACTER_ID const &charid, unsigned int client)
{
	returncode = DEFAULT_RETURNCODE;
	transferFlags[client] = CLIENT_STATE_NONE;
}

void __stdcall PlayerLaunch_AFTER(unsigned int ship, unsigned int client)
{
	returncode = DEFAULT_RETURNCODE;

	if (transferFlags[client] == CLIENT_STATE_TRANSFER)
	{
		if (!ValidateCargo(client))
		{
			PrintUserCmdText(client, STR_INFO2);
			return;
		}

		transferFlags[client] = CLIENT_STATE_NONE;
		MoveClient(client, set_iTargetBaseID);
		return;
	}

	if (transferFlags[client] == CLIENT_STATE_RETURN)
	{
		if (!ValidateCargo(client))
		{
			PrintUserCmdText(client, STR_INFO2);
			return;
		}

		transferFlags[client] = CLIENT_STATE_NONE;
		unsigned int returnPoint = ReadReturnPointForClient(client);

		if (!returnPoint)
			return;

		MoveClient(client, returnPoint);
		HookExt::IniSetI(client, "conn.retbase", 0);
		HookExt::IniSetI(client, "conn.retsystembackup", 0);
		return;
	}
}


/** Functions to hook */
EXPORT PLUGIN_INFO* Get_PluginInfo()
{
	PLUGIN_INFO* p_PI = new PLUGIN_INFO();
	p_PI->sName = "Conn Plugin by MadHunter";
	p_PI->sShortName = "conn";
	p_PI->bMayPause = true;
	p_PI->bMayUnload = true;
	p_PI->ePluginReturnCode = &returncode;
	p_PI->lstHooks.push_back(PLUGIN_HOOKINFO((FARPROC*)&LoadSettings, PLUGIN_LoadSettings, 0));
	p_PI->lstHooks.push_back(PLUGIN_HOOKINFO((FARPROC*)&ClearClientInfo, PLUGIN_ClearClientInfo, 0));
	p_PI->lstHooks.push_back(PLUGIN_HOOKINFO((FARPROC*)&CharacterSelect, PLUGIN_HkIServerImpl_CharacterSelect, 0));
	p_PI->lstHooks.push_back(PLUGIN_HOOKINFO((FARPROC*)&PlayerLaunch_AFTER, PLUGIN_HkIServerImpl_PlayerLaunch_AFTER, 0));
	p_PI->lstHooks.push_back(PLUGIN_HOOKINFO((FARPROC*)&UserCmd_Process, PLUGIN_UserCmd_Process, 0));
	return p_PI;
}