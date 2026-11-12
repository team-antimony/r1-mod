/* r1-mod launcher - starts the CoD Online 3.3.3.3 QA client with everything it needs to reach the lobby.
 *
 * 1. Reads r1-mod.ini next to the exe (written with defaults on first run).
 * 2. Makes sure the game folder's dwauth.ini / stun.ini point at the local backend (originals kept as .orig).
 * 3. Starts the local Demonware backend (backend_dir, by default the repository's backend\ folder found above the
 *    exe; local_auth.server) unless something already listens on 127.0.0.1:443, and waits for its auth (443) and
 *    lobby (3074) ports.
 * 4. Starts the client with the QA launch arguments (-q 0 -src=tgp -game_id 29 -zone_id 83886082, without
 *    which WinMain exits with WIN_NO_TCLS) plus +set dw_username <name>. With skip_intro=1 in r1-mod.ini or
 *    --skip-intro (--intro overrides the ini) the game gets R1MOD_SKIP_INTRO=1 in its environment, and
 *    r1-mod.dll skips the start-up logo videos.
 * 5. Injects r1-mod.dll before the game's own code runs: the entry point is held on a `jmp $` while the loader
 *    finishes, LoadLibraryA runs in a remote thread (DllMain installs the loopback STUN/connect redirects, which
 *    must precede Demonware start-up), then the entry bytes are restored. r1-mod.dll then skips the onboarding
 *    screen whose matchmaking search otherwise ends in "server busy", applies playerdata, stats and the name.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "version.h"
#define SECTION "r1-mod"

static char g_dir[MAX_PATH], g_ini[MAX_PATH];

static void cfg(const char *key, const char *def, char *out, DWORD n)
{
    GetPrivateProfileStringA(SECTION, key, def, out, n, g_ini);
}

static void write_default_ini(void)
{
    static const char text[] =
        "; r1-mod launcher settings\r\n"
        "[r1-mod]\r\n"
        "; player name (dw_username and the name dvar; dw_username keeps 15 characters)\r\n"
        "name=player\r\n"
        "; your unpacked QA 3.3.3.3 game folder\r\n"
        "game_dir=\r\n"
        "game_exe=codoMP_client_shipRetail_loosefs_v2.exe\r\n"
        "; 1 = the room starts the match by itself 10 s after matchmaking; 0 = it waits for Ready\r\n"
        "auto_start=0\r\n"
        "; 1 = skip the publisher and CoD Online logo videos at start-up (--skip-intro / --intro override it)\r\n"
        "skip_intro=0\r\n"
        "; extra client arguments, e.g. +set r_fullscreen 0\r\n"
        "extra_args=\r\n"
        "; local Demonware backend (empty: the repository's backend\\ folder); {name} is replaced by the player name\r\n"
        "backend_dir=\r\n"
        "backend_cmd=uv run --with cryptography python -m local_auth.server --player {name} --lobby-port 3074 "
        "--stun-port 8000\r\n"
        "; 0 = never start the backend (start it yourself)\r\n"
        "start_backend=1\r\n";
    FILE *f;
    if (GetFileAttributesA(g_ini) != INVALID_FILE_ATTRIBUTES)
        return;
    f = fopen(g_ini, "wb");
    if (f) {
        fwrite(text, 1, sizeof text - 1, f);
        fclose(f);
        printf("wrote default %s\n", g_ini);
    }
}

/* text without carriage returns and trailing whitespace, for comparing line-ending variants */
static size_t normalize(char *dst, size_t cap, const char *src, size_t n)
{
    size_t i, w = 0;
    for (i = 0; i < n && w + 1 < cap; i++)
        if (src[i] != '\r')
            dst[w++] = src[i];
    while (w && (dst[w - 1] == '\n' || dst[w - 1] == ' '))
        w--;
    dst[w] = 0;
    return w;
}

/* Replace path's content with want unless it already matches (ignoring line endings and trailing
 * whitespace); the first replaced original is kept as <path>.orig */
static int ensure_file(const char *path, const char *want)
{
    char raw[1024], have[1024], norm_want[1024], orig[MAX_PATH];
    size_t n = 0, w;
    FILE *f = fopen(path, "rb");
    if (f) {
        n = fread(raw, 1, sizeof raw - 1, f);
        fclose(f);
    }
    n = normalize(have, sizeof have, raw, n);
    w = normalize(norm_want, sizeof norm_want, want, strlen(want));
    if (n == w && memcmp(have, norm_want, w) == 0)
        return 1;
    _snprintf(orig, sizeof orig, "%s.orig", path);
    if (f && GetFileAttributesA(orig) == INVALID_FILE_ATTRIBUTES)
        CopyFileA(path, orig, TRUE);
    f = fopen(path, "wb");
    if (!f)
        return 0;
    fwrite(want, 1, strlen(want), f);
    fclose(f);
    printf("updated %s\n", path);
    return 1;
}

static int port_open(unsigned short port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a;
    int ok;
    if (s == INVALID_SOCKET)
        return 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok = connect(s, (struct sockaddr *)&a, sizeof a) == 0;
    closesocket(s);
    return ok;
}

static void replace_all(char *dst, size_t n, const char *src, const char *key, const char *val)
{
    size_t kl = strlen(key), used = 0;
    while (*src && used + 1 < n) {
        if (!strncmp(src, key, kl)) {
            size_t vl = strlen(val);
            if (used + vl >= n)
                break;
            memcpy(dst + used, val, vl);
            used += vl;
            src += kl;
        } else {
            dst[used++] = *src++;
        }
    }
    dst[used] = 0;
}

/* backend_dir, or the repository's backend\ folder: the first backend\local_auth\server.py in the exe's folder or
 * up to six folders above it (the exe is built into build\bin\Win32\<config>\) */
static int backend_dir(char *out, DWORD n)
{
    char base[MAX_PATH], probe[MAX_PATH], *slash;
    int up;
    cfg("backend_dir", "", out, n);
    if (out[0])
        return 1;
    strncpy(base, g_dir, sizeof base - 1);
    base[sizeof base - 1] = 0;
    for (up = 0; up <= 6; up++) {
        _snprintf(probe, sizeof probe, "%sbackend\\local_auth\\server.py", base);
        if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
            _snprintf(out, n, "%sbackend", base);
            return 1;
        }
        base[strlen(base) - 1] = 0; /* drop the trailing backslash, then the last folder */
        slash = strrchr(base, '\\');
        if (!slash)
            break;
        slash[1] = 0;
    }
    return 0;
}

static int start_backend(const char *name)
{
    char dir[MAX_PATH], tmpl[512], cmd[1024], line[1200], log[MAX_PATH];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE out;
    int i;
    cfg("start_backend", "1", tmpl, sizeof tmpl);
    if (port_open(443)) {
        printf("backend: already listening on 127.0.0.1:443\n");
        if (!port_open(3074))
            printf("backend: WARNING nothing on lobby port 3074\n");
        return 1;
    }
    if (atoi(tmpl) == 0) {
        printf("backend: not running and start_backend=0; start it yourself\n");
        return 0;
    }
    if (!backend_dir(dir, sizeof dir)) {
        printf("backend: no backend\\ folder found above %s; set backend_dir in r1-mod.ini\n", g_dir);
        return 0;
    }
    cfg("backend_cmd",
        "uv run --with cryptography python -m local_auth.server --player {name} --lobby-port 3074 --stun-port 8000",
        tmpl, sizeof tmpl);
    replace_all(cmd, sizeof cmd, tmpl, "{name}", name);
    _snprintf(line, sizeof line, "cmd.exe /c %s", cmd);
    _snprintf(log, sizeof log, "%sbackend.log", g_dir);
    out = CreateFileA(log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    printf("backend: starting in %s: %s\n", dir, cmd);
    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) {
        printf("backend: start failed (error %lu)\n", GetLastError());
        if (out != INVALID_HANDLE_VALUE)
            CloseHandle(out);
        return 0;
    }
    CloseHandle(pi.hThread);
    if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    for (i = 0; i < 60; i++) {
        if (port_open(443) && port_open(3074)) {
            printf("backend: up (pid %lu, log %s)\n", pi.dwProcessId, log);
            CloseHandle(pi.hProcess);
            return 1;
        }
        if (WaitForSingleObject(pi.hProcess, 500) == WAIT_OBJECT_0) {
            printf("backend: exited early, see %s\n", log);
            CloseHandle(pi.hProcess);
            return 0;
        }
    }
    printf("backend: ports 443/3074 not open after 30 s, see %s\n", log);
    CloseHandle(pi.hProcess);
    return 0;
}

static DWORD running_client(const char *exe)
{
    PROCESSENTRY32 pe;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD pid = 0;
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    pe.dwSize = sizeof pe;
    if (Process32First(snap, &pe)) {
        do {
            if (!_stricmp(pe.szExeFile, exe)) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

/* Hold the entry point on `jmp $`, let the loader finish, load the DLL, restore the entry bytes. */
static int inject(PROCESS_INFORMATION *pi, const char *dll)
{
    CONTEXT ctx;
    unsigned char saved[2], spin[2] = {0xEB, 0xFE};
    SIZE_T done;
    DWORD old, code = 0;
    void *entry, *remote;
    HANDLE th;
    int i, ok = 0;

    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(pi->hThread, &ctx)) /* a new suspended process: eax = image entry point */
        return 0;
    entry = (void *)(uintptr_t)ctx.Eax;
    if (!ReadProcessMemory(pi->hProcess, entry, saved, 2, &done) ||
        !VirtualProtectEx(pi->hProcess, entry, 2, PAGE_EXECUTE_READWRITE, &old) ||
        !WriteProcessMemory(pi->hProcess, entry, spin, 2, &done))
        return 0;
    FlushInstructionCache(pi->hProcess, entry, 2);
    ResumeThread(pi->hThread);
    for (i = 0; i < 200; i++) {
        Sleep(25);
        SuspendThread(pi->hThread);
        ctx.ContextFlags = CONTEXT_CONTROL;
        ok = GetThreadContext(pi->hThread, &ctx) && (void *)(uintptr_t)ctx.Eip == entry;
        if (ok)
            break;
        ResumeThread(pi->hThread);
    }
    if (!ok) {
        printf("inject: entry point never reached\n");
        return 0;
    }
    /* main thread suspended at the entry point; every static import is loaded */
    ok = 0;
    remote = VirtualAllocEx(pi->hProcess, NULL, strlen(dll) + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote && WriteProcessMemory(pi->hProcess, remote, dll, strlen(dll) + 1, &done)) {
        th = CreateRemoteThread(pi->hProcess, NULL, 0,
                                (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                                       "LoadLibraryA"),
                                remote, 0, NULL);
        if (th) {
            WaitForSingleObject(th, 20000);
            GetExitCodeThread(th, &code); /* low 32 bits of the module handle */
            CloseHandle(th);
            ok = code != 0;
        }
    }
    WriteProcessMemory(pi->hProcess, entry, saved, 2, &done);
    VirtualProtectEx(pi->hProcess, entry, 2, old, &old);
    FlushInstructionCache(pi->hProcess, entry, 2);
    ResumeThread(pi->hThread);
    if (remote)
        VirtualFreeEx(pi->hProcess, remote, 0, MEM_RELEASE);
    printf("inject: %s %s (module 0x%08lx)\n", dll, ok ? "loaded" : "FAILED", code);
    return ok;
}

int main(int argc, char **argv)
{
    char name[64], game_dir[MAX_PATH], game_exe[MAX_PATH], extra[512], path[MAX_PATH], dll[MAX_PATH];
    char cmdline[2048];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    WSADATA wsa;
    DWORD pid, code = 0;
    int i, wait = 1, skip_intro = -1; /* -1: r1-mod.ini decides */
    char *slash;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-wait"))
            wait = 0;
        else if (!strcmp(argv[i], "--skip-intro"))
            skip_intro = 1;
        else if (!strcmp(argv[i], "--intro"))
            skip_intro = 0;
    }
    printf("r1-mod launcher %s\n", R1_VERSION);
    GetModuleFileNameA(NULL, g_dir, sizeof g_dir);
    slash = strrchr(g_dir, '\\');
    if (slash)
        slash[1] = 0;
    _snprintf(g_ini, sizeof g_ini, "%sr1-mod.ini", g_dir);
    _snprintf(dll, sizeof dll, "%sr1-mod.dll", g_dir);
    write_default_ini();
    cfg("name", "player", name, sizeof name);
    cfg("game_dir", "", game_dir, sizeof game_dir);
    if (!game_dir[0]) {
        printf("set game_dir in %s to your unpacked QA 3.3.3.3 game folder (see README.md)\n", g_ini);
        return 1;
    }
    cfg("game_exe", "codoMP_client_shipRetail_loosefs_v2.exe", game_exe, sizeof game_exe);
    cfg("extra_args", "", extra, sizeof extra);
    if (strpbrk(name, "\" \t")) {
        printf("name must not contain spaces or quotes: \"%s\"\n", name);
        return 1;
    }
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        printf("missing %s\n", dll);
        return 1;
    }
    if ((pid = running_client(game_exe)) != 0) {
        printf("the client is already running (pid %lu); close it first\n", pid);
        return 1;
    }

    /* loopback Demonware endpoints: lobby 3074, auth 443, game port 0x3EA for the STUN NAT check */
    _snprintf(path, sizeof path, "%s\\dwauth.ini", game_dir);
    ensure_file(path, "127.0.0.1 3074\n127.0.0.1 443\n0x03EA\n");
    _snprintf(path, sizeof path, "%s\\stun.ini", game_dir);
    ensure_file(path, "127.0.0.1\n");

    WSAStartup(MAKEWORD(2, 2), &wsa);
    /* inherited by the game: r1-mod.dll writes cx_match's startgame.trigger there */
    if (backend_dir(path, sizeof path))
        SetEnvironmentVariableA("R1MOD_BACKEND_DIR", path);
    if (!start_backend(name))
        printf("continuing without a confirmed backend; the client will stop with error 100\n");

    _snprintf(path, sizeof path, "%s\\%s", game_dir, game_exe);
    _snprintf(cmdline, sizeof cmdline,
              "\"%s\" -q 0 -src=tgp -game_id 29 -zone_id 83886082 +set dw_username %s %s", path, name, extra);
    /* inherited by the game; read by r1-mod.dll in DllMain */
    if (skip_intro < 0)
        skip_intro = GetPrivateProfileIntA(SECTION, "skip_intro", 0, g_ini) != 0;
    SetEnvironmentVariableA("R1MOD_SKIP_INTRO", skip_intro ? "1" : NULL);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(path, cmdline, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, game_dir, &si, &pi)) {
        printf("could not start %s (error %lu)\n", path, GetLastError());
        return 1;
    }
    printf("client: pid %lu, player \"%s\"\n", pi.dwProcessId, name);
    if (!inject(&pi, dll)) {
        printf("injection failed; terminating the client\n");
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }
    if (wait) {
        printf("waiting for the client to exit (Ctrl+C leaves it running)\n");
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        printf("client exited with code 0x%08lx\n", code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
