/* ---- in-game chat goes to the game server ----
 * QA has two chat inputs and both end in the lobby's room object (dwRoomImp, dword 0x78B0708, vftable 0xC7EBA4):
 *   - the engine's message mode (chatmodepublic / chatmodeteam; key handler 0x462150): on Enter it asks the room
 *     whether its chat channel is up (vfunc +80, 0x6E4EA0) and then calls the room's send (vfunc +64 all, +68 team);
 *     only when the channel is down does it send  say "<text>"  to the game server (0x463A20, the reliable command)
 *   - the Scaleform chat box (AS SendChatMessage, 0x5E4760): channel 3 "Room Chat" and 5 "Room Team Chat" call the
 *     same two sends
 * The sends (0x6E4D80, 0x6E4DE0) post the line to the Demonware chat channel "chat.room" / "chat.roomteam". With
 * r1-mod that lobby is each player's own local backend, so the line reached nobody: no other player, and not the
 * game server (reported 2026-11-10; `say` typed in the console did work).
 *
 * The two vftable slots are replaced. In a match (connection state 10) the text goes to the game server as the
 * engine's own  say "<text>"  /  say_team "<text>"  reliable command, which r1-server broadcasts under the
 * player's name; outside a match the original send runs, so the room and lobby are unchanged. [r1-mod]
 * chat_to_server=0 in r1-mod.ini leaves the game as it was. */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"

#define VA_ROOM_VFTABLE   0xC7EBA4u
#define SLOT_SEND_ROOM    16        /* +64: 0x6E4D80 */
#define SLOT_SEND_TEAM    17        /* +68: 0x6E4DE0 */
#define VA_SEND_ROOM      0x6E4D80u
#define VA_SEND_TEAM      0x6E4DE0u
#define VA_RELIABLE_CMD   0x463A20u /* cdecl (const char *command) */
#define VA_CONNSTATE      0x1A2E7ECu

/* thiscall (this in ecx, the text on the stack, callee pops) written as fastcall with an unused edx */
typedef int(__fastcall *room_send_t)(void *self, void *edx, const char *text);

static room_send_t g_orig_room, g_orig_team;
static volatile LONG g_sent;
static int g_installed;

static int in_match(void)
{
    __try {
        return *(volatile int *)A(VA_CONNSTATE) == 10;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* say "<text>": the text without quotes, control characters or a trailing backslash, at most 150 bytes as the
 * engine's own input allows. Returns 0 for an empty line. */
static int to_server(const char *text, int team)
{
    char line[224];
    size_t n = 0, i;
    if (!text)
        return 0;
    n = (size_t)_snprintf(line, sizeof line - 1, team ? "say_team \"" : "say \"");
    for (i = 0; text[i] && i < 150 && n < sizeof line - 3; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c == 0x7F)
            continue;
        line[n++] = c == '"' ? '\'' : (char)c;
    }
    while (n > 0 && (line[n - 1] == '\\' || line[n - 1] == ' '))
        n--;
    if (line[n - 1] == '"') /* nothing but the opening quote is left */
        return 0;
    line[n++] = '"';
    line[n] = 0;
    __try {
        ((void(__cdecl *)(const char *))A(VA_RELIABLE_CMD))(line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    InterlockedIncrement(&g_sent);
    return 1;
}

static int __fastcall room_send_hook(void *self, void *edx, const char *text)
{
    if (in_match()) {
        to_server(text, 0);
        return 0;
    }
    return g_orig_room(self, edx, text);
}

static int __fastcall team_send_hook(void *self, void *edx, const char *text)
{
    if (in_match()) {
        to_server(text, 1);
        return 0;
    }
    return g_orig_team(self, edx, text);
}

static int set_slot(int slot, void *fn, uintptr_t expect, room_send_t *orig)
{
    void **entry = (void **)A(VA_ROOM_VFTABLE) + slot;
    DWORD old;
    if ((uintptr_t)*entry != A(expect))
        return 0; /* not the build this was written for */
    *orig = (room_send_t)*entry;
    if (!VirtualProtect(entry, sizeof *entry, PAGE_READWRITE, &old))
        return 0;
    *entry = fn;
    VirtualProtect(entry, sizeof *entry, old, &old);
    return 1;
}

/* cx_chat               say whether chat goes to the game server and how many lines were sent
 * cx_chat test <text>   send <text> through the room's chat function (the path the chat box takes) */
static void __cdecl cmd_cx_chat(void)
{
    /* cx_chat test <text>: call the room's send through its vftable, exactly as the chat box and message mode do,
     * to check the reroute without typing in the box */
    if (Cmd_Argc() >= 3 && !strcmp(Cmd_Argv(1), "test")) {
        if (g_installed <= 0 || !in_match()) {
            /* outside a match the room's own send would run, and it faults without a joined room */
            out("cx_chat test: only in a match with the reroute on\n");
            return;
        }
        __try {
            void **room = *(void ***)A(0x78B0708u);
            if (!room) {
                out("cx_chat test: the room object does not exist yet\n");
                return;
            }
            ((room_send_t)(*(void ***)room)[SLOT_SEND_ROOM])(room, NULL, Cmd_Argv(2));
            out("cx_chat test: sent through the room's chat function (%s; %ld to the server so far)\n",
                in_match() ? "in a match" : "not in a match: the lobby channel got it", g_sent);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out("cx_chat test: the call failed\n");
        }
        return;
    }
    if (g_installed > 0)
        out("chat: in a match the chat box and message mode send say / say_team to the game server (%ld sent)\n", g_sent);
    else if (g_installed < 0)
        out("chat: not installed (the room's chat functions were not recognised)\n");
    else
        out("chat: off (chat_to_server=0 in r1-mod.ini): the game sends chat to the lobby channel\n");
}

void chat_register(void)
{
    char ini[MAX_PATH];
    command_add("cx_chat", cmd_cx_chat);
    _snprintf(ini, sizeof ini, "%sr1-mod.ini", g_dir);
    ini[sizeof ini - 1] = 0;
    if (!GetPrivateProfileIntA("r1-mod", "chat_to_server", 1, ini))
        return;
    __try {
        if (set_slot(SLOT_SEND_ROOM, room_send_hook, VA_SEND_ROOM, &g_orig_room) &&
            set_slot(SLOT_SEND_TEAM, team_send_hook, VA_SEND_TEAM, &g_orig_team))
            g_installed = 1;
        else
            g_installed = -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_installed = -1;
    }
    if (g_installed > 0)
        out("r1-mod: chat box sends to the game server in a match (cx_chat)\n");
    else
        out("r1-mod: chat box not rerouted (room chat functions not recognised)\n");
}
