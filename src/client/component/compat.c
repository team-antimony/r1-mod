/* ---- compatibility number with r1-server (docs/COMPATIBILITY.md) ----
 * The client declares its side in the connect userinfo: r1_compat (src/common/compat.h) and r1_version. QA builds that
 * userinfo by hand in CL_CheckForResend (0x465C00) from a fixed list of keys (name, clantag, xuid, ... onlineStats);
 * no dvar flag adds to it. Its last step formats the packet, Com_sprintf(buf, 0x400, "connect %s \"%s\"", xuid,
 * userinfo) at 0x46627A, so that call goes through connect_sprintf, which appends the two keys to the userinfo (a
 * 1024-byte buffer) first when the packet still fits in its 1024 bytes. Servers that do not know the keys keep them
 * as unknown userinfo.
 *
 * The server's side, r1_compat in its getinfo reply, is read with the message of the day before a join
 * (overlay.cpp) and reported with compat_server_answer. */
#include "std_include.h"
#include "game/game.h"
#include "utils/hook.h"
#include "component/components.h"

#define VA_CONNECT_SPRINTF_SITE 0x46627Au /* call Com_sprintf(buf, 0x400, "connect %s \"%s\"", xuid, userinfo) */
#define VA_COM_SPRINTF_CONNECT  0x637BE0u
#define CONNECT_BUF             0x400

typedef int (__cdecl *connect_sprintf_t)(char *, size_t, const char *, const char *, const char *);
static connect_sprintf_t g_orig_sprintf;
static volatile LONG g_sent;           /* connects that carried the keys */
static char g_server_compat[16];       /* last server's r1_compat; "" = not declared, "?" = no answer yet */

static int __cdecl connect_sprintf(char *buf, size_t size, const char *fmt, const char *xuid, char *userinfo)
{
    char add[64];
    int n = _snprintf(add, sizeof add - 1, "\\r1_compat\\%d\\r1_version\\%s", R1_COMPAT, R1_VERSION);
    add[sizeof add - 1] = 0;
    if (userinfo && xuid && n > 0 && !strstr(userinfo, "\\r1_compat\\")) {
        size_t have = strlen(userinfo);
        /* "connect " + xuid + " \"" + userinfo + "\"" must still fit the packet buffer */
        if (have + (size_t)n < CONNECT_BUF && 8 + strlen(xuid) + 2 + have + (size_t)n + 1 < CONNECT_BUF) {
            memcpy(userinfo + have, add, (size_t)n + 1);
            InterlockedIncrement(&g_sent);
        }
    }
    return g_orig_sprintf(buf, size, fmt, xuid, userinfo);
}

void compat_server_answer(const char *server_compat)
{
    const char *v = server_compat ? server_compat : "";
    _snprintf(g_server_compat, sizeof g_server_compat - 1, "%s", v);
    if (!v[0])
        out("r1-mod: this server does not declare r1_compat (this client: %d)\n", R1_COMPAT);
    else if (atoi(v) != R1_COMPAT)
        out("r1-mod: this server is r1_compat %s, this client %d; features that changed between them may not "
            "match (docs/COMPATIBILITY.md)\n", v, R1_COMPAT);
}

/* cx_compat: this client's number and the last server's */
static void __cdecl cmd_cx_compat(void)
{
    out("r1_compat %d (r1-mod %s); last server: %s; connects that carried it: %ld\n", R1_COMPAT, R1_VERSION,
        g_server_compat[0] == '?' ? "no answer yet" : g_server_compat[0] ? g_server_compat : "not declared",
        (long)g_sent);
}

void compat_register(void)
{
    g_server_compat[0] = '?';
    if (!patch_call(VA_CONNECT_SPRINTF_SITE, VA_COM_SPRINTF_CONNECT, (void *)connect_sprintf,
                    (void **)&g_orig_sprintf))
        out("r1-mod: connect call site differs, r1_compat is not sent\n");
    command_add("cx_compat", cmd_cx_compat);
}
