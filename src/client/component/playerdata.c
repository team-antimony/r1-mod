/* ---- persistent playerdata (classes, tutorial flags) ----
 * The lobby leaves the playerdata buffer blank (the local backend serves no profile), so every class slot is empty.
 * The buffer is a 32,764-byte block at the Demonware stats object + 144 (0xC95BD80 in IDA terms; the engine
 * hands it out in sub_45E560). This thread fills it from r1-mod_playerdata.bin (or the seed data\qa_playerdata.bin),
 * reloads it if the engine blanks it, and writes it back whenever the user changes something, so edits survive
 * restarts. */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"

#define PD_OFFSET    144
#define PD_SIZE      32764

/* Class and character ints in the buffer are stored as key ^ rotr(value, 8) while the flag byte is set (writer
 * sub_592B10, readers sub_689E00/sub_6D90C0 decode rol(stored ^ key, 8)). Key and flag live in the same stats object,
 * outside the buffer (+37156, +37160, i.e. 0xC964EA4/0xC964EA8), and the key is new every launch. A buffer saved
 * without its key decodes to garbage item ids in the next session: the game crashed on them in the class item
 * lookup (0x709603, 0x64BB12) or put default classes back. So the file carries the key and flag after the buffer
 * (PD_TRAILER: magic, key, flag) and both are put back whenever the file is applied. */
#define PD_KEY_OFFSET  37156
#define PD_FLAG_OFFSET 37160
#define PD_MAGIC       0x4B503152u /* "R1PK" */
#define PD_FILE_SIZE   (PD_SIZE + 12)

static unsigned *pd_key_ptr(void)
{
    return (unsigned *)((unsigned char *)A(VA_PD_OBJECT) + PD_KEY_OFFSET);
}

static unsigned char *pd_flag_ptr(void)
{
    return (unsigned char *)A(VA_PD_OBJECT) + PD_FLAG_OFFSET;
}

/* Skipping the intro reached the first-run operator screen before the onboarding thread had run, and that screen
 * writes a fresh profile's default classes into the buffer; the thread below saw the change and saved it over the
 * user's file, so every loadout was reset. Nothing is loaded or saved until the profile is in place:
 * - pd_profile_ready: called by the onboarding check hook (onboarding.c) on the engine's thread, at the moment the
 *   engine decides on onboarding, i.e. after its own profile fetch and before any UI uses the buffer. The file is
 *   applied there, synchronously, so the engine sees the user's profile and never opens the operator screen.
 * - pd_release: the fallback when that hook is unavailable (onboarding_thread, after the dvar 3233 skip); the file is
 *   applied again then, over whatever the operator screen wrote. */
static volatile LONG g_pd_hold = 1;
static volatile LONG g_pd_reload;
static volatile LONG g_pd_loaded;
static int pd_load(const char *why, int game_thread);

/* The engine's profile validation (sub_6985E0, run by sub_5E2630 once the lobby is ready) treats a profile whose
 * loadoutVersion is below dvar "3294" (3) as an old class layout: once onboarding is complete it puts the default
 * classes in every slot (sub_698030), then sets loadoutVersion to 3. The seed and every saved file carry 0, and the
 * profile now has onboard_complete, so a validation pass after the file is applied would reset all classes. The
 * version is brought up to date whenever the file is applied. loadoutVersion is an int at +13318 (offsets[803] of
 * mp/playerdata.def; ints are read at the table's byte offset, sub_62FD90). */
#define PD_LOADOUT_VERSION_OFFSET 13318

static int pd_current_loadout_version(void)
{
    unsigned char *dvar = Dvar_FindVar("3294");
    return dvar && dvar[8] == 5 ? *(int *)(dvar + 12) : 3;
}

/* The engine keeps the classes in caches as well (character classes at 0x2E16E54, filled from playerdata by
 * sub_593700; weapon classes at 0x2E88F1C, filled by sub_593B20) and writes them back over playerdata with sub_6D90C0
 * (all 120 weapon-class entries and the character slots) on connect (CL_ConnectToHost, CL_CheckForResend via the
 * profile sanitizer sub_6D8A70), on gamestate and on some server commands. The caches are filled before r1-mod applies
 * the file, so they held the engine's default classes, and the first StartGame wrote those over every class (found
 * with a hardware write breakpoint on class 0's primary, 2026-11-03). So the caches are reloaded from the applied file:
 * directly when on the game thread (pd_profile_ready, from the onboarding check), otherwise by asking sub_5E2630 for
 * its state 9, which runs the same two functions on the game thread and returns to 4 (the engine uses it after a
 * disconnect; only taken from the idle state 4). */
#define VA_PROFILE_STATE      0x783F864u
#define VA_LOAD_CHARACTER_CACHE 0x593700u
#define VA_LOAD_CLASS_CACHE   0x593B20u

static const char *pd_reload_class_caches(int game_thread)
{
    LONG state;
    __try {
        if (game_thread) {
            ((void(__cdecl *)(void))A(VA_LOAD_CHARACTER_CACHE))();
            ((void(__cdecl *)(void))A(VA_LOAD_CLASS_CACHE))();
            return ", class caches reloaded";
        }
        state = InterlockedCompareExchange((volatile LONG *)A(VA_PROFILE_STATE), 9, 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return ", class caches not reachable";
    }
    return state == 4 ? ", class caches reload requested" : ", class caches NOT reloaded (profile state busy)";
}

void pd_release(void)
{
    InterlockedExchange(&g_pd_reload, 1);
    InterlockedExchange(&g_pd_hold, 0);
}

void pd_profile_ready(void)
{
    if (pd_load("profile ready", 1))
        InterlockedExchange(&g_pd_loaded, 1);
    InterlockedExchange(&g_pd_hold, 0);
}

unsigned char *pd_buf(void)
{
    return (unsigned char *)A(VA_PD_OBJECT) + PD_OFFSET;
}

int pd_get_int(int off)
{
    unsigned v = *(unsigned *)(pd_buf() + off);
    if (*pd_flag_ptr())
        v ^= *pd_key_ptr(), v = (v << 8) | (v >> 24);
    return (int)v;
}

void pd_set_int(int off, int value)
{
    unsigned v = (unsigned)value;
    if (*pd_flag_ptr())
        v = *pd_key_ptr() ^ ((v >> 8) | (v << 24));
    *(unsigned *)(pd_buf() + off) = v;
}

static int pd_blank(const unsigned char *p)
{
    int i;
    for (i = 0; i < PD_SIZE; i++)
        if (p[i])
            return 0;
    return 1;
}

static int read_file(const char *path, unsigned char *dst, int max)
{
    FILE *f = fopen(path, "rb");
    int n;
    if (!f)
        return 0;
    n = (int)fread(dst, 1, max, f);
    fclose(f);
    return n;
}

/* The seed is data\qa_playerdata.bin in the repository. The DLL runs from build\bin\<platform>\<config>\, so look
 * in the DLL folder and up to five folders above it; the first match wins. */
static void pd_paths(char *user, char *seed, size_t n)
{
    char up[64] = "";
    int level;
    _snprintf(user, n, "%sr1-mod_playerdata.bin", g_dir);
    for (level = 0; level <= 5; level++) {
        _snprintf(seed, n, "%s%sdata\\qa_playerdata.bin", g_dir, up);
        if (GetFileAttributesA(seed) != INVALID_FILE_ATTRIBUTES)
            return;
        strcat(up, "..\\");
    }
    _snprintf(seed, n, "%s..\\data\\qa_playerdata.bin", g_dir);
}

/* the buffer plus the key and flag it is encoded with */
static int pd_save(const char *path, const unsigned char *buf, unsigned key, unsigned flag)
{
    unsigned trailer[3];
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    trailer[0] = PD_MAGIC;
    trailer[1] = key;
    trailer[2] = flag;
    fwrite(buf, 1, PD_SIZE, f);
    fwrite(trailer, 1, sizeof trailer, f);
    fclose(f);
    return 1;
}

static int pd_load(const char *why, int game_thread)
{
    char user[MAX_PATH], seed[MAX_PATH];
    unsigned char *tmp = (unsigned char *)calloc(1, PD_FILE_SIZE);
    unsigned *trailer = (unsigned *)(tmp + PD_SIZE);
    const char *keyed = "", *versioned = "", *cached = "";
    int n, seeded = 0;
    pd_paths(user, seed, sizeof user);
    n = read_file(user, tmp, PD_FILE_SIZE);
    if (n <= 0) {
        n = read_file(seed, tmp, PD_FILE_SIZE);
        seeded = n > 0;
    }
    if (n > 0) {
        __try {
            memcpy(pd_buf(), tmp, PD_SIZE);
            if (n >= PD_FILE_SIZE && trailer[0] == PD_MAGIC) {
                /* the key the file was saved under, so its encoded classes stay valid */
                *pd_key_ptr() = trailer[1];
                *pd_flag_ptr() = (unsigned char)trailer[2];
                keyed = ", key restored";
            } else if (seeded) {
                /* the seed holds plain values */
                *pd_flag_ptr() = 0;
                keyed = ", plain seed";
            } else {
                keyed = ", no key in the file (saved by an older r1-mod): encoded classes may not survive";
            }
            {
                int *version = (int *)(pd_buf() + PD_LOADOUT_VERSION_OFFSET), want = pd_current_loadout_version();
                if (*version < want) {
                    *version = want;
                    versioned = ", loadoutVersion brought up to date";
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            n = 0;
        }
        if (n > 0)
            cached = pd_reload_class_caches(game_thread);
    }
    free(tmp);
    out("r1-mod: playerdata %s (%s%s%s%s)\n", n > 0 ? "loaded" : "not loaded", why, n > 0 ? keyed : "",
        n > 0 ? versioned : "", cached);
    return n > 0;
}

/* ---- playerdata backups ----
 * Each launch copies r1-mod_playerdata.bin to playerdata_backups\ before the session can change it: after a crash
 * while loading a match, the game put default classes in every slot and r1-mod saved that over the only copy (the
 * loadoutVersion reset described above).
 * The newest PD_BACKUPS are kept (names sort by time); cx_playerdata list / restore [n] brings one back. */
#define PD_BACKUPS 20
#define PD_BACKUP_NAME 48

static void pd_backup_dir(char *dir, size_t n)
{
    _snprintf(dir, n, "%splayerdata_backups", g_dir);
}

static int pd_name_cmp(const void *a, const void *b)
{
    return -strcmp((const char *)a, (const char *)b); /* newest first */
}

/* backup file names, newest first; returns how many */
static int pd_list_backups(char (*names)[PD_BACKUP_NAME], int max)
{
    char dir[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;
    pd_backup_dir(dir, sizeof dir);
    _snprintf(pattern, sizeof pattern, "%s\\r1-mod_playerdata-*.bin", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (n < max && strlen(fd.cFileName) < PD_BACKUP_NAME)
            strcpy(names[n++], fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, n, PD_BACKUP_NAME, pd_name_cmp);
    return n;
}

static void pd_backup(const char *user)
{
    static char names[256][PD_BACKUP_NAME];
    char dir[MAX_PATH], path[MAX_PATH];
    SYSTEMTIME t;
    int n, i;
    if (GetFileAttributesA(user) == INVALID_FILE_ATTRIBUTES)
        return;
    pd_backup_dir(dir, sizeof dir);
    CreateDirectoryA(dir, NULL);
    GetLocalTime(&t);
    _snprintf(path, sizeof path, "%s\\r1-mod_playerdata-%04u%02u%02u-%02u%02u%02u.bin", dir, t.wYear, t.wMonth,
              t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (!CopyFileA(user, path, FALSE)) {
        out("r1-mod: playerdata backup to %s failed (%lu)\n", path, GetLastError());
        return;
    }
    n = pd_list_backups(names, 256);
    for (i = PD_BACKUPS; i < n; i++) {
        _snprintf(path, sizeof path, "%s\\%s", dir, names[i]);
        DeleteFileA(path);
    }
    out("r1-mod: playerdata backed up (%s)\n", names[0]);
}

DWORD WINAPI playerdata_thread(LPVOID unused)
{
    char user[MAX_PATH], seed[MAX_PATH];
    unsigned char *last = (unsigned char *)calloc(1, PD_SIZE);
    unsigned char *cur = (unsigned char *)calloc(1, PD_SIZE);
    unsigned key = 0, flag = 0, last_key = 0, last_flag = 0;
    int i, have = 0;
    (void)unused;
    pd_paths(user, seed, sizeof user);
    pd_backup(user);
    /* wait for pd_profile_ready or pd_release (both come after Demonware is live) */
    while (g_pd_hold)
        Sleep(250);
    /* after pd_profile_ready the buffer holds the file plus whatever the onboarding check added; the first pass
     * below saves that (last is blank) */
    have = g_pd_loaded != 0;
    for (;;) {
        int ok = 1;
        __try {
            memcpy(cur, pd_buf(), PD_SIZE);
            key = *pd_key_ptr();
            flag = *pd_flag_ptr();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = 0;
        }
        if (ok && InterlockedExchange(&g_pd_reload, 0)) {
            if (pd_load("after onboarding", 0)) {
                memcpy(last, pd_buf(), PD_SIZE);
                last_key = *pd_key_ptr();
                last_flag = *pd_flag_ptr();
                have = 1;
            }
        } else if (ok) {
            if (pd_blank(cur)) {
                if (pd_load(have ? "engine blanked it" : "startup", 0)) {
                    memcpy(last, pd_buf(), PD_SIZE);
                    last_key = *pd_key_ptr();
                    last_flag = *pd_flag_ptr();
                    have = 1;
                }
            } else if (memcmp(cur, last, PD_SIZE) != 0 || key != last_key || flag != last_flag) {
                /* a new key alone also needs saving, or the file's values no longer match it */
                if (have && pd_save(user, cur, key, flag)) {
                    char where[32] = "";
                    int first = -1, changed = 0;
                    for (i = 0; i < PD_SIZE; i++)
                        if (cur[i] != last[i] && (changed++, first < 0))
                            first = i;
                    if (first >= 0)
                        _snprintf(where, sizeof where - 1, ", first at +%d", first);
                    out("r1-mod: playerdata saved (%d bytes changed%s%s)\n", changed, where,
                        key != last_key ? ", new key" : "");
                }
                memcpy(last, cur, PD_SIZE);
                last_key = key;
                last_flag = flag;
                have = 1;
            }
        }
        Sleep(4000);
    }
}

/* cx_playerdata [load|reset|list|restore [n]]   load re-applies the saved file; reset drops it and reloads the seed;
 * list shows the backups (1 = newest); restore n puts backup n back (the current file is backed up first) */
static void __cdecl cmd_cx_playerdata(void)
{
    static char names[256][PD_BACKUP_NAME];
    char user[MAX_PATH], seed[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    const char *sub = Cmd_Argc() >= 2 ? Cmd_Argv(1) : "load";
    int n, i;
    pd_paths(user, seed, sizeof user);
    pd_backup_dir(dir, sizeof dir);
    if (!strcmp(sub, "list") || !strcmp(sub, "restore")) {
        n = pd_list_backups(names, 256);
        if (!strcmp(sub, "list") || n == 0) {
            for (i = 0; i < n; i++)
                out("  %2d  %s\n", i + 1, names[i]);
            out("%d playerdata backup%s in %s\n", n, n == 1 ? "" : "s", dir);
            return;
        }
        i = Cmd_Argc() >= 3 ? atoi(Cmd_Argv(2)) : 1;
        if (i < 1 || i > n) {
            out("cx_playerdata restore: pick 1..%d (cx_playerdata list)\n", n);
            return;
        }
        _snprintf(path, sizeof path, "%s\\%s", dir, names[i - 1]);
        pd_backup(user);
        if (!CopyFileA(path, user, FALSE)) {
            out("cx_playerdata restore: copying %s failed (%lu)\n", path, GetLastError());
            return;
        }
        out("r1-mod: playerdata restored from %s\n", names[i - 1]);
        pd_load("restore", 0);
        return;
    }
    if (!strcmp(sub, "reset")) {
        pd_backup(user);
        DeleteFileA(user);
    }
    pd_load("command", 0);
}

void playerdata_register(void)
{
    command_add("cx_playerdata", cmd_cx_playerdata);
}
