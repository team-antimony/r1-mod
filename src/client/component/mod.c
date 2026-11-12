/* ---- GSC mods on the local server ----
 * GSC runs only on the native server, which compiles every script from the first of its source roots that has the
 * file: codol_scriptNativeOverlay, then codol_scriptOverlay, then codol_scriptPath (codol_script_source.c). A mod is
 * a folder of GSC under [r1-mod] mods_dir (default <dll dir>\mods), laid out like the script set it replaces
 * (mods\hns\maps\mp\gametypes\war.gsc replaces Team Deathmatch). cx_mod points the native overlay at that folder over
 * rcon and runs map_restart, which reloads the level (SV_LoadLevel) and so recompiles every script. The QA client
 * does not follow that reload: the server sends it the new gamestate and it drops back to the lobby without a word
 * (seen 2026-11-03). So when the client is in a match, cx_mod disconnects it first and rejoins with cx_match once
 * the server has a new sv_serverid. The overlay the server had before the first switch is kept in r1-mod.ini
 * (mod_base_overlay) for cx_mod off. Optional mods\<name>\mod.cfg: server commands sent before the restart;
 * mods\<name>\client_off.cfg: client console lines run when that mod is unloaded (client dvars the mod's scripts
 * set with setClientDvar stay set otherwise).
 *
 * cx_mod              list the mods and show which one the server runs
 * cx_mod <name>       load a mod
 * cx_mod off          back to the server's own scripts */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"

#define VA_CONNSTATE 0x1A2E7ECu /* g_connState: 0 lobby, 1..8 connecting and loading, 9 in a match (0x4FA590) */

static volatile LONG g_mod_busy;

static int conn_state(void)
{
    __try {
        return *(volatile int *)A(VA_CONNSTATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static void ini_path(char *path, size_t n)
{
    _snprintf(path, n, "%sr1-mod.ini", g_dir);
    path[n - 1] = 0;
}

static void mods_dir(char *dir, size_t n)
{
    char ini[MAX_PATH], def[MAX_PATH];
    size_t len;
    ini_path(ini, sizeof ini);
    _snprintf(def, sizeof def, "%smods", g_dir);
    def[sizeof def - 1] = 0;
    GetPrivateProfileStringA("r1-mod", "mods_dir", def, dir, (DWORD)n, ini);
    len = strlen(dir);
    while (len && (dir[len - 1] == '\\' || dir[len - 1] == '/'))
        dir[--len] = 0;
}

/* value of a dvar from the server's reply: "name" is: "value^7" default: ... */
static int server_dvar(const char *name, char *value, size_t n)
{
    char reply[2048];
    const char *start, *end;
    size_t len;
    if (!rcon_exec(name, reply, sizeof reply))
        return 0;
    start = strstr(reply, "is: \"");
    if (!start)
        return 0;
    start += 5;
    end = strchr(start, '"');
    if (!end)
        return 0;
    len = (size_t)(end - start);
    if (len >= 2 && start[len - 2] == '^' && start[len - 1] == '7')
        len -= 2;
    if (len >= n)
        len = n - 1;
    memcpy(value, start, len);
    value[len] = 0;
    return 1;
}

static int is_mod_folder(const char *path)
{
    char maps[MAX_PATH];
    DWORD attr;
    _snprintf(maps, sizeof maps, "%s\\maps", path);
    maps[sizeof maps - 1] = 0;
    attr = GetFileAttributesA(maps);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* the mod folder an overlay path names, or NULL when it is not under mods_dir */
static const char *mod_of_overlay(const char *overlay, const char *dir)
{
    size_t len = strlen(dir);
    if (!len || _strnicmp(overlay, dir, len) || (overlay[len] != '\\' && overlay[len] != '/') || !overlay[len + 1])
        return NULL;
    return overlay + len + 1;
}

/* <mod folder>\<file>: one command per line, // comments; to the server over rcon (mod.cfg) or to the client's
 * command buffer (client_off.cfg) */
static void run_mod_cfg(const char *path, const char *file, int on_server)
{
    char cfg[MAX_PATH], line[1024];
    FILE *f;
    _snprintf(cfg, sizeof cfg, "%s\\%s", path, file);
    cfg[sizeof cfg - 1] = 0;
    f = fopen(cfg, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line - 1, f)) { /* room for the "\n" a client line gets */
        char *p = line, reply[512];
        size_t len;
        while (*p == ' ' || *p == '\t')
            p++;
        len = strlen(p);
        while (len && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' '))
            p[--len] = 0;
        if (!*p || !strncmp(p, "//", 2))
            continue;
        if (!on_server) {
            strcat(p, "\n");
            Cbuf_AddText(p);
        } else if (rcon_exec(p, reply, sizeof reply)) {
            out("cx_mod: %s\n", p);
        }
    }
    fclose(f);
}

static void list_mods(const char *dir)
{
    char pattern[MAX_PATH], path[MAX_PATH], overlay[MAX_PATH];
    const char *active = NULL;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int count = 0;
    overlay[0] = 0;
    if (server_dvar("codol_scriptNativeOverlay", overlay, sizeof overlay))
        active = mod_of_overlay(overlay, dir);
    _snprintf(pattern, sizeof pattern, "%s\\*", dir);
    pattern[sizeof pattern - 1] = 0;
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
                continue;
            _snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
            path[sizeof path - 1] = 0;
            if (!is_mod_folder(path))
                continue;
            out("  %s%s\n", fd.cFileName, active && !_stricmp(active, fd.cFileName) ? "   (running)" : "");
            count++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!count)
        out("cx_mod: no mods in %s (a mod is a folder with a maps\\ subfolder of GSC)\n", dir);
    if (!active)
        out("cx_mod: the server runs its own scripts%s\n", overlay[0] ? "" : " (or did not answer)");
}

static DWORD WINAPI mod_thread(LPVOID param)
{
    char *name = (char *)param, dir[MAX_PATH], path[MAX_PATH], ini[MAX_PATH], overlay[MAX_PATH], base[MAX_PATH];
    char line[MAX_PATH + 64], reply[512], id[64], new_id[64];
    const char *target, *active;
    int rejoin, waited;
    overlay[0] = 0;
    mods_dir(dir, sizeof dir);
    ini_path(ini, sizeof ini);
    if (!name) {
        list_mods(dir);
        goto done;
    }
    if (!server_dvar("codol_scriptNativeOverlay", overlay, sizeof overlay)) {
        out("cx_mod: the server did not answer over rcon (see cx_rcon)\n");
        goto done;
    }
    /* remember the server's own overlay the first time it is replaced */
    if (!mod_of_overlay(overlay, dir))
        WritePrivateProfileStringA("r1-mod", "mod_base_overlay", overlay, ini);

    if (!_stricmp(name, "off")) {
        GetPrivateProfileStringA("r1-mod", "mod_base_overlay", "", base, sizeof base, ini);
        if (!base[0] && !server_dvar("codol_scriptOverlay", base, sizeof base)) {
            out("cx_mod: no mod_base_overlay in r1-mod.ini and no codol_scriptOverlay to fall back on\n");
            goto done;
        }
        if (!mod_of_overlay(overlay, dir) && !_stricmp(overlay, base)) {
            out("cx_mod: no mod is loaded\n");
            goto done;
        }
        target = base;
    } else {
        _snprintf(path, sizeof path, "%s\\%s", dir, name);
        path[sizeof path - 1] = 0;
        if (strpbrk(name, "\\/:.") || !is_mod_folder(path)) {
            out("cx_mod: no mod \"%s\" in %s\n", name, dir);
            goto done;
        }
        run_mod_cfg(path, "mod.cfg", 1);
        target = path;
    }
    /* leaving a mod (off, or for another one): undo what it set on this client */
    active = mod_of_overlay(overlay, dir);
    if (active && _stricmp(active, name)) {
        _snprintf(line, sizeof line, "%s\\%s", dir, active);
        line[sizeof line - 1] = 0;
        run_mod_cfg(line, "client_off.cfg", 0);
    }
    if (strchr(target, ' '))
        out("cx_mod: warning: \"%s\" has a space, which rcon may split\n", target);
    _snprintf(line, sizeof line, "set codol_scriptNativeOverlay \"%s\"", target);
    line[sizeof line - 1] = 0;
    if (!rcon_exec(line, reply, sizeof reply))
        goto done;
    if (!server_dvar("codol_scriptNativeOverlay", overlay, sizeof overlay) || _stricmp(overlay, target)) {
        out("cx_mod: the server kept \"%s\"; not restarting\n", overlay);
        goto done;
    }
    rejoin = conn_state() > 0;
    if (rejoin) {
        Cbuf_AddText("disconnect\n");
        for (waited = 0; conn_state() > 0 && waited < 10000; waited += 100)
            Sleep(100);
    }
    if (!server_dvar("sv_serverid", id, sizeof id))
        id[0] = 0;
    out("cx_mod: %s; restarting the map\n", _stricmp(name, "off") ? name : "server scripts");
    rcon_exec("map_restart", reply, sizeof reply);
    if (!rejoin)
        goto done;
    /* the level is up again once the server answers with another id */
    for (waited = 0; waited < 60000; waited += 1000) {
        Sleep(1000);
        if (server_dvar("sv_serverid", new_id, sizeof new_id) && strcmp(new_id, id))
            break;
    }
    if (waited >= 60000) {
        out("cx_mod: the server did not come back within 60 s; rejoin with cx_match\n");
        goto done;
    }
    Sleep(2000);
    out("cx_mod: rejoining\n");
    Cbuf_AddText("cx_match\n");
done:
    free(name);
    InterlockedExchange(&g_mod_busy, 0);
    return 0;
}

static void __cdecl cmd_cx_mod(void)
{
    HANDLE t;
    if (Cmd_Argc() > 2) {
        out("usage: cx_mod [<name>|off]   (mods are folders in [r1-mod] mods_dir, default <dll dir>\\mods)\n");
        return;
    }
    if (InterlockedExchange(&g_mod_busy, 1)) {
        out("cx_mod: a switch is still running\n");
        return;
    }
    t = CreateThread(NULL, 0, mod_thread, Cmd_Argc() == 2 ? _strdup(Cmd_Argv(1)) : NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        InterlockedExchange(&g_mod_busy, 0);
}

void mod_register(void)
{
    command_add("cx_mod", cmd_cx_mod);
}
