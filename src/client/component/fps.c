/* ---- frame rate cap ----
 * The frame limiter (0x59D840) reads dvar "1848" (com_maxfps; pointer at 0x2E8CFE8, registered 0x59BA80 as an int,
 * default 85, range 0..100, 0 = no cap; value at +0x0C, latched +0x1C, range +0x3C/+0x40). While connecting
 * (connection state neither 0 nor 10) it holds the cap at 30 or below. cx_fps raises the range to FPS_LIMIT
 * and keeps the chosen value, rewriting the dvar when something else changes it; [r1-mod] max_fps= in
 * r1-mod.ini applies it at start (unset: the game's own value is left alone). */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"
#include "component/overlay.h"

#define VA_MAXFPS_DVAR 0x2E8CFE8u
#define FPS_LIMIT 1000
static volatile LONG g_fps = -1;

static int read_fps(int *value, int *max)
{
    unsigned char *d = *(unsigned char **)A(VA_MAXFPS_DVAR);
    if (!d)
        return 0;
    __try {
        *value = *(int *)(d + 0x0C);
        *max = *(int *)(d + 0x40);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void apply_fps(int fps)
{
    unsigned char *d = *(unsigned char **)A(VA_MAXFPS_DVAR);
    if (!d)
        return;
    __try {
        if (*(int *)(d + 0x40) < FPS_LIMIT)
            *(int *)(d + 0x40) = FPS_LIMIT;
        if (*(int *)(d + 0x0C) != fps) {
            *(int *)(d + 0x0C) = fps;
            *(int *)(d + 0x1C) = fps;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

/* called by the timescale guard every 50 ms */
void fps_keep(void)
{
    if (g_fps >= 0)
        apply_fps(g_fps);
}

void fps_load_config(void)
{
    char ini[MAX_PATH];
    _snprintf(ini, sizeof ini, "%sr1-mod.ini", g_dir);
    g_fps = GetPrivateProfileIntA("r1-mod", "max_fps", -1, ini);
    if (g_fps > FPS_LIMIT)
        g_fps = FPS_LIMIT;
    if (g_fps >= 0)
        out("r1-mod: max fps %ld from r1-mod.ini\n", (long)g_fps);
}

/* cx_fps [n|off]   show or set the frame rate cap (0 = none, up to FPS_LIMIT); off hands it back to the game */
static void __cdecl cmd_cx_fps(void)
{
    char ini[MAX_PATH], text[16];
    int value, max;
    _snprintf(ini, sizeof ini, "%sr1-mod.ini", g_dir);
    if (Cmd_Argc() >= 2) {
        const char *arg = Cmd_Argv(1);
        if (!_stricmp(arg, "off")) {
            g_fps = -1;
            WritePrivateProfileStringA("r1-mod", "max_fps", NULL, ini);
        } else {
            int fps = atoi(arg);
            if (fps < 0)
                fps = 0;
            if (fps > FPS_LIMIT)
                fps = FPS_LIMIT;
            g_fps = fps;
            apply_fps(fps);
            _snprintf(text, sizeof text, "%d", fps);
            WritePrivateProfileStringA("r1-mod", "max_fps", text, ini);
        }
    }
    if (read_fps(&value, &max))
        out("max fps %d%s (dvar 1848, range 0..%d)%s\n", value, value == 0 ? " (no cap)" : "", max,
            g_fps >= 0 ? ", kept by r1-mod and saved to r1-mod.ini" : ", the game's own setting");
    else
        out("max fps dvar 1848 is not registered yet\n");
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* cx_frametime [reset]   drawn-frame times over the last 4096 frames (or since reset): average, median, 99th
 * percentile, worst, frames over 2x the median (hitches), the frame rate and the connection state (10 in a match).
 * Measured where the overlay draws (once per frame, before EndScene), so it is the whole frame, game and render. */
static void __cdecl cmd_cx_frametime(void)
{
    static float ms[4096];
    double sum = 0;
    int n, i, hitches = 0, state = -1;
    if (Cmd_Argc() >= 2 && !_stricmp(Cmd_Argv(1), "reset")) {
        overlay_frame_times_reset();
        out("cx_frametime: samples cleared\n");
        return;
    }
    n = overlay_frame_times(ms, 4096);
    if (n < 10) {
        out("cx_frametime: %d frames so far\n", n);
        return;
    }
    for (i = 0; i < n; i++)
        sum += ms[i];
    qsort(ms, (size_t)n, sizeof ms[0], cmp_float);
    for (i = 0; i < n; i++)
        if (ms[i] > 2.0f * ms[n / 2])
            hitches++;
    __try {
        state = *(volatile int *)A(0x1A2E7ECu);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    out("cx_frametime: %d frames (connection state %d): avg %.2f ms (%.0f fps), median %.2f, p99 %.2f, worst %.2f, "
        "%d over 2x median\n",
        n, state, sum / n, 1000.0 * n / sum, ms[n / 2], ms[(n * 99) / 100], ms[n - 1],
        hitches);
}

void fps_register(void)
{
    command_add("cx_fps", cmd_cx_fps);
    command_add("cx_frametime", cmd_cx_frametime);
}
