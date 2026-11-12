/* ---- an English text file over the game's Chinese strings ----
 * The QA client's text is LocalizeEntry assets (type 27: {value, name}) from its Chinese localized_*.ff zones; menus
 * ask for "@KEY", the Scaleform UI for "@SF_UI_...". r1-mod ships no game text. A player can put r1_english.txt in
 * the client mod's folder or next to r1-mod.dll: one string per line,
 *     KEY<TAB>text
 * with \n, \t and \\ as escapes, '#' starting a comment line. Each string found in memory gets the file's text
 * (value pointer swapped to r1-mod's own copy). New zones bring new strings (a map's gametype zones), so the file is
 * applied at start and again on the game thread each time the connection state changes; a string already showing the
 * file's text is left alone.
 *   cx_loc            how many strings the file has and how many are in use
 *   cx_loc reload     read the file again and apply it
 *   cx_loc apply      apply it now
 *   cx_loc <KEY>      the text a string shows now */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"

#define LOC_FILE       "r1_english.txt"
#define VA_CONNSTATE   0x1A2E7ECu

struct loc_entry {
    char *key, *text;
};

static struct loc_entry *g_loc;
static int g_loc_n, g_loc_cap, g_loc_applied;
static char g_loc_path[MAX_PATH];

static void unescape(char *s)
{
    char *r = s, *w = s;
    for (; *r; r++) {
        if (r[0] == '\\' && r[1]) {
            r++;
            *w++ = *r == 'n' ? '\n' : *r == 't' ? '\t' : *r;
            continue;
        }
        *w++ = *r;
    }
    *w = 0;
}

static void loc_free(void)
{
    /* the strings stay allocated: the game may hold pointers to them */
    free(g_loc);
    g_loc = NULL;
    g_loc_n = g_loc_cap = 0;
}

static int loc_load(void)
{
    FILE *f = NULL;
    static char line[8192];
    loc_free();
    if (fsmod_dir()) {
        _snprintf(g_loc_path, sizeof g_loc_path, "%s\\%s", fsmod_dir(), LOC_FILE);
        g_loc_path[sizeof g_loc_path - 1] = 0;
        f = fopen(g_loc_path, "rb");
    }
    if (!f) {
        _snprintf(g_loc_path, sizeof g_loc_path, "%s%s", g_dir, LOC_FILE);
        g_loc_path[sizeof g_loc_path - 1] = 0;
        f = fopen(g_loc_path, "rb");
    }
    if (!f) {
        g_loc_path[0] = 0;
        return 0;
    }
    while (fgets(line, sizeof line, f)) {
        char *tab, *end = line + strlen(line);
        while (end > line && (end[-1] == '\n' || end[-1] == '\r'))
            *--end = 0;
        if (!line[0] || line[0] == '#' || !(tab = strchr(line, '\t')) || tab == line)
            continue;
        *tab = 0;
        unescape(tab + 1);
        if (g_loc_n == g_loc_cap) {
            int cap = g_loc_cap ? g_loc_cap * 2 : 4096;
            struct loc_entry *grown = (struct loc_entry *)realloc(g_loc, cap * sizeof *grown);
            if (!grown)
                break;
            g_loc = grown;
            g_loc_cap = cap;
        }
        g_loc[g_loc_n].key = _strdup(line);
        g_loc[g_loc_n].text = _strdup(tab + 1);
        if (g_loc[g_loc_n].key && g_loc[g_loc_n].text)
            g_loc_n++;
    }
    fclose(f);
    return g_loc_n;
}

/* game thread: every string of the file that is loaded gets its text; returns how many show it */
static int loc_apply(void)
{
    int i, shown = 0;
    for (i = 0; i < g_loc_n; i++) {
        __try {
            const char **slot = localize_slot(g_loc[i].key);
            if (!slot)
                continue;
            if (*slot != g_loc[i].text)
                *slot = g_loc[i].text;
            shown++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    g_loc_applied = shown;
    return shown;
}

static void __cdecl cmd_cx_loc(void)
{
    const char *arg = Cmd_Argc() >= 2 ? Cmd_Argv(1) : "";
    if (!strcmp(arg, "reload")) {
        loc_load();
        out("cx_loc: %d strings from %s, %d in use\n", g_loc_n, g_loc_path[0] ? g_loc_path : "(no " LOC_FILE ")",
            loc_apply());
        return;
    }
    if (!strcmp(arg, "apply")) {
        loc_apply();
        return;
    }
    if (arg[0]) {
        const char **slot = localize_slot(arg);
        __try {
            out("%s = \"%s\"\n", arg, slot && *slot ? *slot : "(not loaded)");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return;
    }
    out("cx_loc: %d strings from %s, %d in use at the last apply\n", g_loc_n,
        g_loc_path[0] ? g_loc_path : "(no " LOC_FILE ")", g_loc_applied);
}

/* timescale guard (50 ms): a changed connection state (a map loading, the lobby again) may have brought new zones */
void localize_keep(void)
{
    static int last = -1;
    static DWORD again;
    int state;
    if (!g_loc_n)
        return;
    __try {
        state = *(volatile int *)A(VA_CONNSTATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (state != last) {
        last = state;
        again = GetTickCount() + 1500; /* once the zones of the new state are in */
        Cbuf_AddText("cx_loc apply\n");
    } else if (again && (LONG)(GetTickCount() - again) >= 0) {
        again = 0;
        Cbuf_AddText("cx_loc apply\n");
    }
}

void localize_register(void)
{
    command_add("cx_loc", cmd_cx_loc);
    if (loc_load())
        out("r1-mod: %d English strings from %s\n", g_loc_n, g_loc_path);
}
