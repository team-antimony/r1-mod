/* ---- client-side mods: a mod folder on top of the file system ----
 * The QA build kept IW4's file system. FS_Startup's path builder (sub_627D30) adds every search path through
 * sub_627430 (usercall: ecx = directory name, one stack argument = base path, caller pops; it also adds the
 * directory's localized subfolders), and the last one added is searched first. Every file the client opens walks
 * those paths (sub_626230) and ends in sub_58EE90, which the loose-file exe points at plain fopen, so a directory
 * added after the engine's own overrides every loose file: images\*.iwi, models\ stream models, sound\, .cfg files.
 * Fastfile assets (rawfiles, string tables, weapon definitions) do not go through it, and QA has no mod.ff.
 *
 * Both calls of the path builder are hooked: FS_InitFilesystem (sub_628130, call 0x62815F) and FS_Restart
 * (sub_628210, call 0x62821A). After the engine's paths r1-mod adds <mods_dir>\<client_mod>. IW4's own fs_game (dvar
 * "1006" here) is left alone on purpose: the engine also keeps the player's config in the fs_game folder.
 *
 * The mod is [r1-mod] client_mod= in r1-mod.ini (read at start-up) or cx_clientmod. A change in the lobby calls
 * FS_Restart (sub_628210) on the game thread and then vid_restart to reload what is already loaded. vid_restart alone
 * does not rebuild the paths: sub_465260 restarts the file system only while 0x1A2A578 > 0 and fs_game changed.
 * Installed from DllMain: the file system starts before main_thread runs. A server can name the mod its players need
 * (r1_clientMod, below). */
#include "std_include.h"
#include <ctype.h>
#include "game/game.h"
#include "utils/hook.h"
#include "component/components.h"

#define VA_FS_PATHS        0x627D30u  /* cdecl void (void): FS_Startup's search paths */
#define VA_FS_ADD_GAME_DIR 0x627430u  /* usercall: ecx = directory, push base path */
#define VA_FS_INIT_CALL    0x62815Fu  /* in FS_InitFilesystem */
#define VA_FS_RESTART_CALL 0x62821Au  /* in FS_Restart */
#define VA_FS_RESTART      0x628210u  /* cdecl FS_Restart(int localClientNum, int checksumFeed) */
#define VA_CONNSTATE       0x1A2E7ECu

typedef void(__cdecl *paths_fn)(void);

static paths_fn g_orig_paths;
static CRITICAL_SECTION g_lock;
static char g_root[MAX_PATH];  /* <mods_dir>, no trailing slash */
static char g_mod[64];         /* "" = none */
static char g_added[MAX_PATH]; /* folder added by the last path build, "" = none */
static volatile LONG g_builds; /* path builds seen */
static int g_sites;

static void ini_file(char *path, size_t n, const char *dir)
{
    _snprintf(path, n, "%sr1-mod.ini", dir);
    path[n - 1] = 0;
}

/* a mod name is one folder name: letters, digits, '_', '-', '.' (not "." or "..") */
static int valid_name(const char *name)
{
    const char *p;
    if (!*name || strlen(name) >= sizeof g_mod || !strcmp(name, ".") || !strcmp(name, ".."))
        return 0;
    for (p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.')
            return 0;
    return 1;
}

static int is_dir(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void __cdecl add_game_dir(const char *dir, const char *base)
{
    uintptr_t fn = A(VA_FS_ADD_GAME_DIR);
    __asm {
        mov ecx, dir
        push base
        mov eax, fn
        call eax
        add esp, 4
    }
}

static void __cdecl paths_hook(void)
{
    char mod[sizeof g_mod], path[MAX_PATH];
    g_orig_paths();
    EnterCriticalSection(&g_lock);
    strcpy(mod, g_mod);
    g_added[0] = 0;
    if (mod[0]) {
        _snprintf(path, sizeof path, "%s\\%s", g_root, mod);
        path[sizeof path - 1] = 0;
        if (is_dir(path)) {
            add_game_dir(mod, g_root);
            strcpy(g_added, path);
        }
    }
    LeaveCriticalSection(&g_lock);
    InterlockedIncrement(&g_builds);
}

int fsmod_install(void)
{
    char dll[MAX_PATH], ini[MAX_PATH], def[MAX_PATH], mod[sizeof g_mod], *slash;
    size_t len;
    InitializeCriticalSection(&g_lock);
    GetModuleFileNameA(g_self, dll, sizeof dll);
    slash = strrchr(dll, '\\');
    if (slash)
        slash[1] = 0;
    ini_file(ini, sizeof ini, dll);
    _snprintf(def, sizeof def, "%smods", dll);
    def[sizeof def - 1] = 0;
    GetPrivateProfileStringA("r1-mod", "mods_dir", def, g_root, sizeof g_root, ini);
    len = strlen(g_root);
    while (len && (g_root[len - 1] == '\\' || g_root[len - 1] == '/'))
        g_root[--len] = 0;
    GetPrivateProfileStringA("r1-mod", "client_mod", "", mod, sizeof mod, ini);
    _strlwr(mod);
    if (valid_name(mod))
        strcpy(g_mod, mod);
    g_sites = patch_call(VA_FS_INIT_CALL, VA_FS_PATHS, (void *)paths_hook, (void **)&g_orig_paths);
    g_sites += patch_call(VA_FS_RESTART_CALL, VA_FS_PATHS, (void *)paths_hook, NULL);
    return g_sites;
}

const char *fsmod_dir(void)
{
    return g_added[0] ? g_added : NULL;
}

void fsmod_report(int sites)
{
    if (sites != 2) {
        out("r1-mod: file system path builder not recognised (%d of 2 calls), client mods unavailable\n", sites);
        return;
    }
    if (g_added[0])
        out("r1-mod: client mod '%s' loaded from %s\n", g_mod, g_added);
    else if (g_mod[0])
        out("r1-mod: client mod '%s' not found in %s\n", g_mod, g_root);
}

static void list_client_mods(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pattern[MAX_PATH];
    int n = 0;
    _snprintf(pattern, sizeof pattern, "%s\\*", g_root);
    pattern[sizeof pattern - 1] = 0;
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && valid_name(fd.cFileName)) {
                out("  %s\n", fd.cFileName);
                n++;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!n)
        out("  (no mod folders in %s)\n", g_root);
}

/* cx_clientmod                list the mods and show the loaded one
 * cx_clientmod <name> | off   load or unload a client mod now (vid_restart) and keep it for the next start */
static void __cdecl cmd_cx_clientmod(void)
{
    char want[sizeof g_mod], ini[MAX_PATH], path[MAX_PATH];
    int state;
    LONG before;
    if (g_sites != 2) {
        out("cx_clientmod: the file system hook is not installed (see r1-mod.log)\n");
        return;
    }
    if (Cmd_Argc() < 2) {
        out("client mod: %s%s%s\nmods in %s:\n", g_added[0] ? g_mod : "none", g_added[0] ? " from " : "",
            g_added[0] ? g_added : "", g_root);
        list_client_mods();
        out("cx_clientmod <name> loads one (vid_restart), cx_clientmod off unloads it\n");
        return;
    }
    strncpy(want, Cmd_Argv(1), sizeof want - 1);
    want[sizeof want - 1] = 0;
    _strlwr(want);
    if (!strcmp(want, "off"))
        want[0] = 0;
    else if (!valid_name(want)) {
        out("cx_clientmod: '%s' is not a folder name\n", Cmd_Argv(1));
        return;
    } else {
        _snprintf(path, sizeof path, "%s\\%s", g_root, want);
        path[sizeof path - 1] = 0;
        if (!is_dir(path)) {
            out("cx_clientmod: no folder %s\n", path);
            return;
        }
    }
    EnterCriticalSection(&g_lock);
    strcpy(g_mod, want);
    LeaveCriticalSection(&g_lock);
    if (Cmd_Argc() < 3 || strcmp(Cmd_Argv(2), "server")) { /* "server": asked for by r1_clientMod, not saved */
        ini_file(ini, sizeof ini, g_dir);
        WritePrivateProfileStringA("r1-mod", "client_mod", want, ini);
    }
    state = *(volatile int *)A(VA_CONNSTATE);
    if (state != 0) {
        out("cx_clientmod: saved; it takes effect in the lobby (vid_restart) or at the next start\n");
        return;
    }
    before = g_builds;
    __try {
        ((void(__cdecl *)(int, int))A(VA_FS_RESTART))(0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (g_builds == before) {
        out("cx_clientmod: saved; the file system did not restart, so it takes effect at the next start\n");
        return;
    }
    out("cx_clientmod: %s; file system restarted, reloading the renderer (vid_restart)\n",
        g_added[0] ? g_added : "off");
    Cbuf_AddText("vid_restart\n");
}

/* Search path list (sub_626230 walks it, first node first): node {next, dir, iwd, ?, language}; a directory node
 * points at {char base[256]; char gamedir[256]}. */
#define VA_FS_SEARCHPATHS 0x78852C0u
#define VA_FS_READ_MODE   0x783F654u /* loose / archive order the client was started with */
#define VA_FS_FOPEN_READ  0x626540u  /* usercall int (edi = int *handle, const char *name, int mode): length or < 0 */
#define VA_FS_FCLOSE      0x625FE0u  /* cdecl (int handle) */

struct fs_dir {
    char base[256];
    char gamedir[256];
};

/* calls fn(base, gamedir, ctx) for each directory search path in search order; returns how many */
static int walk_paths(void (*fn)(const struct fs_dir *, int, void *), void *ctx)
{
    int n = 0;
    __try {
        const unsigned *node = *(const unsigned *const *)A(VA_FS_SEARCHPATHS);
        for (; node && n < 256; node = (const unsigned *)node[0]) {
            if (node[1]) {
                fn((const struct fs_dir *)node[1], n, ctx);
                n++;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out("  (search path list not readable)\n");
    }
    return n;
}

static void print_path(const struct fs_dir *d, int i, void *ctx)
{
    (void)ctx;
    out("  %2d  %.200s / %.60s\n", i, d->base, d->gamedir);
}

struct find_ctx {
    const char *file;
    int found;
};

static void find_in_path(const struct fs_dir *d, int i, void *ctx)
{
    struct find_ctx *c = (struct find_ctx *)ctx;
    char path[MAX_PATH * 2], *p;
    if (c->found >= 0)
        return;
    _snprintf(path, sizeof path, "%s\\%s\\%s", d->base, d->gamedir, c->file);
    path[sizeof path - 1] = 0;
    for (p = path; *p; p++)
        if (*p == '/')
            *p = '\\';
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        out("  served from search path %d: %s\n", i, path);
        c->found = i;
    }
}

static int engine_fopen_read(const char *name, int *handle)
{
    uintptr_t fn = A(VA_FS_FOPEN_READ);
    int mode = *(volatile int *)A(VA_FS_READ_MODE), r;
    __asm {
        push edi
        mov edi, handle
        push mode
        push name
        mov eax, fn
        call eax
        add esp, 8
        pop edi
        mov r, eax
    }
    return r;
}

/* cx_fspath   the search paths, highest priority first */
static void __cdecl cmd_cx_fspath(void)
{
    out("file system path builds: %ld; client mod folder: %s\nsearch paths (first wins):\n", g_builds,
        g_added[0] ? g_added : "(none)");
    walk_paths(print_path, NULL);
}

/* cx_fsfind <file>   open a file the way the game does (FS_FOpenFileRead) and say which search path has it */
static void __cdecl cmd_cx_fsfind(void)
{
    struct find_ctx c;
    int handle = 0, len;
    if (Cmd_Argc() < 2) {
        out("cx_fsfind <file>, e.g. cx_fsfind images/$white.iwi\n");
        return;
    }
    c.file = Cmd_Argv(1);
    c.found = -1;
    if (strstr(c.file, "..")) {
        out("cx_fsfind: no '..' in paths\n");
        return;
    }
    __try {
        len = engine_fopen_read(c.file, &handle);
        if (handle)
            ((void(__cdecl *)(int))A(VA_FS_FCLOSE))(handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        len = -100;
    }
    if (len >= 0)
        out("cx_fsfind: the game opens %s (%d bytes)\n", c.file, len);
    else if (len == -7)
        out("cx_fsfind: not on a game thread, cannot ask the game's file system\n");
    else
        out("cx_fsfind: the game cannot open %s (%d)\n", c.file, len);
    walk_paths(find_in_path, &c);
    if (c.found < 0)
        out("  in no directory search path (it may come from an archive or a zone)\n");
}


/* ---- the server's client mod (r1_clientMod) ----
 * r1-server sends r1_clientMod with the movement dvars (`s r1_clientMod "<name>"`). When it names a mod other than the
 * loaded one while the client is in a match, and mods_dir has that folder, r1-mod disconnects, loads it in the lobby
 * (FS_Restart + vid_restart, not saved to r1-mod.ini) and rejoins the same server with cx_match. Without the folder it
 * says so once. Back in the lobby the dvar is cleared, so a later server that does not send it changes nothing. */
#define VA_DVAR_REGISTER_STRING 0x633DD0u /* cdecl dvar *(name, default, flags, desc); value string at +0x0C */

static unsigned char *g_server_mod_dvar;
static char g_server_mod_seen[sizeof g_mod]; /* last name acted on or reported, to do it once per server value */
static volatile LONG g_server_mod_busy;

static DWORD WINAPI server_mod_thread(LPVOID param)
{
    char *name = (char *)param, target[64], line[128];
    LONG builds;
    int waited;
    strncpy(target, match_last_target(), sizeof target - 1);
    target[sizeof target - 1] = 0;
    out("r1-mod: this server uses client mod '%s': loading it and rejoining\n", name);
    Cbuf_AddText("disconnect\n");
    for (waited = 0; *(volatile int *)A(VA_CONNSTATE) != 0 && waited < 15000; waited += 100)
        Sleep(100);
    Sleep(1500);
    builds = g_builds;
    _snprintf(line, sizeof line, "cx_clientmod %s server\n", name);
    line[sizeof line - 1] = 0;
    Cbuf_AddText(line);
    for (waited = 0; g_builds == builds && waited < 15000; waited += 100)
        Sleep(100);
    Sleep(6000); /* vid_restart */
    if (target[0]) {
        _snprintf(line, sizeof line, "cx_match %s\n", target);
        line[sizeof line - 1] = 0;
        Cbuf_AddText(line);
    } else {
        out("r1-mod: client mod '%s' loaded; join the server again\n", name);
    }
    free(name);
    InterlockedExchange(&g_server_mod_busy, 0);
    return 0;
}

void fsmod_keep(void)
{
    static int was_connected;
    char want[sizeof g_mod], path[MAX_PATH];
    const char *value;
    int state;
    if (!g_server_mod_dvar || g_sites != 2)
        return;
    __try {
        state = *(volatile int *)A(VA_CONNSTATE);
        value = *(const char **)(g_server_mod_dvar + 0x0C);
        strncpy(want, value ? value : "", sizeof want - 1);
        want[sizeof want - 1] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (state == 0) {
        if (was_connected) {
            was_connected = 0;
            g_server_mod_seen[0] = 0;
            if (want[0])
                Cbuf_AddText("set r1_clientMod \"\"\n");
        }
        return;
    }
    was_connected = 1;
    _strlwr(want);
    if (!want[0] || !valid_name(want) || !strcmp(want, g_mod) || !strcmp(want, g_server_mod_seen) ||
        g_server_mod_busy)
        return;
    strcpy(g_server_mod_seen, want);
    _snprintf(path, sizeof path, "%s\\%s", g_root, want);
    path[sizeof path - 1] = 0;
    if (!is_dir(path)) {
        out("r1-mod: this server uses client mod '%s', which is not in %s; playing without it\n", want, g_root);
        return;
    }
    InterlockedExchange(&g_server_mod_busy, 1);
    CloseHandle(CreateThread(NULL, 0, server_mod_thread, _strdup(want), 0, NULL));
}

/* cx_fsmodinit: registers r1_clientMod on the game thread (queued once at start: Dvar_Register* keeps state in the
 * caller's thread-local storage) */
static void __cdecl cmd_cx_fsmodinit(void)
{
    typedef unsigned char *(__cdecl *reg_string_t)(const char *, const char *, int, const char *);
    if (g_server_mod_dvar)
        return;
    g_server_mod_dvar = ((reg_string_t)A(VA_DVAR_REGISTER_STRING))("r1_clientMod", "", 0,
                                                                    "Client mod the server uses (set by the server)");
}

void fsmod_start(void)
{
    Cbuf_AddText("cx_fsmodinit\n");
}

void fsmod_register(void)
{
    command_add("cx_clientmod", cmd_cx_clientmod);
    command_add("cx_fspath", cmd_cx_fspath);
    command_add("cx_fsfind", cmd_cx_fsfind);
    command_add("cx_fsmodinit", cmd_cx_fsmodinit);
}
