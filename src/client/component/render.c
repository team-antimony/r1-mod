/* ---- on-screen version string ----
 * The per-frame 2D pass (IDA 0x4700C0) ends by emitting a marker render command via 0x47010F ->
 * 0xAA7300. A call-site hook there runs on the main thread once per frame in every connection
 * state, so text queued with R_AddCmdDrawText (0xAA61A0) lands inside the 2D frame. */
#include "std_include.h"
#include "game/game.h"
#include "utils/hook.h"
#include "component/components.h"
#include "component/overlay.h"

#define VA_FRAME_SITE      0x47010Fu
#define VA_FRAME_MARKER    0xAA7300u
#define VA_DRAW_TEXT       0xAA61A0u /* cdecl(text, maxChars, font, x, y, xScale, yScale, rot, color*, style) */
#define VA_FONT            0x1A1AD98u /* small dev font handle, set by the renderer init */
#define VA_SCRPLACE        0x1A32500u /* float[]: [0],[1] pixel scale, [16],[15] x,y offset */
#define VA_COLOR_WHITE     0xC51388u

typedef const char *(__cdecl *DrawText_t)(const char *, int, int, float, float, float, float, float,
                                          int, int);
static void *g_orig_frame;
static volatile LONG g_show_version = 1;
static char g_banner[160];

static void __cdecl draw_banner(void)
{
    int font = *(int *)A(VA_FONT);
    float *sc = (float *)A(VA_SCRPLACE);
    overlay_game_frame();
    if (!g_show_version || font == 0 || font == -1)
        return;
    _snprintf(g_banner, sizeof g_banner - 1, "r1-mod %s | QA 3.3.3.3 | %s", R1_VERSION,
              name_dvar_value());
    g_banner[sizeof g_banner - 1] = 0;
    /* bottom-left, in pixels: sc[8], sc[9] are the screen width/height (1920 x 1080 here), sc[0],
     * sc[1] the pixel scale of the 853 x 480 virtual space; sc[14..17] are the safe-area margins
     * that the FPS graph anchors to, which put the first version at the top right */
    ((DrawText_t)A(VA_DRAW_TEXT))(g_banner, 0x7FFFFFFF, font, sc[0] * 6.0f, sc[9] - sc[1] * 6.0f,
                                  sc[0] * 0.5f, sc[1] * 0.5f, 0.0f, (int)A(VA_COLOR_WHITE), 0);
}

static __declspec(naked) void frame_hook(void)
{
    __asm {
        pushfd
        pushad
        call draw_banner
        popad
        popfd
        jmp dword ptr [g_orig_frame]
    }
}

int render_install_frame_hook(void)
{
    return patch_call(VA_FRAME_SITE, VA_FRAME_MARKER, (void *)frame_hook, &g_orig_frame);
}

/* The renderer's end of frame (0xA986B0, called by the backend 0xA99FE0 or the single-threaded
 * path 0xAA5690) calls 0xAD3DA0 right before its EndScene; the overlay draws after that call,
 * while the scene is still open (overlay.cpp). cdecl, one argument, result unused. */
#define VA_ENDFRAME_SITE 0xA98795u
#define VA_ENDFRAME      0xAD3DA0u
static void *g_orig_endframe;

static void __cdecl endframe_hook(void *arg)
{
    ((void(__cdecl *)(void *))g_orig_endframe)(arg);
    overlay_render_scene();
}

int render_install_endframe_hook(void)
{
    return patch_call(VA_ENDFRAME_SITE, VA_ENDFRAME, (void *)endframe_hook, &g_orig_endframe);
}

/* cx_version [0|1]   show or hide the banner */
static void __cdecl cmd_cx_version(void)
{
    if (Cmd_Argc() >= 2)
        g_show_version = atoi(Cmd_Argv(1)) != 0;
    out("version banner %s\n", g_show_version ? "on" : "off");
}

static void __cdecl cmd_cx_browser(void)
{
    overlay_toggle_browser();
}

static void __cdecl cmd_cx_statspanel(void)
{
    overlay_toggle_stats();
}

void render_register(void)
{
    command_add("cx_version", cmd_cx_version);
    command_add("cx_browser", cmd_cx_browser);
    command_add("cx_statspanel", cmd_cx_statspanel);
}
