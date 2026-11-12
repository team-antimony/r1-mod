/* ---- secure stats overlay (rank, prestige, unlock flags, weapon levels), non-destructive ----
 * Rank and unlock progress live in the secure playerdata (mp/playersecuredata.def v19, checksum 0x8a317d69), a
 * 32,764-byte buffer at the stats object + 37304 (sub_6DA140() + 9326 dwords). The local backend serves none, so
 * it is all zero and the player is level 1 with every gated item, map and mode locked.
 * - rank is stored 1-based (sub_590720 returns rank - 1 as the rankTable row) and experience is XP within that
 *   rank (sub_590550 adds the row minimum), so level N = rank N, experience 0. rankVersion must equal the table
 *   version (maxrank row, column 3) or experience is clamped.
 * - Every lock (items via sub_692CE0, maps sub_5A8A20, modes sub_5B33B0, notifications sub_5C63B0, per-weapon
 *   attachment/camo levels via uf_<weapon>_mp_rank) tests unlockFlagsNew[flag] >= tier (sub_4A4CA0), a short[2048]
 *   at byte 8291 + 2 * index; the index is column 1 of mp/unlockFlags.csv. The blanket "flags N" setting raises only
 *   gate flags (*_rank, cf_ch_*, uf_mp_*, *_completed_*); any flag, state flags included, can be set by name.
 * - Weapon level is item XP (item info out[28], WL_GetWeaponRank -> sub_46EB20 over coWeapon*RankTable). Items
 *   owned only through the cx_unlock patch have no inventory record, and sub_695BC0 zeroes out[28] on that path;
 *   its owned tail at 0x695C32 is redirected to a stub that writes the weapon's XP (out[0] = item id): a per-weapon
 *   override from `weapon <name> <xp>`, else the blanket weaponxp. Nothing is written to the buffer for this.
 *
 * Non-destructive: every value the overlay writes keeps the value it replaced (the first write wins; when the
 * engine or the backend changes a value behind the overlay, that becomes the original). Each pass writes what the
 * settings want and puts back every value no setting wants any more, so removing an override, lowering a setting
 * or turning the overlay off restores the profile's own values. r1-mod never saves the secure buffer.
 *
 * The thread re-applies the settings every 2 s. On by default; r1-mod_nostats.txt next to the DLL starts with it
 * off. Settings and overrides are kept in r1-mod_stats.txt. */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"
#include "component/overlay.h"

#define SD_OFFSET       37304
#define SD_SIZE         32764
#define SD_EXPERIENCE   4840
#define SD_PRESTIGE     4879
#define SD_RANK         4883
#define SD_RANKVERSION  4887
#define SD_FLAGS_NEW    8291
#define SD_FLAGS_COUNT  2048
#define VA_ITEM_OWNED_TAIL 0x695C32u
static const unsigned char g_tail_orig[9] = {0x83, 0xC9, 0xFF, 0x66, 0xC7, 0x40, 0x04, 0x01, 0x00};

#define MAX_SAVED     4096
#define MAX_OVERRIDES 512
#define KEY_LEN       64

enum ov_kind { OV_FLAG = 1, OV_WEAPON, OV_RAW };

struct override {
    int kind;
    char key[KEY_LEN];  /* flag or weapon name; raw: "" */
    int index;          /* flag index, item id or raw byte offset; -1 = not resolved yet */
    int size;           /* raw: 1, 2 or 4 */
    int value;
};

struct saved {
    int off, size;
    int orig;  /* the profile's value */
    int wrote; /* the overlay's value */
    int seen;  /* pass number that wanted it */
};

static volatile LONG g_stats_on;
static int g_level = 70, g_prestige = 0, g_flag_level = 100, g_experience = 0;
static int g_weapon_xp = 121230; /* top of coWeaponPrimaryRankTable (level 30); above the secondary table's top */
static int g_rank_version = -1, g_max_level = 70, g_max_prestige = 3;
static short g_flag_index[SD_FLAGS_COUNT];
static int g_flag_count = -1;
static CRITICAL_SECTION g_lock;
static struct saved g_saved[MAX_SAVED];
static int g_saved_n, g_pass;
static struct override g_ov[MAX_OVERRIDES];
static int g_ov_n;
static volatile LONG g_tables_ready;
/* every unlock flag (name, index), copied from mp/unlockFlags.csv for the overlay panel, which runs on the render
 * thread and must not ask the asset system */
static struct { char name[KEY_LEN]; short index; } *g_all_flags;
static int g_all_flag_n;

static unsigned char *sd_buf(void)
{
    return (unsigned char *)A(VA_PD_OBJECT) + SD_OFFSET;
}

static int sd_read(const unsigned char *b, int off, int size)
{
    return size == 1 ? (int)b[off] : size == 2 ? (int)*(const short *)(b + off) : *(const int *)(b + off);
}

static void sd_write(unsigned char *b, int off, int size, int v)
{
    if (size == 1)
        b[off] = (unsigned char)v;
    else if (size == 2)
        *(short *)(b + off) = (short)v;
    else
        *(int *)(b + off) = v;
}

static struct saved *saved_find(int off)
{
    int i;
    for (i = 0; i < g_saved_n; i++)
        if (g_saved[i].off == off)
            return &g_saved[i];
    return NULL;
}

/* this pass wants value at off; returns 1 when the buffer changed. Caller holds g_lock, inside __try. */
static int sd_put(unsigned char *b, int off, int size, int v)
{
    struct saved *s = saved_find(off);
    int cur;
    if (off < 0 || off + size > SD_SIZE)
        return 0;
    cur = sd_read(b, off, size);
    if (!s) {
        if (g_saved_n == MAX_SAVED)
            return 0;
        s = &g_saved[g_saved_n++];
        s->off = off;
        s->size = size;
        s->orig = cur;
    } else if (cur != s->wrote) {
        s->orig = cur; /* changed behind the overlay: the new value is the profile's */
    }
    s->wrote = v;
    s->seen = g_pass;
    if (cur == v)
        return 0;
    sd_write(b, off, size, v);
    return 1;
}

/* put back every saved value this pass did not want; returns the number restored */
static int sd_release_unwanted(unsigned char *b)
{
    int i, n = 0;
    for (i = 0; i < g_saved_n;) {
        struct saved *s = &g_saved[i];
        if (s->seen == g_pass) {
            i++;
            continue;
        }
        if (sd_read(b, s->off, s->size) == s->wrote && s->orig != s->wrote) {
            sd_write(b, s->off, s->size, s->orig);
            n++;
        }
        *s = g_saved[--g_saved_n];
    }
    return n;
}

static int gate_flag(const char *name)
{
    size_t n = strlen(name);
    return (n > 5 && !strcmp(name + n - 5, "_rank")) || !strncmp(name, "cf_ch_", 6) ||
           !strncmp(name, "uf_mp_", 6) || strstr(name, "_completed_") != NULL;
}

/* index of an unlock flag (name or number), -1 when unknown */
static int flag_lookup(const char *key)
{
    const unsigned char *t;
    int r, rows;
    if (*key >= '0' && *key <= '9') {
        r = atoi(key);
        return r > 0 && r < SD_FLAGS_COUNT ? r : -1;
    }
    if (!(t = find_table("mp/unlockFlags.csv")))
        return -1;
    rows = *(const int *)(t + 8);
    for (r = 0; r < rows; r++)
        if (!_stricmp(table_cell(t, r, 0), key)) {
            int idx = atoi(table_cell(t, r, 1));
            return idx > 0 && idx < SD_FLAGS_COUNT ? idx : -1;
        }
    return -1;
}

/* marketplace item id of a weapon (name or number), -1 when unknown */
static int item_lookup(const char *key)
{
    const unsigned char *t;
    int r, rows;
    if (*key >= '0' && *key <= '9')
        return atoi(key);
    if (!(t = find_table("mp/coMarketplaceItems.csv")))
        return -1;
    rows = *(const int *)(t + 8);
    for (r = 0; r < rows; r++)
        if (!_stricmp(table_cell(t, r, 1), key))
            return atoi(table_cell(t, r, 0));
    return -1;
}

static void stats_load_tables(void)
{
    const unsigned char *t = find_table("mp/rankTable.csv");
    int r, rows, n = 0;
    if (t) {
        rows = *(const int *)(t + 8);
        for (r = 0; r < rows; r++) {
            if (!strcmp(table_cell(t, r, 0), "maxrank")) {
                g_max_level = atoi(table_cell(t, r, 1)) + 1;
                g_rank_version = atoi(table_cell(t, r, 3));
            } else if (!strcmp(table_cell(t, r, 0), "maxprestige")) {
                g_max_prestige = atoi(table_cell(t, r, 1));
            }
        }
    }
    t = find_table("mp/unlockFlags.csv");
    if (t) {
        rows = *(const int *)(t + 8);
        for (r = 0; r < rows && n < SD_FLAGS_COUNT; r++) {
            int idx = atoi(table_cell(t, r, 1));
            if (idx > 0 && idx < SD_FLAGS_COUNT && gate_flag(table_cell(t, r, 0)))
                g_flag_index[n++] = (short)idx;
        }
        if (!g_all_flags && rows > 0 &&
            (g_all_flags = calloc((size_t)rows, sizeof *g_all_flags)) != NULL) {
            for (r = 0; r < rows; r++) {
                int idx = atoi(table_cell(t, r, 1));
                if (idx <= 0 || idx >= SD_FLAGS_COUNT)
                    continue;
                strncpy(g_all_flags[g_all_flag_n].name, table_cell(t, r, 0), KEY_LEN - 1);
                g_all_flags[g_all_flag_n].index = (short)idx;
                g_all_flag_n++;
            }
        }
    }
    g_flag_count = n;
    g_tables_ready = 1;
    out("r1-mod: stats tables: max level %d, prestige %d, rank version %d, %d gate flags\n", g_max_level,
        g_max_prestige, g_rank_version, g_flag_count);
}

static void resolve_overrides(void)
{
    int i;
    if (!g_tables_ready)
        return;
    for (i = 0; i < g_ov_n; i++) {
        struct override *o = &g_ov[i];
        if (o->index != -1 || o->kind == OV_RAW)
            continue; /* resolved, or reported as unknown (-2) */
        o->index = o->kind == OV_FLAG ? flag_lookup(o->key) : item_lookup(o->key);
        if (o->index < 0) {
            out("r1-mod: stats: unknown %s '%s', override ignored\n", o->kind == OV_FLAG ? "flag" : "weapon",
                o->key);
            o->index = -2; /* reported once */
        }
    }
}

static struct override *ov_find(int kind, const char *key, int index)
{
    int i;
    for (i = 0; i < g_ov_n; i++)
        if (g_ov[i].kind == kind && (kind == OV_RAW ? g_ov[i].index == index : !_stricmp(g_ov[i].key, key)))
            return &g_ov[i];
    return NULL;
}

/* one pass: write every wanted value, restore the rest. Returns values changed, -1 if the buffer is unreadable. */
static int stats_apply(void)
{
    int changed = 0, i;
    EnterCriticalSection(&g_lock);
    resolve_overrides();
    g_pass++;
    __try {
        unsigned char *b = sd_buf();
        if (g_stats_on) {
            changed += sd_put(b, SD_RANK, 4, g_level);
            if (g_experience >= 0)
                changed += sd_put(b, SD_EXPERIENCE, 4, g_experience);
            changed += sd_put(b, SD_PRESTIGE, 4, g_prestige);
            if (g_rank_version >= 0)
                changed += sd_put(b, SD_RANKVERSION, 4, g_rank_version);
            for (i = 0; g_flag_level > 0 && i < g_flag_count; i++) {
                int off = SD_FLAGS_NEW + 2 * g_flag_index[i];
                struct saved *s = saved_find(off);
                int base = s ? s->orig : sd_read(b, off, 2);
                if (base < g_flag_level) /* raise only: a flag already higher keeps its value */
                    changed += sd_put(b, off, 2, g_flag_level);
            }
            for (i = 0; i < g_ov_n; i++) {
                const struct override *o = &g_ov[i];
                if (o->kind == OV_FLAG && o->index > 0)
                    changed += sd_put(b, SD_FLAGS_NEW + 2 * o->index, 2, o->value);
                else if (o->kind == OV_RAW)
                    changed += sd_put(b, o->index, o->size, o->value);
            }
        }
        changed += sd_release_unwanted(b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        changed = -1;
    }
    LeaveCriticalSection(&g_lock);
    return changed;
}

/* weapon XP for an item the inventory has no record of (called from the item info stub) */
static int __cdecl weapon_xp_for(int id)
{
    int i;
    for (i = 0; i < g_ov_n; i++)
        if (g_ov[i].kind == OV_WEAPON && g_ov[i].index == id)
            return g_ov[i].value;
    return g_weapon_xp;
}

static __declspec(naked) void item_owned_tail(void)
{
    __asm {
        or ecx, 0FFFFFFFFh
        mov word ptr [eax + 4], 1
        mov [eax + 20h], ecx
        mov [eax + 24h], ebx
        mov [eax + 8], ecx
        mov [eax + 10h], ecx
        push eax
        push edx
        push dword ptr [eax]
        call weapon_xp_for
        add esp, 4
        mov ecx, eax
        pop edx
        pop eax
        mov [eax + 1Ch], ecx
        pop esi
        pop ebx
        mov esp, ebp
        pop ebp
        ret
    }
}

static int set_weapon_xp_patch(int on)
{
    unsigned char *p = (unsigned char *)A(VA_ITEM_OWNED_TAIL);
    unsigned char jmp[5];
    DWORD old;
    jmp[0] = 0xE9;
    *(int *)(jmp + 1) = (int)((uintptr_t)item_owned_tail - ((uintptr_t)p + 5));
    if (memcmp(p, g_tail_orig, sizeof g_tail_orig) != 0 && p[0] != 0xE9)
        return 0; /* unknown code at this address: wrong build */
    if (!VirtualProtect(p, sizeof g_tail_orig, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    if (on)
        memcpy(p, jmp, sizeof jmp);
    else
        memcpy(p, g_tail_orig, sizeof g_tail_orig);
    VirtualProtect(p, sizeof g_tail_orig, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof g_tail_orig);
    return 1;
}

static void stats_path(char *path, size_t n)
{
    _snprintf(path, n, "%sr1-mod_stats.txt", g_dir);
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int stats_set(const char *key, int v)
{
    if (!strcmp(key, "level"))
        g_level = clampi(v, 1, g_max_level);
    else if (!strcmp(key, "prestige"))
        g_prestige = clampi(v, 0, g_max_prestige);
    else if (!strcmp(key, "flags"))
        g_flag_level = clampi(v, 0, 32767);
    else if (!strcmp(key, "weaponxp"))
        g_weapon_xp = v < 0 ? 0 : v;
    else if (!strcmp(key, "xp"))
        g_experience = v < -1 ? -1 : v;
    else if (!strcmp(key, "enabled"))
        g_stats_on = v != 0;
    else
        return 0;
    return 1;
}

/* add or replace an override; remove = 1 drops it. Returns 0 on bad input. Caller holds g_lock. */
static int ov_set(int kind, const char *key, int size, int value, int remove)
{
    struct override *o;
    int index = -1;
    if (kind == OV_RAW) {
        index = atoi(key);
        if (index < 0 || (size != 1 && size != 2 && size != 4) || index + size > SD_SIZE)
            return 0;
    } else if (!*key || strlen(key) >= KEY_LEN) {
        return 0;
    }
    o = ov_find(kind, key, index);
    if (remove) {
        if (o)
            *o = g_ov[--g_ov_n];
        return 1;
    }
    if (!o) {
        if (g_ov_n == MAX_OVERRIDES)
            return 0;
        o = &g_ov[g_ov_n++];
        o->kind = kind;
        strcpy(o->key, kind == OV_RAW ? "" : key);
        o->index = index;
    }
    o->size = kind == OV_FLAG ? 2 : kind == OV_RAW ? size : 4;
    o->value = kind == OV_FLAG ? clampi(value, 0, 32767) : value;
    if (kind != OV_RAW)
        o->index = -1; /* resolved on the next pass */
    return 1;
}

/* r1-mod_stats.txt: "key value" lines (enabled, level, prestige, xp, flags, weaponxp), plus
 * "flag <name|index> <value>", "weapon <name|item id> <xp>" and "raw <offset> <bytes> <value>" */
static void stats_read_config(void)
{
    char path[MAX_PATH], line[256], key[32], name[KEY_LEN];
    FILE *f;
    int v, size;
    stats_path(path, sizeof path);
    f = fopen(path, "rb");
    if (!f)
        return;
    EnterCriticalSection(&g_lock);
    g_ov_n = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%31s", key) != 1)
            continue;
        if (!strcmp(key, "flag") && sscanf(line, "%*s %63s %d", name, &v) == 2)
            ov_set(OV_FLAG, name, 2, v, 0);
        else if (!strcmp(key, "weapon") && sscanf(line, "%*s %63s %d", name, &v) == 2)
            ov_set(OV_WEAPON, name, 4, v, 0);
        else if (!strcmp(key, "raw") && sscanf(line, "%*s %63s %d %d", name, &size, &v) == 3)
            ov_set(OV_RAW, name, size, v, 0);
        else if (sscanf(line, "%*s %d", &v) == 1)
            stats_set(key, v);
    }
    LeaveCriticalSection(&g_lock);
    fclose(f);
}

static void stats_write_config(void)
{
    char path[MAX_PATH];
    FILE *f;
    int i;
    stats_path(path, sizeof path);
    f = fopen(path, "wb");
    if (!f)
        return;
    EnterCriticalSection(&g_lock);
    fprintf(f, "enabled %d\nlevel %d\nprestige %d\nxp %d\nflags %d\nweaponxp %d\n", (int)g_stats_on, g_level,
            g_prestige, g_experience, g_flag_level, g_weapon_xp);
    for (i = 0; i < g_ov_n; i++) {
        const struct override *o = &g_ov[i];
        if (o->kind == OV_FLAG)
            fprintf(f, "flag %s %d\n", o->key, o->value);
        else if (o->kind == OV_WEAPON)
            fprintf(f, "weapon %s %d\n", o->key, o->value);
        else
            fprintf(f, "raw %d %d %d\n", o->index, o->size, o->value);
    }
    LeaveCriticalSection(&g_lock);
    fclose(f);
}

void stats_startup(void)
{
    char noun[MAX_PATH];
    InitializeCriticalSection(&g_lock);
    _snprintf(noun, sizeof noun, "%sr1-mod_nostats.txt", g_dir);
    if (GetFileAttributesA(noun) == INVALID_FILE_ATTRIBUTES) {
        g_stats_on = 1;
        stats_read_config();
        if (g_stats_on && !set_weapon_xp_patch(1))
            out("r1-mod: item info tail not recognised, weapon levels not raised\n");
    }
}

DWORD WINAPI stats_thread(LPVOID unused)
{
    int i, first = 1;
    (void)unused;
    /* The lobby header asks for the level once, when it is built (Scaleform GetPlayerLevel -> 0x590720, the secure
     * "rank"), right after the profile arrives. Waiting 2 s after Demonware came up and then applying every 2 s let
     * it read the profile's own rank first: the header showed level 1 "Recruit" while everything else had 70
     * (2026-11-11). So the overlay goes in as soon as the profile is there, and is checked every 100 ms for the
     * first 20 s, when the profile can still be replaced by a download, then every 2 s. */
    for (i = 0; i < 2400 && dw_live_state() != 2; i++)
        Sleep(50);
    stats_load_tables();
    stats_read_config(); /* again, now that the table limits are known */
    for (i = 0;; i++) {
        int n = stats_apply();
        if (n > 0 && g_stats_on)
            out("r1-mod: stats %s (level %d, prestige %d, flags >= %d, %d overrides, %d values)\n",
                first ? "applied" : "re-applied", g_level, g_prestige, g_flag_level, g_ov_n, n);
        if (n >= 0)
            first = 0;
        Sleep(i < 200 ? 100 : 2000);
    }
}

/* print the live value at off and, when the overlay changed it, the profile's own value */
static void show_value(const char *what, int off, int size)
{
    struct saved *s;
    int live;
    EnterCriticalSection(&g_lock);
    s = saved_find(off);
    __try {
        live = sd_read(sd_buf(), off, size);
        if (s)
            out("  %s: %d (overlay; profile value %d)\n", what, live, s->orig);
        else
            out("  %s: %d\n", what, live);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out("  %s: secure buffer not readable\n", what);
    }
    LeaveCriticalSection(&g_lock);
}

/* cx_stats find <text>: unlock flags whose name contains text, with their live values */
static void find_flags(const char *text)
{
    const unsigned char *t = find_table("mp/unlockFlags.csv");
    char lower[KEY_LEN], name[KEY_LEN];
    int r, rows, shown = 0;
    if (!t) {
        out("cx_stats: mp/unlockFlags.csv not loaded yet\n");
        return;
    }
    strncpy(lower, text, sizeof lower - 1);
    lower[sizeof lower - 1] = 0;
    _strlwr(lower);
    rows = *(const int *)(t + 8);
    for (r = 0; r < rows && shown < 200; r++) {
        int idx = atoi(table_cell(t, r, 1));
        strncpy(name, table_cell(t, r, 0), sizeof name - 1);
        name[sizeof name - 1] = 0;
        _strlwr(name);
        if (idx <= 0 || idx >= SD_FLAGS_COUNT || !strstr(name, lower))
            continue;
        show_value(table_cell(t, r, 0), SD_FLAGS_NEW + 2 * idx, 2);
        shown++;
    }
    out("%d flag(s)%s\n", shown, shown == 200 ? " (first 200)" : "");
}

static void usage(void)
{
    out("cx_stats [0|1]                       overlay off (restores the profile's values) or on\n"
        "cx_stats level N prestige N xp N flags N weaponxp N   blanket settings (xp -1 leaves XP alone)\n"
        "cx_stats flag <name|index> <value>   one unlock flag (any flag; 'clear' as value removes it)\n"
        "cx_stats weapon <name|item id> <xp>  one weapon's XP ('clear' removes it)\n"
        "cx_stats raw <offset> <1|2|4> <value> any secure field by byte offset ('clear' removes it)\n"
        "cx_stats show [flag X | raw O B]     current settings, or a live value with its profile value\n"
        "cx_stats find <text>                 unlock flags whose name contains text, with values\n"
        "cx_stats clearall                    drop every flag/weapon/raw override\n");
}

static void print_status(void)
{
    int i;
    out("stats overlay: %s, level %d/%d, prestige %d/%d, xp %d, flags >= %d (%d gate flags), weapon xp %d\n",
        g_stats_on ? "on" : "off", g_level, g_max_level, g_prestige, g_max_prestige, g_experience, g_flag_level,
        g_flag_count, g_weapon_xp);
    EnterCriticalSection(&g_lock);
    for (i = 0; i < g_ov_n; i++) {
        const struct override *o = &g_ov[i];
        if (o->kind == OV_FLAG)
            out("  flag %s (index %d) = %d\n", o->key, o->index, o->value);
        else if (o->kind == OV_WEAPON)
            out("  weapon %s (item %d) xp = %d\n", o->key, o->index, o->value);
        else
            out("  raw +%d (%d bytes) = %d\n", o->index, o->size, o->value);
    }
    out("  %d value(s) currently replaced by the overlay; their profile values come back when it no longer "
        "sets them\n", g_saved_n);
    LeaveCriticalSection(&g_lock);
}

static void __cdecl cmd_cx_stats(void)
{
    int i, argc = Cmd_Argc(), changed = 0, ok = 1;
    const char *a1 = argc >= 2 ? Cmd_Argv(1) : "";
    if (!strcmp(a1, "help")) {
        usage();
        return;
    }
    if (!strcmp(a1, "find") && argc >= 3) {
        find_flags(Cmd_Argv(2));
        return;
    }
    if (!strcmp(a1, "show")) {
        if (argc >= 4 && !strcmp(Cmd_Argv(2), "flag")) {
            int idx = flag_lookup(Cmd_Argv(3));
            if (idx < 0)
                out("cx_stats: unknown flag '%s'\n", Cmd_Argv(3));
            else
                show_value(Cmd_Argv(3), SD_FLAGS_NEW + 2 * idx, 2);
        } else if (argc >= 5 && !strcmp(Cmd_Argv(2), "raw")) {
            int off = atoi(Cmd_Argv(3)), size = atoi(Cmd_Argv(4));
            if (off < 0 || (size != 1 && size != 2 && size != 4) || off + size > SD_SIZE)
                out("cx_stats: raw <offset 0..%d> <1|2|4>\n", SD_SIZE - 4);
            else
                show_value(Cmd_Argv(3), off, size);
        } else {
            print_status();
            show_value("rank", SD_RANK, 4);
            show_value("experience", SD_EXPERIENCE, 4);
            show_value("prestige", SD_PRESTIGE, 4);
        }
        return;
    }
    if (argc == 2 && (!strcmp(a1, "0") || !strcmp(a1, "1"))) {
        stats_set("enabled", atoi(a1));
        if (!set_weapon_xp_patch(g_stats_on))
            out("cx_stats: item info tail not recognised, weapon levels unchanged\n");
        changed = 1;
    } else if (!strcmp(a1, "clearall")) {
        EnterCriticalSection(&g_lock);
        g_ov_n = 0;
        LeaveCriticalSection(&g_lock);
        changed = 1;
    } else if (!strcmp(a1, "flag") || !strcmp(a1, "weapon")) {
        int kind = a1[0] == 'f' ? OV_FLAG : OV_WEAPON;
        if (argc < 4) {
            usage();
            return;
        }
        EnterCriticalSection(&g_lock);
        ok = ov_set(kind, Cmd_Argv(2), 0, atoi(Cmd_Argv(3)), !strcmp(Cmd_Argv(3), "clear"));
        LeaveCriticalSection(&g_lock);
        changed = ok;
    } else if (!strcmp(a1, "raw")) {
        int clear = argc >= 4 && !strcmp(Cmd_Argv(argc - 1), "clear");
        if (argc < 5 && !clear) {
            usage();
            return;
        }
        EnterCriticalSection(&g_lock);
        ok = ov_set(OV_RAW, Cmd_Argv(2), clear ? 4 : atoi(Cmd_Argv(3)), clear ? 0 : atoi(Cmd_Argv(4)), clear);
        if (clear && !ok) /* clearing a raw entry: size does not matter */
            ok = ov_set(OV_RAW, Cmd_Argv(2), 1, 0, 1);
        LeaveCriticalSection(&g_lock);
        changed = ok;
    } else if (argc >= 3) {
        for (i = 1; i + 1 < argc; i += 2)
            if (!stats_set(Cmd_Argv(i), atoi(Cmd_Argv(i + 1)))) {
                out("cx_stats: unknown setting '%s' (cx_stats help)\n", Cmd_Argv(i));
                ok = 0;
            }
        changed = 1;
    } else if (argc == 2) {
        usage();
        return;
    }
    if (!ok)
        out("cx_stats: not changed (bad name, offset or size, or too many overrides)\n");
    if (changed) {
        stats_write_config();
        if (stats_apply() < 0)
            out("cx_stats: secure buffer not readable\n");
    }
    print_status();
}

/* ---- read-only view for the overlay's stats panel (render thread: no asset lookups, no engine calls) ---- */
void stats_get_view(struct stats_view *v)
{
    v->on = (int)g_stats_on;
    v->level = g_level;
    v->max_level = g_max_level;
    v->prestige = g_prestige;
    v->max_prestige = g_max_prestige;
    v->xp = g_experience;
    v->flags = g_flag_level;
    v->weapon_xp = g_weapon_xp;
    v->gate_flags = g_flag_count;
    v->overrides = g_ov_n;
    v->replaced = g_saved_n;
    v->tables = (int)g_tables_ready;
}

/* unlock flags whose name contains text (case-insensitive), up to max: live value, the profile's own value when
 * the overlay replaced it (else -1) and the override's value (else -1) */
int stats_find_flags(const char *text, struct stats_flag_row *rows, int max)
{
    char lower[KEY_LEN], name[KEY_LEN];
    int i, n = 0;
    strncpy(lower, text ? text : "", sizeof lower - 1);
    lower[sizeof lower - 1] = 0;
    _strlwr(lower);
    for (i = 0; i < g_all_flag_n && n < max; i++) {
        struct saved *sv;
        int off = SD_FLAGS_NEW + 2 * g_all_flags[i].index, j;
        strcpy(name, g_all_flags[i].name);
        _strlwr(name);
        if (lower[0] && !strstr(name, lower))
            continue;
        strcpy(rows[n].name, g_all_flags[i].name);
        rows[n].index = g_all_flags[i].index;
        __try {
            rows[n].live = sd_read(sd_buf(), off, 2);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            rows[n].live = -1;
        }
        sv = saved_find(off); /* read without the lock: a stale answer only shows for one frame */
        rows[n].profile = sv ? sv->orig : -1;
        rows[n].override = -1;
        for (j = 0; j < g_ov_n; j++)
            if (g_ov[j].kind == OV_FLAG && g_ov[j].index == g_all_flags[i].index)
                rows[n].override = g_ov[j].value;
        n++;
    }
    return n;
}

void stats_register(void)
{
    command_add("cx_stats", cmd_cx_stats);
}
