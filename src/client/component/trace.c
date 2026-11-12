/* ---- runtime trace of Com_sprintf output ----
 * Com_sprintf (IDA 0x637BE0) is a thin _vsnprintf wrapper called from ~500 places, so what it
 * formats is a good window into what the engine is doing (paths, asset names, commands). The hook
 * reimplements it exactly (49 bytes, no one jumps into the middle) and logs results that contain
 * the substring set by `cx_trace`. Off by default; ~hundreds of calls per second when on. */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"

#define VA_COM_SPRINTF 0x637BE0u

static char g_trace[96];
static volatile LONG g_trace_on;
static volatile LONG g_trace_hits;

static int __cdecl sprintf_hook(char *buf, int count, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = _vsnprintf(buf, (size_t)count, fmt, ap);
    va_end(ap);
    if (count > 0)
        buf[count - 1] = 0;
    if (g_trace_on && count > 0 && (!g_trace[0] || strstr(buf, g_trace))) {
        char line[240];
        strncpy(line, buf, sizeof line - 1);
        line[sizeof line - 1] = 0;
        InterlockedIncrement(&g_trace_hits);
        out("[sprintf t%lu] %s\n", GetCurrentThreadId(), line);
    }
    return r >= count ? -1 : r;
}

int trace_install(void)
{
    static const unsigned char sig[6] = {0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x10};
    unsigned char *p = (unsigned char *)A(VA_COM_SPRINTF);
    DWORD old;
    if (memcmp(p, sig, sizeof sig) != 0)
        return 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    p[0] = 0xE9;
    *(int *)(p + 1) = (int)((uintptr_t)sprintf_hook - ((uintptr_t)p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

/* cx_trace            -> status
 * cx_trace <text>     -> log Com_sprintf results containing <text>
 * cx_trace *          -> log everything
 * cx_trace off        -> stop */
static void __cdecl cmd_cx_trace(void)
{
    if (Cmd_Argc() < 2) {
        out("trace: %s, filter \"%s\", %ld lines logged\n", g_trace_on ? "on" : "off", g_trace,
            (long)g_trace_hits);
        return;
    }
    if (!strcmp(Cmd_Argv(1), "off")) {
        g_trace_on = 0;
        out("trace off\n");
        return;
    }
    if (!strcmp(Cmd_Argv(1), "*"))
        g_trace[0] = 0;
    else {
        strncpy(g_trace, Cmd_Argv(1), sizeof g_trace - 1);
        g_trace[sizeof g_trace - 1] = 0;
    }
    g_trace_hits = 0;
    g_trace_on = 1;
    out("trace on, filter \"%s\"\n", g_trace);
}

void trace_register(void)
{
    command_add("cx_trace", cmd_cx_trace);
}
