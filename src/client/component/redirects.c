/* ---- loopback redirects (formerly frida hooks in launch.py / qa_console.py) ----
 * The first STUN probe goes to a hard-wired Tencent address (116.130.229.213:8000) through the exe's WS2_32
 * `connect` import (ordinal 4, IAT slot 0xB5B498); it is sent to 127.0.0.1 instead. The decrypted STUN host table
 * handed to sub_B166D0 (usercall: ebx = config, host count at +392, 36-byte entries from +396; one call site,
 * 0xB16B8B) is rewritten to loopback on its first use. Both must be in place before Demonware starts, so they are
 * installed from DllMain when the launcher injects at the entry point. */
#include "std_include.h"
#include "game/game.h"
#include "utils/hook.h"
#include "component/components.h"

#define VA_IAT_CONNECT 0xB5B498u
#define VA_STUN_SITE   0xB16B8Bu
#define VA_STUN_SETUP  0xB166D0u

typedef int (WSAAPI *connect_t)(SOCKET, const struct sockaddr *, int);
static connect_t g_orig_connect;
static void *g_orig_stun;
static volatile LONG g_stun_done, g_connect_redirects;

static int WSAAPI connect_hook(SOCKET s, const struct sockaddr *name, int len)
{
    if (name && len >= (int)sizeof(struct sockaddr_in) && name->sa_family == AF_INET) {
        const unsigned char *ip = (const unsigned char *)&((const struct sockaddr_in *)name)->sin_addr;
        if (ip[0] == 116 && ip[1] == 130) {
            struct sockaddr_in local = *(const struct sockaddr_in *)name;
            local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            InterlockedIncrement(&g_connect_redirects);
            return g_orig_connect(s, (const struct sockaddr *)&local, sizeof local);
        }
    }
    return g_orig_connect(s, name, len);
}

static void __cdecl fix_stun_hosts(unsigned char *cfg)
{
    unsigned i, n;
    if (InterlockedExchange(&g_stun_done, 1))
        return;
    __try {
        n = *(unsigned *)(cfg + 392);
        for (i = 0; i < n && i < 16; i++) {
            char *host = (char *)(cfg + 396 + i * 36);
            if (*host && strcmp(host, "127.0.0.1") != 0)
                strcpy(host, "127.0.0.1");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static __declspec(naked) void stun_hook(void)
{
    __asm {
        pushfd
        pushad
        push ebx
        call fix_stun_hosts
        add esp, 4
        popad
        popfd
        jmp dword ptr [g_orig_stun]
    }
}

/* returns a bit mask: 1 = connect redirect, 2 = STUN table redirect */
int redirects_install(void)
{
    connect_t *slot = (connect_t *)A(VA_IAT_CONNECT);
    connect_t real = (connect_t)GetProcAddress(GetModuleHandleA("ws2_32.dll"), "connect");
    int ok = 0;
    DWORD old;
    if (real && (*slot == real || *slot == connect_hook) &&
        VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) {
        g_orig_connect = real;
        *slot = connect_hook;
        VirtualProtect(slot, sizeof *slot, old, &old);
        ok |= 1;
    }
    if (patch_call(VA_STUN_SITE, VA_STUN_SETUP, (void *)stun_hook, &g_orig_stun))
        ok |= 2;
    return ok;
}

/* installed_at_load: the mask DllMain got; anything else means a late injection, so try again now */
void redirects_report(int installed_at_load)
{
    int redirects = installed_at_load;
    if (redirects == 3) {
        out("r1-mod: loopback redirects installed at load (STUN table %s, %ld connect redirects so far)\n",
            g_stun_done ? "rewritten" : "not used yet", (long)g_connect_redirects);
    } else {
        /* injected after start-up (launch.py / inject.py): STUN set-up has usually run already */
        redirects |= redirects_install();
        out("r1-mod: late injection, loopback redirects: connect %s, STUN table %s\n",
            (redirects & 1) ? "on" : "FAILED", (redirects & 2) ? "hooked" : "FAILED");
    }
}
