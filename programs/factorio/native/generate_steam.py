"""Generates the C header through which the kpp game calls the Steam API: native/include/kpp_steam.h.

The Steam SDK (libraries/steam/steam, v1.65) is C++: interfaces with virtual methods, callbacks through
templates and registration macros (STEAM_CALLBACK, CCallResult). kpp calls C only (docs/design.md,
"Interop: C only"), and Valve's own answer for other languages is its flat API, which steam_api.dll and
libsteam_api.so export as C functions: an accessor per interface (SteamAPI_SteamUser_v023) and a function
per method taking the interface (SteamAPI_ISteamUser_GetSteamID), CSteamID as a uint64 (uint64_steamid),
SteamAPI_InitFlat, and manual callback dispatch (SteamAPI_ManualDispatch_*: the callbacks and call results
taken one at a time, as data). steam_api_flat.h declares them, but as C++. This script reads the SDK's
steam_api.json, the description of all of it, and writes a C header with the parts the game uses (USED
below; a method the game calls that is not listed is a compile error in kpp, never a silent stub): the
typedefs, the enums, the interfaces as incomplete structs, the functions, and the callback structs with the
SDK's packing (8 on Windows, 4 on Linux, macOS and FreeBSD: steamclientpublic.h's VALVE_CALLBACK_PACK_*)
and their k_iCallback numbers as constants (<STRUCT>_k_iCallback).

    python generate_steam.py STEAM_SDK_DIR OUT_HEADER
"""
import json
import os
import sys

# The interfaces and the methods the game calls (src/SteamIntegration, AppManagerStates, PlayerData).
USED = {
    'ISteamApps': ['GetCurrentBetaName', 'GetCurrentGameLanguage'],
    'ISteamFriends': ['ClearRichPresence', 'GetFriendByIndex', 'GetFriendCount', 'GetFriendRichPresence', 'RequestFriendRichPresence', 'SetRichPresence'],
    'ISteamMatchmaking': ['CreateLobby', 'GetLobbyGameServer', 'JoinLobby', 'LeaveLobby', 'SetLobbyGameServer'],
    'ISteamNetworking': ['AcceptP2PSessionWithUser', 'IsP2PPacketAvailable', 'ReadP2PPacket', 'SendP2PPacket'],
    'ISteamRemoteStorage': ['FileDelete', 'FileExists', 'FileRead', 'FileWrite', 'GetFileSize', 'GetFileTimestamp', 'GetQuota',
                            'IsCloudEnabledForAccount', 'IsCloudEnabledForApp'],
    'ISteamUser': ['CancelAuthTicket', 'GetAuthSessionTicket', 'GetAuthTicketForWebApi', 'GetSteamID'],
    'ISteamUserStats': ['GetAchievement', 'GetStat', 'ResetAllStats', 'SetAchievement', 'SetStat', 'StoreStats'],
    'ISteamUtils': ['DismissFloatingGamepadTextInput', 'IsRunningOnSteamHardware', 'ShowFloatingGamepadTextInput'],
}
CALLBACKS = ['FloatingGamepadTextInputDismissed_t', 'GameLobbyJoinRequested_t', 'GameOverlayActivated_t', 'GameRichPresenceJoinRequested_t',
             'GetAuthSessionTicketResponse_t', 'GetTicketForWebApiResponse_t', 'LobbyCreated_t', 'LobbyEnter_t', 'SteamAPICallCompleted_t',
             'SteamServersConnected_t', 'SteamServersDisconnected_t', 'UserStatsReceived_t', 'P2PSessionRequest_t']
ENUMS = ['EResult', 'ESteamAPIInitResult', 'ELobbyType', 'EP2PSend', 'EFloatingGamepadTextInputMode', 'ESteamHardwareType', 'EFriendFlags']
CONSTS = ['k_HAuthTicketInvalid']

# The SDK's integer names (steamtypes.h) as C's.
INTEGERS = {'uint8': 'uint8_t', 'int8': 'int8_t', 'uint16': 'uint16_t', 'int16': 'int16_t', 'uint32': 'uint32_t', 'int32': 'int32_t',
            'uint64': 'uint64_t', 'int64': 'int64_t', 'unsigned int': 'uint32_t', 'int': 'int32_t', 'unsigned long long': 'uint64_t',
            'long long': 'int64_t', 'unsigned char': 'uint8_t', 'unsigned short': 'uint16_t', 'short': 'int16_t'}
# Types the flat API passes as these (CSteamID and CGameID as integers; SteamNetworkingIdentity by pointer, never looked into).
FLAT = {'CSteamID': 'uint64_steamid', 'CGameID': 'uint64_gameid', 'class CSteamID': 'uint64_steamid'}


def main():
    sdk, out = sys.argv[1], sys.argv[2]
    with open(os.path.join(sdk, 'steam_api.json'), encoding='utf-8') as f:
        api = json.load(f)
    typedefs = {t['typedef']: t['type'] for t in api['typedefs']}
    enums = {e['enumname']: e for e in api['enums']}
    for interface in api['interfaces']:
        for e in interface.get('enums', []):
            enums.setdefault(e['enumname'], e)
    consts = {c['constname']: c for c in api['consts']}
    callbacks = {c['struct']: c for c in api['callback_structs']}
    interfaces = {i['classname']: i for i in api['interfaces']}
    used_typedefs = set()
    lines = []

    def ctype(t):
        t = t.strip()
        t = FLAT.get(t, t)
        const = t.startswith('const ')
        base = t[6:] if const else t
        stars = ''
        while base.endswith('*') or base.endswith('&'):
            stars = ('*' if base.endswith('*') else '*') + stars
            base = base[:-1].strip()
        # (CSteamID by pointer too: the class is its 64-bit id alone, steamclientpublic.h's union)
        base = FLAT.get(base, base)
        if base in INTEGERS:
            base = INTEGERS[base]
        elif base in typedefs:
            used_typedefs.add(base)
        elif base in ('bool', 'char', 'void', 'float', 'double'):
            pass
        elif base in enums:
            pass
        elif base in ('uint64_steamid', 'uint64_gameid'):
            pass
        elif base.startswith('struct ') or base[0].isupper():
            base = base.replace('struct ', '')
            opaque.add(base)
        else:
            raise SystemExit(f'generate_steam: the type {t} is not one this script knows')
        return ('const ' if const else '') + base + (' ' + stars if stars else '')

    opaque = set()
    functions = []
    for name, methods in USED.items():
        interface = interfaces[name]
        accessor = [a for a in interface['accessors'] if a['kind'] == 'user']
        if not accessor:
            raise SystemExit(f'generate_steam: {name} has no user accessor')
        functions.append(f'KPP_STEAM_API {name}* {accessor[0]["name_flat"]}(void);')
        for method in methods:
            # (each overload: GetStat's int32 and float ones are GetStatInt32 and GetStatFloat in the flat API)
            overloads = [m for m in interface['methods'] if m['methodname'] == method]
            if not overloads:
                raise SystemExit(f'generate_steam: {name} has no method {method} in this SDK')
            for m in overloads:
              params = [f'{name}* self'] + [f'{ctype(p.get("paramtype_flat", p["paramtype"]))} {p["paramname"]}' for p in m['params']]
              functions.append(f'KPP_STEAM_API {ctype(m.get("returntype_flat", m["returntype"]))} {m["methodname_flat"]}({", ".join(params)});')
        opaque.add(name)

    structs = []
    for name in CALLBACKS:
        c = callbacks[name]
        fields = []
        for field in c['fields']:
            t = field['fieldtype']
            array = ''
            if '[' in t:
                t, array = t[:t.index('[')].strip(), t[t.index('['):]
            # (a CSteamID or CGameID field: the class is packed to 1 in the SDK (steamclientpublic.h's
            # #pragma pack( push, 1 )), so it sits unaligned where a uint64 would not: KppSteamID is that)
            if t.strip() in ('CSteamID', 'class CSteamID', 'CGameID', 'class CGameID'):
                fields.append(f'  KppSteamID {field["fieldname"]}{array};')
            else:
                fields.append(f'  {ctype(t)} {field["fieldname"]}{array};')
        structs.append(f'typedef struct {name}\n{{\n' + ('\n'.join(fields) if fields else '  uint8_t m_unused; /* (empty in C++: one byte there too) */') +
                       f'\n}} {name};\nenum {{ {name}_k_iCallback = {c["callback_id"]} }};')

    lines += ['/* Generated by kpp\'s programs/factorio/native/generate_steam.py from the Steam SDK\'s steam_api.json',
              f'   ({open(os.path.join(sdk, "..", "version.txt")).read().strip() if os.path.exists(os.path.join(sdk, "..", "version.txt")) else "the SDK"}): the parts of the',
              '   flat API the game uses, as C. Do not edit: run the script. */',
              '#pragma once', '#include <stdbool.h>', '#include <stdint.h>', '',
              '#ifdef _WIN32', '#define KPP_STEAM_API __declspec(dllimport)', '#else', '#define KPP_STEAM_API', '#endif', '']
    lines += ['typedef uint64_t uint64_steamid;', 'typedef uint64_t uint64_gameid;']
    for t in sorted(used_typedefs | {'HSteamPipe', 'HSteamUser', 'SteamAPICall_t', 'HAuthTicket', 'AppId_t'}):
        target = typedefs[t]
        if '[' in target:
            lines.append(f'typedef {INTEGERS.get(target[:target.index("[")].strip(), target[:target.index("[")].strip())} {t}{target[target.index("["):]};')
        else:
            lines.append(f'typedef {INTEGERS.get(target, target)} {t};')
    lines.append('typedef char SteamErrMsg[1024];')
    lines.append('')
    for name in ENUMS:
        e = enums[name]
        lines.append(f'typedef enum {name}\n{{')
        lines += [f'  {v["name"]} = {v["value"]},' for v in e['values']]
        lines.append(f'}} {name};')
    lines.append('')
    for name in CONSTS:
        c = consts[name]
        lines.append(f'#define {name} (({INTEGERS.get(c["consttype"], c["consttype"])})({c["constval"]}))')
    # (a callback struct's own constant: isteamuser.h's GetTicketForWebApiResponse_t::k_nCubTicketMaxLength, its ticket's size)
    lines.append('#define GetTicketForWebApiResponse_t_k_nCubTicketMaxLength 2560')
    lines.append('')
    for name in sorted(opaque - set(enums)):
        lines.append(f'typedef struct {name} {name};')
    lines += ['', '/* A CSteamID or CGameID inside a callback struct: its 64-bit value, aligned to 1 as the class of the SDK is */',
              '#pragma pack(push, 1)', 'typedef struct KppSteamID\n{\n  uint64_t m_value;\n} KppSteamID;', '#pragma pack(pop)']
    lines += ['', '#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)', '#pragma pack(push, 4)', '#else', '#pragma pack(push, 8)', '#endif']
    lines += ['/* steam_api_internal.h: a callback taken by manual dispatch */',
              'typedef struct CallbackMsg_t\n{\n  HSteamUser m_hSteamUser;\n  int32_t m_iCallback;\n  uint8_t* m_pubParam;\n  int32_t m_cubParam;\n} CallbackMsg_t;']
    lines += structs
    lines += ['#pragma pack(pop)', '']
    lines += ['/* steam_api.h: the API\'s lifetime and manual dispatch */',
              'KPP_STEAM_API ESteamAPIInitResult SteamAPI_InitFlat(SteamErrMsg* pOutErrMsg);',
              'KPP_STEAM_API void SteamAPI_Shutdown(void);',
              'KPP_STEAM_API bool SteamAPI_RestartAppIfNecessary(uint32_t unOwnAppID);',
              'KPP_STEAM_API HSteamPipe SteamAPI_GetHSteamPipe(void);',
              'KPP_STEAM_API void SteamAPI_ManualDispatch_Init(void);',
              'KPP_STEAM_API void SteamAPI_ManualDispatch_RunFrame(HSteamPipe hSteamPipe);',
              'KPP_STEAM_API bool SteamAPI_ManualDispatch_GetNextCallback(HSteamPipe hSteamPipe, CallbackMsg_t* pCallbackMsg);',
              'KPP_STEAM_API void SteamAPI_ManualDispatch_FreeLastCallback(HSteamPipe hSteamPipe);',
              'KPP_STEAM_API bool SteamAPI_ManualDispatch_GetAPICallResult(HSteamPipe hSteamPipe, SteamAPICall_t hSteamAPICall, void* pCallback, int32_t cubCallback, int32_t iCallbackExpected, bool* pbFailed);',
              '']
    lines += ['/* The interfaces\' accessors and the methods the game calls */'] + functions + ['']
    with open(out, 'w', newline='\n') as f:
        f.write('\n'.join(lines))


main()
