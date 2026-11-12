/* r1-mod overlay: an in-game console and a server browser drawn with Dear ImGui over the
 * game's Direct3D 9 device. The QA build compiled IW4's console out, so both are r1-mod's own.
 *
 * overlay.cpp is C++, the rest of the client is C; this header is the whole interface between them. */
#ifndef R1_OVERLAY_H
#define R1_OVERLAY_H

#include <stdint.h>

#include "version.h"
#include "compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* provided by console.c */
uintptr_t r1_addr(unsigned ida_va);                 /* IDA address -> runtime address */
void r1_log(const char *fmt, ...);                  /* r1-mod.log, the console window and the overlay */
void r1_exec(const char *line);                     /* the console line runner (dvar, cmdlist, help, Cbuf) */
const char *r1_dir(void);                           /* folder of r1-mod.dll, ends with a backslash */
/* Calls back once per engine command (value NULL) and dvar (value formatted) whose name starts
 * with prefix, case-insensitively, stopping after max matches. Returns the number of matches. */
int r1_complete(const char *prefix, int max, void (*cb)(const char *name, const char *value, void *ctx),
                void *ctx);

/* provided by stats.c: a read-only view for the stats panel (safe on the render thread) */
struct stats_view {
    int on, level, max_level, prestige, max_prestige, xp, flags, weapon_xp, gate_flags, overrides, replaced, tables;
};
struct stats_flag_row {
    char name[64];
    int index, live, profile, override; /* profile / override: -1 when the overlay has none */
};
void stats_get_view(struct stats_view *v);
int stats_find_flags(const char *text, struct stats_flag_row *rows, int max);

/* provided by textcolor.c */
void textcolor_set_custom(const char *rgb);         /* "r g b" 0..1 for ^; text (the server's sv_customTextColor) */
/* provided by compat.c */
void compat_server_answer(const char *server_compat); /* the server's r1_compat from getinfo ("" = not declared) */

/* provided by overlay.cpp */
void overlay_start(void);                           /* reads r1-mod.ini */
void overlay_render_scene(void);                    /* render thread, inside the frame's last scene */
void overlay_print(const char *text);               /* appends text (may hold several lines) to the console */
void overlay_game_frame(void);                      /* main thread, once per frame: input focus upkeep */
void overlay_toggle_browser(void);                  /* cx_browser */
void overlay_toggle_stats(void);                    /* cx_statspanel */
int overlay_frame_times(float *out, int max);       /* last drawn-frame intervals in ms, oldest first (cx_frametime) */
void overlay_frame_times_reset(void);
/* asks the server being joined for its name and message of the day (sv_motd), shown as the loading-screen tip */
void overlay_motd_query(const char *ip, unsigned short port);
/* 1 and the last answer's name and message (UTF-8, colour codes kept) when one arrived for the current join */
int overlay_motd_get(char *host, int host_size, char *motd, int motd_size);

#ifdef __cplusplus
}
#endif

#endif
