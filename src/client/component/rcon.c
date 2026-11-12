/* ---- remote console ----
 * The QA client has no rcon command (only map_restart and devmap, which start a local map). cx_rcon sends
 * "rcon <password> <command>" out of band to the server in [r1-mod] rcon_address (default 127.0.0.1:28960) with
 * rcon_password (default r1modlocal, what tools\dev\run_native.ps1 sets) and prints the server's replies. The reply
 * wait runs on its own thread so the game never blocks on it. */
#include "std_include.h"
#include <ctype.h>
#include "game/game.h"
#include "component/components.h"

/* Sends one rcon command and waits for the reply (up to 3 s, blocking: call it off the game thread). The reply text,
 * without the "print\n" prefixes, goes to reply when it is given, else to the console. Returns 1 once the server
 * answered, 0 otherwise (the reason is printed). */
int rcon_exec(const char *line, char *reply, size_t reply_size)
{
    char ini[MAX_PATH], target[128], password[64], buf[4096];
    struct sockaddr_in to;
    unsigned ip;
    unsigned short port;
    SOCKET s;
    DWORD timeout = 250;
    ULONGLONG until;
    size_t used = 0;
    int sent, got = 0;
    if (reply && reply_size)
        reply[0] = 0;
    _snprintf(ini, sizeof ini, "%sr1-mod.ini", g_dir);
    GetPrivateProfileStringA("r1-mod", "rcon_address", "127.0.0.1:28960", target, sizeof target, ini);
    GetPrivateProfileStringA("r1-mod", "rcon_password", "r1modlocal", password, sizeof password, ini);
    if (!parse_target(target, &ip, &port)) {
        out("cx_rcon: bad rcon_address \"%s\" in r1-mod.ini\n", target);
        return 0;
    }
    if (!strchr(target, ':'))
        port = htons(28960);
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        out("cx_rcon: no UDP socket\n");
        return 0;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = ip;
    to.sin_port = port;
    _snprintf(buf, sizeof buf - 1, "\xff\xff\xff\xffrcon %s %s\n", password, line);
    buf[sizeof buf - 1] = 0;
    sent = sendto(s, buf, (int)strlen(buf), 0, (struct sockaddr *)&to, sizeof to);
    if (sent <= 0) {
        out("cx_rcon: send to %s failed (%d)\n", target, WSAGetLastError());
    } else {
        /* the server answers with one or more "print\n<text>" datagrams; stop once it has gone quiet */
        until = GetTickCount64() + 3000;
        while (GetTickCount64() < until) {
            int n = recvfrom(s, buf, sizeof buf - 1, 0, NULL, NULL);
            if (n <= 0) {
                if (got)
                    break;
                continue;
            }
            buf[n] = 0;
            if (n > 4 && !memcmp(buf, "\xff\xff\xff\xff", 4)) {
                const char *text = buf + 4;
                if (!strncmp(text, "print\n", 6))
                    text += 6;
                if (reply && reply_size) {
                    _snprintf(reply + used, reply_size - used - 1, "%s", text);
                    reply[reply_size - 1] = 0;
                    used = strlen(reply);
                } else if (*text) {
                    out("%s%s", text, text[strlen(text) - 1] == '\n' ? "" : "\n");
                }
                got = 1;
                until = GetTickCount64() + 1000;
            }
        }
        if (!got)
            out("cx_rcon: no reply from %s (is the server running, is rcon_password right?)\n", target);
    }
    closesocket(s);
    return got;
}

static DWORD WINAPI rcon_thread(LPVOID param)
{
    rcon_exec((char *)param, NULL, 0);
    free(param);
    return 0;
}

/* cx_rcon <command...>   e.g. cx_rcon map mp_seatown */
static void __cdecl cmd_cx_rcon(void)
{
    char line[1024];
    size_t len = 0;
    int i;
    HANDLE t;
    if (Cmd_Argc() < 2) {
        out("usage: cx_rcon <command>   e.g. cx_rcon map mp_seatown (server from [r1-mod] rcon_address)\n");
        return;
    }
    line[0] = 0;
    for (i = 1; i < Cmd_Argc() && len < sizeof line - 2; i++) {
        _snprintf(line + len, sizeof line - len - 1, "%s%s", i > 1 ? " " : "", Cmd_Argv(i));
        line[sizeof line - 1] = 0;
        len = strlen(line);
    }
    t = CreateThread(NULL, 0, rcon_thread, _strdup(line), 0, NULL);
    if (t)
        CloseHandle(t);
}

/* ---- host shortcuts over rcon ----
 * For the player who holds the server's rcon password (the match host): the server-side commands r1-server added for
 * hosts, typed without the slot or the rcon prefix.
 *   cx_devmap <map> [gametype]   load a map with cheats on (the server's devmap); a game type is set first
 *   cx_god | cx_noclip | cx_ufo [slot]   toggle it for a player, by default yourself (server: god/noclip/ufo <slot>)
 *   cx_endgame                   end the match now (server: endgame)
 * The local client number is cg+13656 (cg = 0x1A19510[*0x7817BA8] ^ 0xFBB2C97A), valid in a match (state 10). */
static void rcon_async(const char *line)
{
    HANDLE t = CreateThread(NULL, 0, rcon_thread, _strdup(line), 0, NULL);
    if (t)
        CloseHandle(t);
}

static int own_slot(void)
{
    __try {
        unsigned key = *(volatile unsigned *)A(0x7817BA8u);
        unsigned cg = ((volatile unsigned *)A(0x1A19510u))[key] ^ 0xFBB2C97Au;
        int slot = *(volatile int *)(cg + 13656);
        if (*(volatile int *)A(0x1A2E7ECu) == 10 && slot >= 0 && slot < 64)
            return slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

static void host_toggle(const char *what)
{
    char line[64];
    int slot = Cmd_Argc() >= 2 ? atoi(Cmd_Argv(1)) : own_slot();
    if (slot < 0) {
        out("cx_%s: not in a match (or give a player slot: cx_%s <slot>)\n", what, what);
        return;
    }
    _snprintf(line, sizeof line - 1, "%s %d", what, slot);
    line[sizeof line - 1] = 0;
    rcon_async(line);
}

static void __cdecl cmd_cx_god(void) { host_toggle("god"); }
static void __cdecl cmd_cx_noclip(void) { host_toggle("noclip"); }
static void __cdecl cmd_cx_ufo(void) { host_toggle("ufo"); }

static void __cdecl cmd_cx_endgame(void)
{
    rcon_async("endgame");
}

static int plain_name(const char *s)
{
    if (!*s || strlen(s) > 48)
        return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_')
            return 0;
    return 1;
}

static DWORD WINAPI devmap_thread(LPVOID param)
{
    char *args = (char *)param, *type = strchr(args, ' '), line[96];
    if (type) {
        *type++ = 0;
        _snprintf(line, sizeof line - 1, "set g_gametype %s", type);
        line[sizeof line - 1] = 0;
        if (!rcon_exec(line, NULL, 0)) {
            free(param);
            return 0;
        }
    }
    _snprintf(line, sizeof line - 1, "devmap %s", args);
    line[sizeof line - 1] = 0;
    rcon_exec(line, NULL, 0);
    free(param);
    return 0;
}

static void __cdecl cmd_cx_devmap(void)
{
    char args[112];
    HANDLE t;
    if (Cmd_Argc() < 2 || !plain_name(Cmd_Argv(1)) || (Cmd_Argc() >= 3 && !plain_name(Cmd_Argv(2)))) {
        out("usage: cx_devmap <map> [gametype]   e.g. cx_devmap mp_rust_long dm (cheats on; needs the rcon password)\n");
        return;
    }
    _snprintf(args, sizeof args - 1, Cmd_Argc() >= 3 ? "%s %s" : "%s", Cmd_Argv(1), Cmd_Argc() >= 3 ? Cmd_Argv(2) : "");
    args[sizeof args - 1] = 0;
    t = CreateThread(NULL, 0, devmap_thread, _strdup(args), 0, NULL);
    if (t)
        CloseHandle(t);
}

void rcon_register(void)
{
    command_add("cx_rcon", cmd_cx_rcon);
    command_add("cx_devmap", cmd_cx_devmap);
    command_add("cx_god", cmd_cx_god);
    command_add("cx_noclip", cmd_cx_noclip);
    command_add("cx_ufo", cmd_cx_ufo);
    command_add("cx_endgame", cmd_cx_endgame);
}
