/* ---- skip the start-up logo videos ----
 * Com_Init (sub_59C310, at 0x59CE36) and the renderer start-up (sub_465260, at 0x4655CF) queue
 * `autocinematic PUB_logo` and then CODO_logo only while dvars 2452 and 2628 are both 0, and `autocinematic none`
 * otherwise. Setting those dvars on the command line does not work (they read 0 after registration), so the first
 * `jnz` of each test is made a `jmp` to the `autocinematic none` branch. Patched from DllMain, before Com_Init runs.
 * The launcher sets R1MOD_SKIP_INTRO=1 for the game with skip_intro=1 in r1-mod.ini or --skip-intro. */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"

static const unsigned g_sites[] = {0x59CE3Fu, 0x4655D2u};

static int jnz_to_jmp(unsigned va)
{
    unsigned char *p = (unsigned char *)A(va);
    DWORD old;
    if (p[0] == 0xEB)
        return 1;
    if (p[0] != 0x75 || !VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    p[0] = 0xEB;
    VirtualProtect(p, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 1);
    return 1;
}

/* -1 = not asked for, otherwise the number of sites patched */
int intro_install(void)
{
    char value[8];
    int i, n = 0;
    if (!GetEnvironmentVariableA("R1MOD_SKIP_INTRO", value, sizeof value) || value[0] != '1')
        return -1;
    for (i = 0; i < (int)(sizeof g_sites / sizeof g_sites[0]); i++)
        n += jnz_to_jmp(g_sites[i]);
    return n;
}

void intro_report(int patched)
{
    if (patched == 2)
        out("r1-mod: start-up logo videos skipped\n");
    else if (patched >= 0)
        out("r1-mod: logo video test not recognised at %d of 2 sites, the videos may still play\n", 2 - patched);
}
