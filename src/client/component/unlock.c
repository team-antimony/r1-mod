/* ---- unlock every item ----
 * Item ownership goes through sub_695BC0 (item info: out[4] = owned). An id the inventory has no record for falls
 * through to sub_695A10(esi = id): true for the onboarding character or for ids in the 24-entry entitlement table
 * (the free starter items). Making it true for every positive id marks the whole item table as owned with unlimited
 * quantity; ids on the hidden list (0xC8031D0) are still refused first. Replaced by `test esi,esi / setg al / ret`.
 * On by default; r1-mod_nounlock.txt next to the DLL starts with it off. */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"

#define VA_ITEM_FREE 0x695A10u
static const unsigned char g_free_orig[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
static const unsigned char g_free_patch[6] = {0x85, 0xF6, 0x0F, 0x9F, 0xC0, 0xC3};
static volatile LONG g_unlock_on;

static int set_unlock(int on)
{
    unsigned char *p = (unsigned char *)A(VA_ITEM_FREE);
    const unsigned char *want = on ? g_free_patch : g_free_orig;
    const unsigned char *have = on ? g_free_orig : g_free_patch;
    DWORD old;
    if (memcmp(p, have, 6) != 0 && memcmp(p, want, 6) != 0)
        return 0; /* unknown code at this address: wrong build */
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(p, want, 6);
    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    g_unlock_on = on;
    return 1;
}

void unlock_startup(void)
{
    char noun[MAX_PATH];
    _snprintf(noun, sizeof noun, "%sr1-mod_nounlock.txt", g_dir);
    if (GetFileAttributesA(noun) == INVALID_FILE_ATTRIBUTES && !set_unlock(1))
        out("r1-mod: ownership check not recognised, unlock-all not applied\n");
}

/* cx_unlock [0|1]   all items owned (default on) */
static void __cdecl cmd_cx_unlock(void)
{
    if (Cmd_Argc() >= 2 && !set_unlock(atoi(Cmd_Argv(1)) != 0))
        out("cx_unlock: unexpected code at the ownership check, not changed\n");
    out("unlock all items: %s\n", g_unlock_on ? "on" : "off");
}

void unlock_register(void)
{
    command_add("cx_unlock", cmd_cx_unlock);
}
