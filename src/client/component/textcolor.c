/* ---- IW4x-style text colours: ^: rainbow and ^; the server's colour ----
 * QA draws text colour codes ^0..^9 (the glyph loops of the text renderer 0xA96780 / 0xA96230 take "^<digit>",
 * ColorIndex 0x6374F0 maps it to 0..9, and 0xA95A50 turns that into RGBA: ^0..^7 from the table 0xC10870, ^8 and ^9
 * from two colours the game sets). IW4x added two more codes, kept here with the same meaning:
 *   ^:  rainbow: the hue cycles over time (every ^: on screen shows the same colour at a given moment)
 *   ^;  the server's colour: sv_customTextColor "r g b" (0..1), which r1-server reports in its getinfo reply and
 *       r1-mod reads when a join starts; orange until a server says otherwise
 *   ^<  the same colour: a player name cannot hold ';' (userinfo, the game log), so r1-server turns a name's ^;
 *       into ^<
 * Done by widening the digit checks: every "c <= '9'" of the text code (draw, width, wrap, clean) becomes
 * "c <= '<'", ColorIndex returns 10 / 11 / 12 for ':' / ';' / '<' instead of the white fallback, and 0xA95A50 is replaced by
 * text_color below, which gives the two new codes their colours and the others exactly what the game gave.
 * r1-server keeps ^: and ^< in player names (sv_allowColoredNames). */
#include "std_include.h"
#include <math.h>
#include "game/game.h"
#include "component/components.h"
#include "component/overlay.h"

#define VA_COLOR_FROM_CODE 0xA95A50u /* cdecl (unsigned char code, int *rgba out) */
#define VA_COLOR_TABLE     0xC10870u /* ^0..^7, bytes R G B A */
#define VA_COLOR_8         0x103512Cu
#define VA_COLOR_9         0x1035130u

/* (VA of the immediate byte, original value, new value) */
static const struct {
    unsigned va;
    unsigned char from, to;
} g_patches[] = {
    {0x6374F9u, 0x0A, 0x0D}, /* ColorIndex: (c - '0') >= 10 -> white; now >= 13 */
    {0xA9639Eu, 0x39, 0x3C}, /* glyph loop (0xA96230): c <= '9' */
    {0xA969BBu, 0x39, 0x3C}, /* glyph loop (0xA96780) */
    {0xAA3C34u, 0x39, 0x3C}, /* text width 0xAA3B70: a code has no width */
    {0xAA3D28u, 0x39, 0x3C}, /* 0xAA3CA0 */
    {0xAA3ED0u, 0x39, 0x3C}, /* 0xAA3E60 */
    {0x637BB8u, 0x39, 0x3C}, /* colour stripper 0x637B90 */
};

static volatile LONG g_custom_rgba = (LONG)0xFF00B3FFu; /* R FF, G B3, B 00, A FF: orange */

static unsigned rgba(double r, double g, double b)
{
    unsigned R = (unsigned)(r * 255.0 + 0.5), G = (unsigned)(g * 255.0 + 0.5), B = (unsigned)(b * 255.0 + 0.5);
    return R | G << 8 | B << 16 | 0xFF000000u;
}

static unsigned rainbow(void)
{
    double h = (double)(GetTickCount() % 3000u) / 500.0; /* one turn in 3 s, 0..6 */
    double x = 1.0 - fabs(fmod(h, 2.0) - 1.0);
    switch ((int)h) {
    case 0: return rgba(1, x, 0);
    case 1: return rgba(x, 1, 0);
    case 2: return rgba(0, 1, x);
    case 3: return rgba(0, x, 1);
    case 4: return rgba(x, 0, 1);
    default: return rgba(1, 0, x);
    }
}

static int __cdecl text_color(unsigned char code, int *out)
{
    unsigned index = (unsigned char)(code - '0');
    if (code == ':')
        *out = (int)rainbow();
    else if (code == ';' || code == '<')
        *out = g_custom_rgba;
    else if (index < 8)
        *out = ((const int *)A(VA_COLOR_TABLE))[index];
    else if (code == '8')
        *out = *(const int *)A(VA_COLOR_8);
    else if (code == '9')
        *out = *(const int *)A(VA_COLOR_9);
    else
        *out = -1;
    return *out;
}

/* "r g b" (0..1) from a server, as IW4x's sv_customTextColor */
void textcolor_set_custom(const char *text)
{
    float r, g, b;
    if (!text || sscanf(text, "%f %f %f", &r, &g, &b) != 3)
        return;
    if (r < 0) r = 0; if (r > 1) r = 1;
    if (g < 0) g = 0; if (g > 1) g = 1;
    if (b < 0) b = 0; if (b > 1) b = 1;
    InterlockedExchange(&g_custom_rgba, (LONG)rgba(r, g, b));
}

/* ---- colour codes in Scaleform text ----
 * The Scaleform UI (the TAB scoreboard, the match results, menus) draws its text itself and shows a colour code as
 * the characters it is made of ("^<pla^:yer" on the scoreboard, 2026-11-11). Every string the game hands to it goes
 * through MultiByteToWideChar with the game's code page 936 (the scoreboard rows: 0x5C4520), and the engine's own
 * text renderer does not, so the exe's import of MultiByteToWideChar is pointed at a wrapper that removes colour
 * codes (^0..^9, ^:, ^;, ^<) from code-page-936 text first. */
typedef int(WINAPI *mbtwc_fn)(UINT, DWORD, LPCCH, int, LPWSTR, int);
static mbtwc_fn g_mbtwc;

static int is_code(const char *p, const char *end)
{
    return p + 1 < end && p[0] == '^' && ((p[1] >= '0' && p[1] <= '9') || p[1] == ':' || p[1] == ';' || p[1] == '<');
}

static int WINAPI mbtwc_hook(UINT cp, DWORD flags, LPCCH s, int n, LPWSTR w, int wn)
{
    char buf[2048];
    const char *p, *end;
    int len, k = 0, any = 0;
    if (cp != 936 || !s)
        return g_mbtwc(cp, flags, s, n, w, wn);
    len = n < 0 ? (int)strnlen(s, sizeof buf) : n;
    if (len >= (int)sizeof buf)
        return g_mbtwc(cp, flags, s, n, w, wn);
    end = s + len;
    for (p = s; p < end; p++)
        if (is_code(p, end)) {
            any = 1;
            break;
        }
    if (!any)
        return g_mbtwc(cp, flags, s, n, w, wn);
    for (p = s; p < end; p++) {
        if (is_code(p, end)) {
            p++;
            continue;
        }
        buf[k++] = *p;
    }
    if (n < 0)
        buf[k++] = 0; /* -1: the terminator counts */
    return g_mbtwc(cp, flags, buf, k, w, wn);
}

/* the exe's import slot for kernel32!MultiByteToWideChar */
static void hook_scaleform_text(void)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
    FARPROC target = GetProcAddress(GetModuleHandleA("kernel32.dll"), "MultiByteToWideChar");
    if (!dir.VirtualAddress || !target)
        return;
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; t->u1.Function; t++) {
            DWORD old;
            if ((FARPROC)t->u1.Function != target)
                continue;
            if (VirtualProtect(&t->u1.Function, sizeof t->u1.Function, PAGE_READWRITE, &old)) {
                g_mbtwc = (mbtwc_fn)target;
                t->u1.Function = (DWORD)(uintptr_t)mbtwc_hook;
                VirtualProtect(&t->u1.Function, sizeof t->u1.Function, old, &old);
            }
            return;
        }
    }
    out("r1-mod: Scaleform text keeps colour codes (MultiByteToWideChar import not found)\n");
}

void textcolor_register(void)
{
    unsigned char *fn = (unsigned char *)A(VA_COLOR_FROM_CODE);
    static const unsigned char prologue[4] = {0x53, 0x8B, 0x5C, 0x24}; /* push ebx; mov ebx, [esp+8] */
    DWORD old;
    size_t i;
    hook_scaleform_text();
    for (i = 0; i < sizeof g_patches / sizeof g_patches[0]; i++)
        if (*(unsigned char *)A(g_patches[i].va) != g_patches[i].from) {
            out("r1-mod: text colours ^: and ^; off (byte at 0x%x is not 0x%02x)\n", g_patches[i].va, g_patches[i].from);
            return;
        }
    if (memcmp(fn, prologue, sizeof prologue)) {
        out("r1-mod: text colours ^: and ^; off (colour function not recognised)\n");
        return;
    }
    for (i = 0; i < sizeof g_patches / sizeof g_patches[0]; i++) {
        unsigned char *p = (unsigned char *)A(g_patches[i].va);
        if (VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) {
            *p = g_patches[i].to;
            VirtualProtect(p, 1, old, &old);
        }
    }
    if (VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old)) {
        fn[0] = 0xE9;
        *(int *)(fn + 1) = (int)((uintptr_t)text_color - ((uintptr_t)fn + 5));
        VirtualProtect(fn, 5, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
}
