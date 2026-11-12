/* Interfaces between the r1-mod client components. main.c installs them in a fixed order
 * (see main_thread); each component registers its own console commands. */
#ifndef R1_COMPONENTS_H
#define R1_COMPONENTS_H

#include "std_include.h"

/* command.c: commands registered into the game's own command lists */
typedef void(__cdecl *command_fn)(void);
void command_add(const char *name, command_fn fn);
void command_register_all(void);       /* links every added command into the engine's command lists */
int command_engine_count(void);        /* commands in the engine list (g_engine->cmd_head) */

/* console.c: log, console window, line runner, game message mirror */
void out(const char *fmt, ...);
void console_open_log(void);
void console_open_window(void);
void console_register(void);
void console_run_line(char *line, int echo);
int console_install_gamemsg_hooks(void); /* number of call sites hooked */
int console_gamemsg_site_count(void);
DWORD WINAPI console_input_thread(LPVOID unused);
DWORD WINAPI console_file_thread(LPVOID unused);

/* trace.c: cx_trace (Com_sprintf output) */
int trace_install(void);
void trace_register(void);

/* render.c: version banner (2D frame) and the overlay's end-of-frame hook */
int render_install_frame_hook(void);
int render_install_endframe_hook(void);
void render_register(void);

/* name.c: player name */
void name_register(void);
DWORD WINAPI name_thread(LPVOID unused);

/* direct_connect.c: connect <ip>[:port] over plain UDP */
int direct_connect_install(void);
void direct_connect_register(void);
int parse_target(const char *arg, unsigned *ip, unsigned short *port);

/* redirects.c: loopback STUN/connect redirects; returns 1 = connect, 2 = STUN table */
int redirects_install(void);
void redirects_report(int installed_at_load);

/* scaleform.c: UI files (Scaleform movies) from <dll dir>\ui\ before the zones */
int scaleform_install(void);
void scaleform_report(int installed_at_load);

/* fsmod.c: client mods, <mods_dir>\<client_mod> on top of the file system's search paths (DllMain) */
int fsmod_install(void);                   /* call sites hooked (2 when recognised) */
void fsmod_report(int sites);
const char *fsmod_dir(void);               /* the loaded client mod's folder, NULL when none */
void fsmod_register(void);
void fsmod_start(void);                    /* queues the r1_clientMod dvar registration on the game thread */
void fsmod_keep(void);                     /* timescale guard: follow the server's r1_clientMod */

/* intro.c: start-up logo videos (DllMain; -1 = not asked for, else sites patched) */
int intro_install(void);
void intro_report(int patched);

/* onboarding.c: onboarding skip, default binds, room settings */
int onboarding_install(void);              /* hooks the engine's onboarding check (DllMain) */
void onboarding_report(int installed_at_load);
void onboarding_register(void);
DWORD WINAPI onboarding_thread(LPVOID unused);

/* playerdata.c: persistent playerdata */
unsigned char *pd_buf(void);
int pd_get_int(int off);
void pd_set_int(int off, int value);
void pd_release(void);                     /* fallback: load and save from now on */
void pd_profile_ready(void);               /* the engine's profile is in place: apply the file now */
void playerdata_register(void);
DWORD WINAPI playerdata_thread(LPVOID unused);

/* operators.c: cx_operator */
void operators_register(void);

/* unlock.c: every item owned */
void unlock_startup(void);
void unlock_register(void);

/* stats.c: secure stats overlay */
void stats_startup(void);
void stats_register(void);
DWORD WINAPI stats_thread(LPVOID unused);

/* checksum.c: configstring checksum check */
void checksum_startup(void);
void checksum_register(void);

/* match.c: cx_match */
void match_register(void);
const char *match_last_target(void);       /* "ip port" of the last join, "" before one */

/* fps.c / turn_speed.c: kept dvar values, re-applied by timescale.c */
void fps_load_config(void);
void fps_keep(void);
void fps_register(void);
void turn_speed_load_config(void);
void turn_speed_keep(void);
void turn_speed_register(void);

/* timescale.c: time scale guard */
DWORD WINAPI timescale_guard(LPVOID unused);

/* rcon.c: cx_rcon */
int rcon_exec(const char *line, char *reply, size_t reply_size); /* blocking; 1 = the server answered */
void rcon_register(void);
void chat_register(void); /* chat.c: the chat box and message mode send say to the game server in a match */
void compat_register(void);                 /* compat.c: r1_compat in the connect userinfo, cx_compat */
void compat_server_answer(const char *server_compat); /* the server's r1_compat from getinfo ("" = not declared) */
void textcolor_register(void);              /* textcolor.c: ^: rainbow and ^; server colour in text */
void textcolor_set_custom(const char *rgb); /* "r g b" 0..1, the server's sv_customTextColor */

/* weapon_anims.c: bg_weaponSwapStyle, IW4 quick drop/raise clips (set by the server) */
void weapon_anims_register(void);
void weapon_anims_start(void);             /* queues the install on the game thread */
void weapon_anims_keep(void);              /* timescale guard: style 0 again in the lobby */
/* match.c: the value slot of a loaded localized string (LocalizeEntry, asset type 27), NULL when not loaded */
const char **localize_slot(const char *key);
void localize_register(void);              /* localize.c: r1_english.txt over the game's Chinese strings */
void localize_keep(void);                  /* timescale guard: queue a re-apply when the connection state changes */

/* mod.c: cx_mod (GSC mods on the local server) */
void mod_register(void);

/* movement.c: iw4x movement dvars in the client's prediction (the server sends their values) */
void movement_register(void);
void movement_start(void);                 /* queues the install on the game thread */
void movement_keep(void);                  /* timescale guard: defaults back in the lobby */

#endif
