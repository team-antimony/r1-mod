/* ---- skip the first-run operator screen and its "server busy" dialog ----
 * The onboarding screen is gated by IsOnboardComplete (0x5C36F0): playerdata notification flag onboard_complete
 * (uiNotificationFlags, row 87 of mp/uiNotifications.csv) OR dvar "3233". Its Continue button starts a matchmaking
 * search (21/5) that the local backend cannot answer, which is what produces "server busy".
 *
 * The engine asks once per frame from sub_6B9ED0, but only once Demonware is live and the profile has been fetched
 * (sub_A3CEB0), and opens the screen the first time the answer is no. That call (0x6BA054) is hooked, so the decision
 * is made in order rather than raced from a thread: on the first call the saved playerdata is applied
 * (pd_profile_ready), then onboard_complete is set through the engine's own flag setter if the profile lacks it,
 * and the engine goes straight to the lobby with the user's profile. The older path (dvar 3233 and
 * switchtomainmenu from onboarding_thread) remains the fallback when the call site is not recognised or never
 * reached. Disable both by creating r1-mod_noskip.txt next to the DLL. */
#include "std_include.h"
#include "game/game.h"
#include "utils/hook.h"
#include "component/components.h"

#define VA_ONBOARD_SITE  0x6BA054u /* sub_6B9ED0: call IsOnboardComplete */
#define VA_ONBOARD_CHECK 0x5C36F0u /* cdecl bool(void) */
#define VA_NOTIFY_INDEX  0x5C6350u /* cdecl int(const char *name): row of mp/uiNotifications.csv */
#define VA_SET_FLAG      0x5952E0u /* usercall: esi = playerdata property; (int index, int value); marks the profile dirty */

typedef char(__cdecl *check_fn)(void);
static check_fn g_orig_check;
static volatile LONG g_decided; /* the first onboarding check ran, or the fallback took over */

static int skip_enabled(void)
{
    char path[MAX_PATH];
    _snprintf(path, sizeof path, "%sr1-mod_noskip.txt", g_dir);
    return GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES;
}

static void set_notification_flag(const char *name)
{
    static const char prop[] = "uiNotificationFlags";
    int index = ((int(__cdecl *)(const char *))A(VA_NOTIFY_INDEX))(name);
    void *fn = (void *)A(VA_SET_FLAG);
    __asm {
        push 1
        push index
        lea esi, prop
        call fn
        add esp, 8
    }
}

static char __cdecl onboard_check_hook(void)
{
    if (!InterlockedExchange(&g_decided, 1)) {
        pd_profile_ready();
        if (g_orig_check())
            out("r1-mod: onboarding already complete in the profile\n");
        else if (skip_enabled()) {
            set_notification_flag("onboard_complete");
            out("r1-mod: onboarding skipped (onboard_complete set in the profile)\n");
        }
    }
    return g_orig_check();
}

int onboarding_install(void)
{
    return patch_call(VA_ONBOARD_SITE, VA_ONBOARD_CHECK, (void *)onboard_check_hook, (void **)&g_orig_check);
}

/* installed_at_load: what DllMain got; after a late injection the check has usually run already */
void onboarding_report(int installed_at_load)
{
    if (!installed_at_load)
        out("r1-mod: onboarding check call not recognised or late injection, using the dvar 3233 fallback\n");
}

static void skip_onboarding(void)
{
    Cbuf_AddText("set 3233 1\n");
    Sleep(1500);
    Cbuf_AddText("xcancelconnectingdialog\n");
    Cbuf_AddText("switchtomainmenu\n");
}

/* ---- default key bindings ----
 * The lobby warns "检测到有重要操作命令未绑定按键" (an important command has no key) when a command of the settings
 * list has no binding. The engine's own defaults bind `r` to IW4's combined +usereload, but CoDOL splits use
 * (+activate, `e`) from reload, and the settings list (dword_CE1E50, 41 commands, saved to playerdata keyBindings
 * by sub_463100 / loaded by sub_463270) only has +reload, which ends up unbound. Before the lobby opens, each
 * default below is bound when its command has no key and the key is free or holds the command it replaces.
 * Binding through the `bind` command marks the bindings dirty, so the engine writes them to playerdata and its
 * config, and they persist. Bind table: dword_1A2762C, 256 keys x 3 dwords for local client 0, first dword = index
 * into the command name table off_CE1CC0 (99 entries). */
#define VA_BIND_TABLE    0x1A2762Cu
#define VA_BIND_NAMES    0xCE1CC0u
#define BIND_NAME_COUNT  0x63
#define VA_BINDS_LOADED  0xCA0E090u /* byte, set once sub_463760 has synced bindings with playerdata */

static int bind_index(const char *cmd)
{
    const char *const *names = (const char *const *)A(VA_BIND_NAMES);
    int i;
    for (i = 1; i < BIND_NAME_COUNT; i++)
        if (names[i] && !_stricmp(names[i], cmd))
            return i;
    return 0;
}

static void apply_default_binds(void)
{
    static const struct { const char *cmd; int key; const char *key_name; const char *replaces; } defs[] = {
        {"+reload", 'r', "r", "+usereload"},
    };
    int d, k;
    __try {
        const int *table = (const int *)A(VA_BIND_TABLE);
        for (d = 0; d < (int)(sizeof defs / sizeof defs[0]); d++) {
            int want = bind_index(defs[d].cmd), old = bind_index(defs[d].replaces), held, bound = 0;
            char cmd[96];
            if (!want)
                continue;
            for (k = 0; k < 256; k++)
                if (table[3 * k] == want)
                    bound = 1;
            held = table[3 * defs[d].key];
            if (bound || (held != 0 && held != old))
                continue;
            _snprintf(cmd, sizeof cmd - 1, "bind %s \"%s\"\n", defs[d].key_name, defs[d].cmd);
            cmd[sizeof cmd - 1] = 0;
            Cbuf_AddText(cmd);
            out("r1-mod: bound %s to %s (was %s)\n", defs[d].key_name, defs[d].cmd,
                held ? defs[d].replaces : "free");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out("r1-mod: bind table not readable, default binds skipped\n");
    }
}

/* ---- room start ----
 * After matchmaking puts the player in a room, the matchmaking state machine (sub_6F10E0, state 5) starts the
 * match once dw_enterRoomCountingDown (10 s) has passed if dw_autoStartGame is set: the host broadcasts
 * StartCountingDown(0) and the "战斗开始" dialog covers the class editor. With it off the room waits; the room
 * host logic (sub_6E7260) still starts at once when every member is Ready (准备), and the local backend answers
 * the StartCountingDown with StartGame. r1-mod.ini [r1-mod] auto_start=1 restores the timer. */
/* dw_autoStartGame alone is not enough: when the room's dw_enterRoomCountingDown (10 s, registered 0..60) runs
 * out the player is marked Ready anyway, and a room whose members are all Ready starts (sub_6E7260). The dvar's
 * value is written directly, past its registered maximum, so the room waits for 准备 (room_wait seconds). */
static void apply_room_settings(void)
{
    char ini[MAX_PATH], cmd[64];
    unsigned char *dvar;
    int on, wait;
    _snprintf(ini, sizeof ini, "%sr1-mod.ini", g_dir);
    on = GetPrivateProfileIntA("r1-mod", "auto_start", 0, ini) != 0;
    wait = GetPrivateProfileIntA("r1-mod", "room_wait", 3600, ini);
    _snprintf(cmd, sizeof cmd - 1, "set dw_autoStartGame %d\n", on);
    cmd[sizeof cmd - 1] = 0;
    Cbuf_AddText(cmd);
    if (!on && (dvar = Dvar_FindVar("dw_enterRoomCountingDown")) != NULL && dvar[8] == 1 && wait > 0)
        *(float *)(dvar + 12) = (float)wait;
    out("r1-mod: room auto start %s (%s)\n", on ? "on" : "off",
        on ? "10 s after matchmaking" : "the room waits for Ready");
}

DWORD WINAPI onboarding_thread(LPVOID unused)
{
    int i, skip = skip_enabled();
    (void)unused;
    for (i = 0; i < 480 && dw_live_state() != 2; i++)
        Sleep(250);
    if (dw_live_state() != 2) {
        out("r1-mod: Demonware live state never reached 2, onboarding skip not run\n");
        if (!InterlockedExchange(&g_decided, 1))
            pd_release();
        return 0;
    }
    for (i = 0; i < 40 && !*(volatile unsigned char *)A(VA_BINDS_LOADED); i++)
        Sleep(250);
    Sleep(3000);
    apply_default_binds();
    apply_room_settings();
    /* the hooked onboarding check normally runs before this point; give it a little longer */
    for (i = 0; g_orig_check && i < 80 && !g_decided; i++)
        Sleep(250);
    if (InterlockedExchange(&g_decided, 1))
        return 0;
    if (g_orig_check)
        out("r1-mod: onboarding check not reached, using the dvar 3233 fallback\n");
    if (!skip) {
        out("r1-mod: onboarding skip disabled (r1-mod_noskip.txt)\n");
        pd_release();
        return 0;
    }
    skip_onboarding();
    out("r1-mod: onboarding skipped (dvar 3233 = 1, main menu)\n");
    /* let the operator screen finish whatever it writes, then put the saved classes back */
    Sleep(2000);
    pd_release();
    return 0;
}

/* cx_menu   run the onboarding skip by hand */
static void __cdecl cmd_cx_menu(void)
{
    skip_onboarding();
}

void onboarding_register(void)
{
    command_add("cx_menu", cmd_cx_menu);
}
