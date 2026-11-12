/* cx_room                create a room (no start timer runs: see apply_room_settings in onboarding.c), so classes can be
 *                        edited in the room's loadout panel for as long as needed
 * cx_start [ip[:port] [port] [password]]   ask the local lobby backend to push StartGame for this server now
 * cx_match [ip[:port] [port] [password]]   both: create a room, then start 3 s later (joins any r1-server: the
 *                        backend fetches its match key first; password = the server's join password)
 * The backend sends the push once <backend_dir>\startgame.trigger exists ([r1-mod] backend_dir in r1-mod.ini, else
 * R1MOD_BACKEND_DIR, the repository's backend\ folder the launcher found); the client only routes it while in a room. Class edits in the room
 * are written straight to playerdata (everything is owned, so nothing is a preview) and reach the server with the
 * stats the client sends when it connects. Pressing the room's own start button alone in the room also starts at
 * once: the host's all-Ready path, which the backend answers with StartGame. */
#include "std_include.h"
#include <ctype.h>
#include "game/game.h"
#include "component/components.h"
#include "component/overlay.h"

static char g_match_target[64];

/* The client keeps its rooms in a fixed pool of two (IDA 0xC8032B0, 9680 bytes each, byte 0 = in use).
 * room.create takes a slot with sub_6C9A90 and dwRoomImp::create (0x6E2650) writes through it without a
 * null check, so a third cx_match in one session (each one created a room and never left it) faulted at
 * 0x6E268D writing 0x25C8. room.leave frees the slot, through the backend when connected (sub_6E4580 ->
 * sub_6E87D0), so wait for a free one before creating. */
#define VA_ROOM_POOL       0xC8032B0u
#define ROOM_POOL_STRIDE   9680u
#define ROOM_POOL_SLOTS    2

static int room_slot_free(void)
{
    int i;
    __try {
        for (i = 0; i < ROOM_POOL_SLOTS; i++)
            if (*(volatile unsigned char *)(A(VA_ROOM_POOL) + ROOM_POOL_STRIDE * i) == 0)
                return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

static void trigger_path(char *path, size_t n)
{
    char ini[MAX_PATH], dir[MAX_PATH];
    size_t len;
    _snprintf(ini, sizeof ini, "%sr1-mod.ini", g_dir);
    GetPrivateProfileStringA("r1-mod", "backend_dir", "", dir, sizeof dir, ini);
    if (!dir[0] && !GetEnvironmentVariableA("R1MOD_BACKEND_DIR", dir, sizeof dir))
        dir[0] = 0;
    len = strlen(dir);
    _snprintf(path, n, "%s%sstartgame.trigger", dir, len && dir[len - 1] == '\\' ? "" : "\\");
    path[n - 1] = 0;
}

static char g_match_password[64];

/* target from the command's arguments: [ip[:port] [port] [password]], default 127.0.0.1 28960. The password is
 * the server's join password (r1_joinPassword); without one, [r1-mod] join_password in r1-mod.ini. The backend
 * asks that server for its match key with it before it starts the join. */
static void set_target(void)
{
    char ip[48], ini[MAX_PATH];
    const char *port = "28960", *colon;
    int next = 2;
    _snprintf(ip, sizeof ip - 1, "%s", Cmd_Argc() >= 2 ? Cmd_Argv(1) : "127.0.0.1");
    ip[sizeof ip - 1] = 0;
    colon = strchr(ip, ':');
    if (colon) {
        port = Cmd_Argv(1) + (colon - ip) + 1;
        ip[colon - ip] = 0;
    } else if (Cmd_Argc() >= 3) {
        port = Cmd_Argv(2);
        next = 3;
    }
    if (Cmd_Argc() > next) {
        _snprintf(g_match_password, sizeof g_match_password - 1, "%s", Cmd_Argv(next));
    } else {
        _snprintf(ini, sizeof ini, "%sr1-mod.ini", g_dir);
        GetPrivateProfileStringA("r1-mod", "join_password", "", g_match_password, sizeof g_match_password, ini);
    }
    g_match_password[sizeof g_match_password - 1] = 0;
    _snprintf(g_match_target, sizeof g_match_target - 1, "%s %s", ip, port);
    overlay_motd_query(ip, (unsigned short)atoi(port)); /* server name and message of the day for the loading screen */
}

static void request_start(const char *who)
{
    char path[MAX_PATH];
    FILE *f;
    trigger_path(path, sizeof path);
    f = fopen(path, "wb");
    if (!f) {
        out("%s: cannot write %s\n", who, path);
        return;
    }
    fputs(g_match_target, f);
    if (g_match_password[0]) {
        fputc(' ', f);
        fputs(g_match_password, f);
    }
    fclose(f);
    out("%s: StartGame requested for %s%s\n", who, g_match_target, g_match_password[0] ? " (with join password)" : "");
}

/* param: 1 = start the match once the room exists (cx_match), 0 = only the room (cx_room) */
static DWORD WINAPI room_later(LPVOID param)
{
    int waited;
    for (waited = 0; !room_slot_free() && waited < 5000; waited += 100)
        Sleep(100);
    if (!room_slot_free()) {
        out("cx_room: both room slots are still held after room.leave; not creating a room (restart the client)\n");
        return 0;
    }
    Cbuf_AddText("room.create\n");
    if (!param) {
        out("cx_room: room created; edit classes in the room, then cx_start (or the room's start button)\n");
        return 0;
    }
    Sleep(3000);
    request_start("cx_match");
    return 0;
}

static void create_room(int start)
{
    /* leave the room an earlier cx_room / cx_match (or the last match) left behind; room_later creates the new
     * one once the pool has a free slot */
    Cbuf_AddText("room.leave\n");
    CreateThread(NULL, 0, room_later, (LPVOID)(intptr_t)start, 0, NULL);
}

static void __cdecl cmd_cx_room(void)
{
    create_room(0);
}

static void __cdecl cmd_cx_start(void)
{
    set_target();
    request_start("cx_start");
}

static void __cdecl cmd_cx_match(void)
{
    set_target();
    create_room(1);
}

/* ---- the server's message of the day on the loading screen ----
 * The loading screen's intel panel (F1) shows up to three tips: dvars loadHintEnable_N / loadHintIcon_N /
 * loadHintName_N / loadHintDesc_N, which the UI script command setLoadingHint (0x617740, entry at 0xC52404 in the UI
 * command table) fills with localization keys from mp/loadingScreenHints.csv ("LOAD_HINT_TDM_HINT_DESC_0"); the
 * menu shows "@" + key, localized. Its table entry is pointed at a wrapper: after the game has picked its tips, the
 * localized strings of tip 0's two keys (LocalizeEntry assets, type 27: {value, name}) point at the server's name and
 * sv_motd, so the panel shows them through the game's own path (a plain dvar value came out as "@Welcome!..."). They
 * get their own text back on the next join. overlay_motd_query asked the server when the join started.
 *
 * The text is the server's, so only this reaches the UI: printable characters, and colour codes ^0..^9, ^: and ^; (the text
 * renderer draws them as colours). Every other '^' sequence is dropped: '^' followed by a control character embeds a
 * material (button icons) in this renderer. Control characters and a leading '@' (a localization key) go too. */
#define VA_UI_CMD_SET_LOADING_HINT 0xC52404u /* {name, function} pair: the function pointer */
#define VA_DB_BUCKETS              0x1BC9F20u
#define VA_DB_ENTRIES              0x1FC4E28u
#define DB_HASH_SIZE               0x13880u
#define ASSET_LOCALIZE             27
#define MOTD_HINT_MAX              240

typedef void(__cdecl *ui_command_fn)(int, int, int);
static ui_command_fn g_set_loading_hint;

static void safe_text(char *out, size_t size, const char *in)
{
    size_t n = 0;
    while (*in == '@' || *in == ' ')
        in++;
    for (; *in && n + 1 < size; in++) {
        if (in[0] == '^') {
            if (((in[1] >= '0' && in[1] <= '9') || in[1] == ':' || in[1] == ';' || in[1] == '<') && n + 2 < size) {
                out[n++] = '^';
                out[n++] = in[1];
            }
            if (in[1])
                in++;
            continue;
        }
        if ((unsigned char)*in < 32)
            continue;
        out[n++] = *in;
    }
    out[n] = 0;
}

/* the value field of a loaded LocalizeEntry, NULL when the key is not loaded */
const char **localize_slot(const char *key)
{
    const unsigned *buckets = (const unsigned *)A(VA_DB_BUCKETS);
    const unsigned *entries = (const unsigned *)A(VA_DB_ENTRIES);
    unsigned h = ASSET_LOCALIZE, i, guard;
    const char *c;
    if (!key || !*key)
        return NULL;
    for (c = key; *c; c++)
        h = (*c == '\\' ? '/' : (unsigned)tolower((unsigned char)*c)) + 31u * h;
    for (i = buckets[h % DB_HASH_SIZE], guard = 0; i && guard < 4096; i = entries[7 * i + 5], guard++) {
        const unsigned *e = &entries[7 * i];
        const char **loc = (const char **)e[2];
        if (e[0] == ASSET_LOCALIZE && loc && loc[1] && !_stricmp(loc[1], key))
            return loc;
    }
    return NULL;
}

static struct {
    const char **slot;
    const char *orig;
} g_loc_patch[2];
static char g_hint_name[128], g_hint_desc[MOTD_HINT_MAX];

static void hint_restore(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        __try {
            if (g_loc_patch[i].slot)
                *g_loc_patch[i].slot = g_loc_patch[i].orig;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        g_loc_patch[i].slot = NULL;
    }
}

/* tip 0's localized name and description now read name and desc */
static void hint_apply(const char *name, const char *desc)
{
    static const char *const dvars[2] = {"loadHintName_0", "loadHintDesc_0"};
    char key[96];
    int i;
    strcpy(g_hint_name, name);
    strcpy(g_hint_desc, desc);
    for (i = 0; i < 2; i++) {
        __try {
            unsigned char *dvar = Dvar_FindVar(dvars[i]);
            const char *value = dvar ? *(const char **)(dvar + 0x0C) : NULL;
            const char **slot;
            if (!value)
                continue;
            _snprintf(key, sizeof key - 1, "%s", value[0] == '@' ? value + 1 : value);
            key[sizeof key - 1] = 0;
            if (!(slot = localize_slot(key)))
                continue;
            g_loc_patch[i].slot = slot;
            g_loc_patch[i].orig = *slot;
            *slot = i ? g_hint_desc : g_hint_name;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

/* The black bar above the loading bar is the loadscreen menu's "did you know" line: one random row of
 * mp/didyouknow.csv (one column of "@PLATFORM_DYK_..." keys; the coop, zombies and other modes have their own
 * tables). With a message of the day every row of those tables points at it (plain text, which the menu shows as it
 * is, where a '@' key is localized); the rows get their own text back for a server without one. */
static const char *const g_dyk_tables[] = {
    "mp/didyouknow.csv", "mp/didyouknowcoop.csv", "mp/didyouknowzm.csv", "mp/didyouknowevac.csv",
    "mp/didyouknowsurvival.csv", "mp/didyouknowgunship.csv", "mp/didyouknowasym.csv", "mp/didyouknowborg.csv",
};
#define DYK_MAX_ROWS 128
static struct {
    const unsigned char *table;
    const char *orig[DYK_MAX_ROWS];
    int rows;
} g_dyk[sizeof g_dyk_tables / sizeof g_dyk_tables[0]];
static char g_dyk_text[MOTD_HINT_MAX];

static void dyk_apply(const char *text)
{
    size_t t;
    int r;
    if (text)
        strcpy(g_dyk_text, text);
    for (t = 0; t < sizeof g_dyk_tables / sizeof g_dyk_tables[0]; t++) {
        __try {
            const unsigned char *table = find_table(g_dyk_tables[t]);
            int cols, rows;
            const char **cells;
            if (!table)
                continue;
            cols = *(const int *)(table + 4);
            rows = *(const int *)(table + 8);
            cells = *(const char ***)(table + 12);
            if (cols < 1 || rows < 1 || rows > DYK_MAX_ROWS || !cells)
                continue;
            if (g_dyk[t].table != table) { /* first use (or a reloaded table): keep the table's own text */
                g_dyk[t].table = table;
                g_dyk[t].rows = rows;
                for (r = 0; r < rows; r++)
                    g_dyk[t].orig[r] = cells[2 * (r * cols)];
            }
            for (r = 0; r < g_dyk[t].rows && r < rows; r++)
                cells[2 * (r * cols)] = text ? g_dyk_text : g_dyk[t].orig[r];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_dyk[t].table = NULL;
        }
    }
}

static void __cdecl set_loading_hint(int a, int b, int c)
{
    char host[128], motd[MOTD_HINT_MAX], name[128], text[MOTD_HINT_MAX];
    hint_restore(); /* the last join's patched strings get their own text back before the game reads them */
    g_set_loading_hint(a, b, c);
    if (!overlay_motd_get(host, sizeof host, motd, sizeof motd) || !motd[0]) {
        dyk_apply(NULL);
        return;
    }
    safe_text(name, sizeof name, host[0] ? host : "Message of the day");
    safe_text(text, sizeof text, motd);
    if (!text[0]) {
        dyk_apply(NULL);
        return;
    }
    dyk_apply(text);
    hint_apply(name[0] ? name : "Message of the day", text);
}

static void install_motd_hint(void)
{
    ui_command_fn *slot = (ui_command_fn *)A(VA_UI_CMD_SET_LOADING_HINT);
    DWORD old;
    if (*slot != (ui_command_fn)A(0x617740u) || !VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) {
        out("r1-mod: loading tip not hooked (setLoadingHint not where expected)\n");
        return;
    }
    g_set_loading_hint = *slot;
    *slot = set_loading_hint;
    VirtualProtect(slot, sizeof *slot, old, &old);
}

void match_register(void)
{
    command_add("cx_room", cmd_cx_room);
    command_add("cx_start", cmd_cx_start);
    command_add("cx_match", cmd_cx_match);
    install_motd_hint();
}

/* "ip port" of the last cx_match / cx_start target, "" before the first (fsmod.c rejoins with it) */
const char *match_last_target(void)
{
    return g_match_target;
}
