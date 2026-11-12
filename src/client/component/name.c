/* ---- player name ----
 * The `name` dvar defaults to dw_username (sub_4691C0). With no Demonware account name, dw_username keeps
 * "unknown_user" and sub_6CA5B0 replaces it with the first 9 characters of the Windows user name plus "Client"
 * (GetUserNameA in sub_643770), which is where "aClient" came from. The launcher passes +set dw_username; this
 * thread also sets the name dvar from r1-mod.ini ([r1-mod] name=, default player) once the lobby is up, through
 * cx_name so it runs on the game thread. */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"

/* cx_name <new name>   sets the player name dvar, which the connect userinfo base64-encodes */
static void __cdecl cmd_cx_name(void)
{
    unsigned char *d = *(unsigned char **)A(VA_NAME_DVAR);
    char name[64], cmd[160];
    const char *dvar_name;
    if (!d) {
        out("cx_name: name dvar not registered yet\n");
        return;
    }
    dvar_name = *(const char **)d;
    if (Cmd_Argc() < 2) {
        out("name = \"%s\" (dvar %s)\n", name_dvar_value(), dvar_name);
        return;
    }
    strncpy(name, Cmd_Argv(1), sizeof name - 1);
    name[sizeof name - 1] = 0;
    if (strlen(name) > 31) {
        name[31] = 0;
        out("cx_name: truncated to 31 bytes\n");
    }
    /* The console `set` path passes source 1/2 to Dvar_SetValueCore, which refuses dvars flagged
     * 0x2800 (this one is 0x2000). Source 0 is what engine code uses, so call it directly; the
     * command handler already runs on the game thread. */
    ((int (__cdecl *)(int, void *, float, float, float, int))A(0x632150u))((int)d, name, 0, 0, 0, 0);
    _snprintf(cmd, sizeof cmd - 1, "dvar %s now \"%s\"\n", dvar_name, name_dvar_value());
    cmd[sizeof cmd - 1] = 0;
    out("cx_name: %s", cmd);
}

void name_register(void)
{
    command_add("cx_name", cmd_cx_name);
}

static void config_name(char *name, size_t n)
{
    char ini[MAX_PATH];
    _snprintf(ini, sizeof ini, "%sr1-mod.ini", g_dir);
    GetPrivateProfileStringA("r1-mod", "name", "player", name, (DWORD)n, ini);
}

DWORD WINAPI name_thread(LPVOID unused)
{
    char name[64], cmd[128];
    int i;
    (void)unused;
    config_name(name, sizeof name);
    if (!*name)
        return 0;
    for (i = 0; i < 480 && (dw_live_state() != 2 || !*(unsigned char **)A(VA_NAME_DVAR)); i++)
        Sleep(250);
    Sleep(1000);
    if (strcmp(name_dvar_value(), name) != 0) {
        _snprintf(cmd, sizeof cmd - 1, "cx_name \"%s\"\n", name);
        cmd[sizeof cmd - 1] = 0;
        Cbuf_AddText(cmd);
    } else {
        out("r1-mod: player name \"%s\"\n", name);
    }
    return 0;
}
