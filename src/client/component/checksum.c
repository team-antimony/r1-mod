/* ---- configstring checksum ----
 * CL_ParseGamestate (0x46DEA0) compares the server's u32 against g_csChecksum (0x2DF7D90), which sub_5850B0 computes
 * from the net-const-string assets of the mode the client loaded. The value differs by lobby path (Free For All
 * matchmaking 3852334052, room.create something else), so a private server cannot know it up front. Turning the
 * `jz` after the compare at 0x46E071 into `jmp` accepts any value; cx_cs shows the client's value afterwards. */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"

#define VA_CS_JZ 0x46E071u
#define VA_CS_CHECKSUM 0x2DF7D90u
static volatile LONG g_cschk_off;

static int set_cschk(int off)
{
    unsigned char *p = (unsigned char *)A(VA_CS_JZ);
    unsigned char want = off ? 0xEB : 0x74;
    DWORD old;
    if ((p[0] != 0x74 && p[0] != 0xEB) || p[1] != 0x0F)
        return 0;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    p[0] = want;
    VirtualProtect(p, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 1);
    g_cschk_off = off;
    return 1;
}

void checksum_startup(void)
{
    if (!set_cschk(1))
        out("r1-mod: configstring checksum compare not recognised, cx_cs unavailable\n");
}

/* cx_cs [0|1]   configstring checksum check off (1, default) or on (0); prints the client's value */
static void __cdecl cmd_cx_cs(void)
{
    if (Cmd_Argc() >= 2 && !set_cschk(atoi(Cmd_Argv(1)) != 0))
        out("cx_cs: unexpected code at the checksum compare, not changed\n");
    out("configstring checksum check: %s, client value %u\n", g_cschk_off ? "ignored" : "enforced",
        *(unsigned *)A(VA_CS_CHECKSUM));
}

void checksum_register(void)
{
    command_add("cx_cs", cmd_cx_cs);
}
