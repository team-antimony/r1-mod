/* ---- direct connect: replaces the Demonware transport for one peer with a plain UDP socket ---- */
#include "std_include.h"
#include "game/game.h"
#include "utils/hook.h"
#include "component/components.h"

#define VA_CONNECT_HOST     0x46B8D0u /* thiscall CL_ConnectToHost(host *this), host is 0x130 bytes */
#define VA_XNADDR_CALLSITE  0x46BA49u /* call DW_XNADDR_ToNetadr inside CL_ConnectToHost */
#define VA_XNADDR           0x6D6F90u /* usercall: eax = out netadr, stack = host, key; returns al */
#define VA_SEND_CALLSITE    0x6429C0u /* call DW_SendPacket inside Sys_SendPacket */
#define VA_SEND             0x6D5CF0u /* cdecl int(size, data, netadr[6]); netadr[5] = DW handle */
#define VA_RECV_CALLSITE1   0x598915u /* calls DW_RecvPacket inside NET_Loop */
#define VA_RECV_CALLSITE2   0x598AE5u
#define VA_RECV             0x6D5B00u /* cdecl int(netadr *out, char *buf, int maxlen) */
#define VA_NETSTR           0x6D6A40u /* usercall: edi = DW handle, esi = char[64] out; returns al */
#define VA_NETSTR_SITE1     0x46BA6Au
#define VA_NETSTR_SITE2     0x640CC0u
#define VA_NETSTR_SITE3     0x6D6337u
#define VA_NETSTR_SITE4     0x6D63F7u
#define VA_PEERSTATE        0x6D61D0u /* cdecl int(handle): 0/3 unreachable, 1 pending, 2 connected */
#define VA_PEERSTATE_SITE1  0x465C55u
#define VA_PEERSTATE_SITE2  0x685D80u
#define VA_PEERSTATE_SITE3  0x685E11u
#define VA_PEERSTATE_SITE4  0x6E75A8u
/* Netadr handle marking a direct (non-Demonware) peer. It must be out of range for both the
 * signed (handle < 157) and unsigned (handle <= 0x9C) checks in the DW table code. */
#define DIRECT_HANDLE       0x7FFFFFFFu
#define DEFAULT_PORT        28969

typedef int (__cdecl *DwSend_t)(int, const void *, unsigned, unsigned, unsigned, unsigned, unsigned,
                                unsigned);
typedef int (__cdecl *DwRecv_t)(unsigned *, char *, int);

static volatile LONG g_direct_active;
static unsigned g_direct_ip;           /* network order */
static unsigned short g_direct_port;   /* network order */
static SOCKET g_sock = INVALID_SOCKET;
static void *g_orig_xnaddr;
static DwSend_t g_orig_send;
static DwRecv_t g_orig_recv;
static long g_sent, g_received;

static void fill_direct_netadr(unsigned *na, unsigned ip, unsigned short port)
{
    memset(na, 0, 24);
    na[0] = 4; /* NA_IP */
    na[1] = ip;
    *(unsigned short *)(na + 2) = port;
    na[5] = DIRECT_HANDLE;
}

static void __cdecl fill_direct_for_xnaddr(unsigned *out)
{
    fill_direct_netadr(out, g_direct_ip, g_direct_port);
}

static __declspec(naked) void xnaddr_hook(void)
{
    __asm {
        cmp g_direct_active, 0
        je orig
        push eax
        call fill_direct_for_xnaddr
        add esp, 4
        mov al, 1
        ret
    orig:
        jmp dword ptr [g_orig_xnaddr]
    }
}

static void __cdecl fill_direct_name(char *out)
{
    memset(out, 0, 64);
    strcpy(out, "direct");
}

static void *g_orig_netstr;

typedef int (__cdecl *PeerState_t)(unsigned);
static PeerState_t g_orig_peerstate;

static int __cdecl peerstate_hook(unsigned handle)
{
    if (handle == DIRECT_HANDLE)
        return 2; /* connected */
    return g_orig_peerstate(handle);
}

static __declspec(naked) void netstr_hook(void)
{
    __asm {
        cmp edi, DIRECT_HANDLE
        jne orig
        push esi
        call fill_direct_name
        add esp, 4
        mov al, 1
        ret
    orig:
        jmp dword ptr [g_orig_netstr]
    }
}

static int ensure_socket(void)
{
    u_long nonblock = 1;
    struct sockaddr_in any;
    if (g_sock != INVALID_SOCKET)
        return 1;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET)
        return 0;
    ioctlsocket(g_sock, FIONBIO, &nonblock);
    memset(&any, 0, sizeof any);
    any.sin_family = AF_INET;
    if (bind(g_sock, (struct sockaddr *)&any, sizeof any) != 0) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return 0;
    }
    return 1;
}

static int __cdecl send_hook(int size, const void *data, unsigned a0, unsigned a1, unsigned a2,
                             unsigned a3, unsigned a4, unsigned handle)
{
    if (handle == DIRECT_HANDLE) {
        struct sockaddr_in to;
        int n;
        if (!ensure_socket())
            return -7;
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = a1;
        to.sin_port = *(unsigned short *)&a2;
        n = sendto(g_sock, (const char *)data, size, 0, (struct sockaddr *)&to, sizeof to);
        if (n >= 0)
            g_sent++;
        return n >= 0 ? n : -7;
    }
    return g_orig_send(size, data, a0, a1, a2, a3, a4, handle);
}

static int __cdecl recv_hook(unsigned *out, char *buf, int maxlen)
{
    if (InterlockedCompareExchange(&g_direct_active, 0, 0) && g_sock != INVALID_SOCKET) {
        struct sockaddr_in from;
        int fromlen = sizeof from;
        int n = recvfrom(g_sock, buf, maxlen, 0, (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            fill_direct_netadr(out, from.sin_addr.s_addr, from.sin_port);
            g_received++;
            return n;
        }
    }
    return g_orig_recv(out, buf, maxlen);
}

int direct_connect_install(void)
{
    WSADATA wsa;
    int ok = 1;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    ok &= patch_call(VA_XNADDR_CALLSITE, VA_XNADDR, (void *)xnaddr_hook, &g_orig_xnaddr);
    ok &= patch_call(VA_NETSTR_SITE1, VA_NETSTR, (void *)netstr_hook, &g_orig_netstr);
    ok &= patch_call(VA_NETSTR_SITE2, VA_NETSTR, (void *)netstr_hook, NULL);
    ok &= patch_call(VA_NETSTR_SITE3, VA_NETSTR, (void *)netstr_hook, NULL);
    ok &= patch_call(VA_NETSTR_SITE4, VA_NETSTR, (void *)netstr_hook, NULL);
    ok &= patch_call(VA_PEERSTATE_SITE1, VA_PEERSTATE, (void *)peerstate_hook,
                     (void **)&g_orig_peerstate);
    ok &= patch_call(VA_PEERSTATE_SITE2, VA_PEERSTATE, (void *)peerstate_hook, NULL);
    ok &= patch_call(VA_PEERSTATE_SITE3, VA_PEERSTATE, (void *)peerstate_hook, NULL);
    ok &= patch_call(VA_PEERSTATE_SITE4, VA_PEERSTATE, (void *)peerstate_hook, NULL);
    ok &= patch_call(VA_SEND_CALLSITE, VA_SEND, (void *)send_hook, (void **)&g_orig_send);
    ok &= patch_call(VA_RECV_CALLSITE1, VA_RECV, (void *)recv_hook, (void **)&g_orig_recv);
    ok &= patch_call(VA_RECV_CALLSITE2, VA_RECV, (void *)recv_hook, NULL);
    return ok;
}

static void CL_ConnectToHost(void *host)
{
    void *fn = (void *)A(VA_CONNECT_HOST);
    __asm {
        mov ecx, host
        call fn
    }
}

int parse_target(const char *arg, unsigned *ip, unsigned short *port)
{
    char host[128];
    const char *colon = strrchr(arg, ':');
    size_t n = colon ? (size_t)(colon - arg) : strlen(arg);
    unsigned long addr;
    int p = DEFAULT_PORT;
    if (n == 0 || n >= sizeof host)
        return 0;
    memcpy(host, arg, n);
    host[n] = 0;
    addr = inet_addr(host);
    if (addr == INADDR_NONE)
        return 0;
    if (colon) {
        p = atoi(colon + 1);
        if (p <= 0 || p > 65535)
            return 0;
    }
    *ip = (unsigned)addr;
    *port = htons((unsigned short)p);
    return 1;
}

static DWORD WINAPI gdvars_later(LPVOID unused)
{
    (void)unused;
    Sleep(8000);
    Cbuf_AddText("cx_gdvars\n");
    return 0;
}

/* runs on the game thread through the command buffer */
static void __cdecl cmd_connect(void)
{
    unsigned ip;
    unsigned short port;
    void *host;
    if (Cmd_Argc() < 2 || !parse_target(Cmd_Argv(1), &ip, &port)) {
        out("usage: connect <ip>[:port]   (default port %d)\n", DEFAULT_PORT);
        return;
    }
    if (!ensure_socket()) {
        out("connect: socket setup failed (%d)\n", WSAGetLastError());
        return;
    }
    g_direct_ip = ip;
    g_direct_port = port;
    InterlockedExchange(&g_direct_active, 1);
    host = calloc(1, 0x130);
    out("connect: direct to %s:%d\n", inet_ntoa(*(struct in_addr *)&ip), ntohs(port));
    CL_ConnectToHost(host);
    free(host);
    /* a remote match needs the game dvars registered while the map loads (see cx_gdvars) */
    CreateThread(NULL, 0, gdvars_later, NULL, 0, NULL);
}

static void __cdecl cmd_cx_net(void)
{
    out("net: direct=%ld sent=%ld received=%ld hooks installed\n", (long)g_direct_active, g_sent,
        g_received);
}

/* A client that joins a remote server never runs the level start path (0x566940) that calls
 * G_RegisterDvars (0x4E2AC0), so cached dvar pointers such as g_gametype (0x2C1A564) stay null and the
 * first snapshot crashes the gametype check at 0x574D60. */
#define VA_G_REGISTER_DVARS 0x4E2AC0u

static void __cdecl cmd_cx_gdvars(void)
{
    typedef void (__cdecl *Fn)(void);
    unsigned *gt = (unsigned *)A(0x2C1A564u);
    ((Fn)A(VA_G_REGISTER_DVARS))();
    out("cx_gdvars: g_gametype pointer now 0x%08x\n", *gt);
}

void direct_connect_register(void)
{
    command_add("connect", cmd_connect);
    command_add("cx_net", cmd_cx_net);
    command_add("cx_gdvars", cmd_cx_gdvars);
}
