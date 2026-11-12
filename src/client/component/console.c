/* ---- console ----
 * Output goes to r1-mod.log, the console window and the overlay console. Lines come from the console window
 * (stdin), from r1-mod_in.txt next to the DLL (only lines appended while the game runs) and from the overlay. */
#include "std_include.h"
#include <intrin.h>
#include "game/game.h"
#include "utils/hook.h"
#include "utils/memory.h"
#include "component/components.h"
#include "component/overlay.h"

static FILE *g_log;
static CRITICAL_SECTION g_out;

void out(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = 0;
    EnterCriticalSection(&g_out);
    if (g_log) {
        fputs(buf, g_log);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_out);
    overlay_print(buf);
    /* outside g_out: a console paused in selection mode ("Select r1-mod") blocks every write,
     * which used to hold g_out and stall every thread that logs, timescale_guard included */
    fputs(buf, stdout);
    fflush(stdout);
}

/* g_dir, r1-mod.log and the output lock; first thing main_thread does */
void console_open_log(void)
{
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, g_dir, sizeof g_dir);
    {
        char *slash = strrchr(g_dir, '\\');
        if (slash)
            slash[1] = 0;
    }
    _snprintf(path, sizeof path, "%sr1-mod.log", g_dir);
    g_log = fopen(path, "ab");
    InitializeCriticalSection(&g_out);
}

void console_open_window(void)
{
    AllocConsole();
    SetConsoleTitleA("r1-mod " R1_VERSION);
    freopen("CONOUT$", "w", stdout);
    freopen("CONIN$", "r", stdin);
    {
        /* no QuickEdit: a stray click put the console in selection mode, which blocks output */
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode;
        if (GetConsoleMode(in, &mode))
            SetConsoleMode(in, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
    }
}

/* name and "value (type)" of a dvar; 0 if it cannot be read */
static int format_dvar(unsigned char *d, char *nm, size_t nn, char *val, size_t vn)
{
    char sv[256];
    __try {
        unsigned char type = d[8];
        if (!safe_str(*(const void **)d, nm, nn))
            return 0;
        switch (type) {
        case 0: _snprintf(val, vn, "%u (bool)", d[12]); break;
        case 1: _snprintf(val, vn, "%g (float)", *(float *)(d + 12)); break;
        case 5: _snprintf(val, vn, "%d (int)", *(int *)(d + 12)); break;
        case 6: _snprintf(val, vn, "%d (enum)", *(int *)(d + 12)); break;
        case 7:
            if (safe_str(*(const void **)(d + 12), sv, sizeof sv))
                _snprintf(val, vn, "\"%s\" (string)", sv);
            else
                _snprintf(val, vn, "<unreadable string>");
            break;
        default: _snprintf(val, vn, "0x%08x (type %u)", *(unsigned *)(d + 12), type); break;
        }
        val[vn - 1] = 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void print_dvar(const char *name)
{
    unsigned char *d = Dvar_FindVar(name);
    char nm[128], val[320];
    if (!d) {
        out("dvar '%s' not found\n", name);
        return;
    }
    if (format_dvar(d, nm, sizeof nm, val, sizeof val))
        out("%s = %s\n", nm, val);
    else
        out("dvar '%s': access fault\n", name);
}

static void list_commands(const char *filter)
{
    unsigned *n = *(unsigned **)A(g_engine->cmd_head);
    int count = 0;
    while (n && count < 4096) {
        const char *nm = (const char *)n[1];
        char tmp[96];
        if (nm && safe_str(nm, tmp, sizeof tmp) && (!filter || strstr(tmp, filter)))
            out("  %s\n", tmp);
        n = (unsigned *)n[0];
        count++;
    }
    out("%d commands\n", count);
}

static void __cdecl cmd_r1mod(void)
{
    out("r1-mod: %s, module base 0x%08x, delta 0x%x\n", g_engine->label, (unsigned)(A(IDA_BASE)),
        (unsigned)g_delta);
}

static void __cdecl cmd_cx_dvar(void)
{
    if (Cmd_Argc() < 2) {
        out("usage: cx_dvar <name>\n");
        return;
    }
    print_dvar(Cmd_Argv(1));
}

void console_register(void)
{
    command_add("r1mod", cmd_r1mod);
    command_add("cx_dvar", cmd_cx_dvar);
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = 0;
}

/* echo: print "> line" for text sent to the game (the overlay console echoes on its own) */
void console_run_line(char *line, int echo)
{
    trim(line);
    if (!*line)
        return;
    if (!strncmp(line, "dvar ", 5)) {
        print_dvar(line + 5);
    } else if (!strncmp(line, "cmdlist", 7)) {
        list_commands(line[7] == ' ' ? line + 8 : NULL);
    } else if (!strcmp(line, "help")) {
        out("r1-mod console: 'dvar <name>', 'cmdlist [filter]', anything else goes to the game command buffer\n");
        if (g_engine->build == BUILD_QA_3333)
            out("in game: ` opens the console (Shift+` taller, Tab completes), F6 or cx_browser the server browser\n");
        else
            out("on %s only this window and r1-mod_in.txt take lines; '<dvar> <value>' sets a dvar\n",
                g_engine->label);
    } else {
        /* the engine buffers text until a newline; without one, queued lines run together */
        char text[1100];
        _snprintf(text, sizeof text - 1, "%s\n", line);
        text[sizeof text - 1] = 0;
        Cbuf_AddText(text);
        if (echo)
            out("> %s\n", line);
    }
}

DWORD WINAPI console_input_thread(LPVOID unused)
{
    char line[1024];
    (void)unused;
    while (fgets(line, sizeof line, stdin))
        console_run_line(line, 1);
    return 0;
}

DWORD WINAPI console_file_thread(LPVOID unused)
{
    char path[MAX_PATH];
    long offset = 0;
    (void)unused;
    _snprintf(path, sizeof path, "%sr1-mod_in.txt", g_dir);
    {
        /* only lines appended from now on: what earlier sessions left is not run again */
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            offset = ftell(f);
            fclose(f);
        }
    }
    for (;;) {
        FILE *f = fopen(path, "rb");
        if (f) {
            long size;
            fseek(f, 0, SEEK_END);
            size = ftell(f);
            if (size < offset)
                offset = 0;
            if (size > offset) {
                char *buf = (char *)malloc(size - offset + 1);
                fseek(f, offset, SEEK_SET);
                size = (long)fread(buf, 1, size - offset, f);
                buf[size] = 0;
                offset += size;
                {
                    char *p = strtok(buf, "\n");
                    while (p) {
                        console_run_line(p, 1);
                        p = strtok(NULL, "\n");
                    }
                }
                free(buf);
            }
            fclose(f);
        }
        Sleep(200);
    }
}

/* ---- interface for the overlay (overlay.h) ---- */
uintptr_t r1_addr(unsigned ida_va)
{
    return A(ida_va);
}

void r1_log(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = 0;
    out("%s", buf);
}

void r1_exec(const char *line)
{
    char tmp[1024];
    strncpy(tmp, line, sizeof tmp - 1);
    tmp[sizeof tmp - 1] = 0;
    console_run_line(tmp, 0);
}

const char *r1_dir(void)
{
    return g_dir;
}

int r1_complete(const char *prefix, int max, void (*cb)(const char *, const char *, void *), void *ctx)
{
    size_t n = strlen(prefix);
    int found = 0, count = 0, b;
    unsigned *node = *(unsigned **)A(g_engine->cmd_head);
    char nm[128], val[320];
    while (node && count++ < 4096 && found < max) {
        if (safe_str((const void *)node[1], nm, sizeof nm) && !_strnicmp(nm, prefix, n)) {
            cb(nm, NULL, ctx);
            found++;
        }
        node = (unsigned *)node[0];
    }
    for (b = 0; b < 1024 && found < max; b++) {
        unsigned char *d = ((unsigned char **)A(g_engine->dvar_hash))[b];
        for (count = 0; d && count < 256 && found < max; count++) {
            if (format_dvar(d, nm, sizeof nm, val, sizeof val) && !_strnicmp(nm, prefix, n)) {
                cb(nm, val, ctx);
                found++;
            }
            if (!safe_ptr(d + 0x48, (void **)&d))
                break;
        }
    }
    return found;
}

/* ---- game message windows mirrored into the overlay console ----
 * The QA build has no Com_Printf; what remains of console text goes through 0x45B070,
 * which prints into a con_gameMsgWindow: chat, obituaries and server prints. Its 7 callers are
 * hooked at the call site (cdecl, 6 arguments, the text is the third). */
#define VA_GAMEMSG 0x45B070u
static const unsigned g_gamemsg_sites[] = {0x442ED5u, 0x442F25u, 0x442FA4u, 0x45B523u, 0x45B7A2u, 0x4F982Eu, 0x52561Au};
typedef char(__cdecl *GameMsg_t)(int, int, const char *, int, int, int);
static void *g_orig_gamemsg;

/* A line the game sends again and again is shown once and then counted: "(repeated N more times)" follows when
 * another line arrives, and every 500 repeats r1-mod.log names the call site, message window and stack. Seen
 * 2026-11-03 on mp_rust_long: the announcer subtitle "我们上吧" (UK_1mc_boost) ~950 times a second, because the
 * native server sent an entity loop sound as a C1 sound slot, which QA restarts every frame (fixed in its
 * sv_snapshot.c). Messages arrive on the game thread only. */
#define GAMEMSG_REPORT_EVERY 500
static char g_last_msg[1024];
static unsigned g_last_repeats;

static char __cdecl gamemsg_hook(int local, int window, const char *text, int a4, int a5, int a6)
{
    char buf[1024];
    if (text && safe_str(text, buf, sizeof buf - 1) && buf[0] && strcmp(buf, "\n")) {
        size_t len = strlen(buf);
        if (buf[len - 1] != '\n') {
            buf[len] = '\n';
            buf[len + 1] = 0;
        }
        if (!strcmp(buf, g_last_msg)) {
            if (++g_last_repeats % GAMEMSG_REPORT_EVERY == 0) {
                void *frames[12];
                char chain[160] = "";
                USHORT i, n = RtlCaptureStackBackTrace(1, 12, frames, NULL);
                for (i = 0; i < n && strlen(chain) < sizeof chain - 12; i++)
                    _snprintf(chain + strlen(chain), sizeof chain - strlen(chain) - 1, " %x",
                              (unsigned)((uintptr_t)frames[i] - g_delta));
                out("r1-mod: game message repeated %u times (window %d, called from ida 0x%08x; stack%s): %s",
                    g_last_repeats, window, (unsigned)((uintptr_t)_ReturnAddress() - 5 - g_delta), chain, buf);
            }
        } else {
            if (g_last_repeats) {
                char note[64];
                _snprintf(note, sizeof note, "  (repeated %u more times)\n", g_last_repeats);
                note[sizeof note - 1] = 0;
                overlay_print(note);
            }
            memcpy(g_last_msg, buf, sizeof g_last_msg);
            g_last_repeats = 0;
            overlay_print(buf);
        }
    }
    return ((GameMsg_t)g_orig_gamemsg)(local, window, text, a4, a5, a6);
}

int console_gamemsg_site_count(void)
{
    return (int)(sizeof g_gamemsg_sites / sizeof g_gamemsg_sites[0]);
}

int console_install_gamemsg_hooks(void)
{
    int i, ok = 0;
    for (i = 0; i < console_gamemsg_site_count(); i++)
        ok += patch_call(g_gamemsg_sites[i], VA_GAMEMSG, (void *)gamemsg_hook, &g_orig_gamemsg);
    return ok;
}
