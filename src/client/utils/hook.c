#include "std_include.h"
#include "game/game.h"
#include "utils/hook.h"

int patch_call(unsigned site_va, unsigned expect_va, void *hook, void **orig)
{
    unsigned char *p = (unsigned char *)A(site_va);
    DWORD old;
    int rel;
    if (p[0] != 0xE8)
        return 0;
    rel = *(int *)(p + 1);
    if ((uintptr_t)p + 5 + rel != A(expect_va))
        return 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    if (orig)
        *orig = (void *)A(expect_va);
    *(int *)(p + 1) = (int)((uintptr_t)hook - ((uintptr_t)p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}
