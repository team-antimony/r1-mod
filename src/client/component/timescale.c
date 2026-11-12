/* ---- time scale guard ----
 * In a remote match the script time scale (IDA 0x2E8A974) ends up holding ~6.8e-38. The frame limiter (0x59D840)
 * allows timescale * 320 fps, minimum 1, so the game ran at one frame per second. The write that corrupts it was not
 * caught; until it is, put back 1.0 whenever the value is outside a sane range (a real slow-motion stays inside it).
 * The same 50 ms loop re-applies the kept frame rate cap and turn speeds (fps.c, turn_speed.c). */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"

#define VA_TIMESCALE 0x2E8A974u
/* dvar "1867" (pointer at 0x2E8CFC0, registered 0x59BF82: default 1, 0.001..1000) is the engine
 * timescale multiplied in by sub_59D490. In a remote match on mp_crash_tropical it held 50: the game
 * ran 50x, the client clock raced past every snapshot (QA CL_AdjustTimeDelta resets per snapshot),
 * usercmds hit the server's +200 ms clamp and prediction ran out of command history. Not set by the
 * server, the scripts or this extension; until the writer is found, put back 1.0 when it is outside
 * the slow-motion range a script could use, and say so. */
#define VA_TIMESCALE_DVAR 0x2E8CFC0u

DWORD WINAPI timescale_guard(LPVOID unused)
{
    (void)unused;
    for (;;) {
        fps_keep();
        turn_speed_keep();
        movement_keep();
        weapon_anims_keep();
        localize_keep();
        fsmod_keep();
        volatile float *p = (volatile float *)A(VA_TIMESCALE);
        unsigned dvar = *(volatile unsigned *)A(VA_TIMESCALE_DVAR);
        float v = *p;
        if (!(v >= 0.001f && v <= 100.0f))
            *p = 1.0f;
        if (dvar) {
            volatile float *dv = (volatile float *)(dvar + 12);
            float f = *dv;
            if (!(f >= 0.05f && f <= 4.0f)) {
                *dv = 1.0f;
                out("r1-mod: timescale dvar 1867 was %g, reset to 1\n", (double)f);
            }
        }
        Sleep(50);
    }
}
