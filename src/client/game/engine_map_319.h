/* Engine reference map for the CoD Online 3.19.26.22 retail build (file version 1649370, IDA base 0x400000).
 *
 * Same purpose as engine_map.h, for the second build r1-mod recognises. The addresses are those of the
 * client's unpacked code; the exe has no relocations, so they are also the runtime addresses (A() adds 0).
 * r1-mod uses only the command and dvar rows so far (game.c); the rest is reference for porting the other
 * components. This build names nothing in its messages (Com_Error takes numeric ids such as "897"), so the
 * map was made from structure: the command executor, the dvar registrars and their callers.
 *
 * Deliberately not here, as in engine_map.h: the Demonware transport, the file layer and the WinMain
 * start-up gate.
 *
 * Dvar layout (0x4C bytes): name +0x00, flags +0x04, type +0x08 (0 bool, 1 float, 2-4 vec2-vec4, 5 int,
 * 6 enum, 7 string, 8 color), modified +0x09, current +0x0C, latched +0x1C, reset +0x2C, domain +0x3C,
 * change callback +0x44, hash chain next +0x48. Name, type and value sit where QA 3.3.3.3 has them. */
#ifndef R1_ENGINE_MAP_319_H
#define R1_ENGINE_MAP_319_H

/* --- command buffer --- */
#define EA319_CMD_TEXT           0x76595A4u /* cmd_text[]: {char *data, int maxsize, int cmdsize}, 12 bytes each; [0] is the console's */
#define EA319_CMD_TEXT_LOCK      0x81A55A0u /* CRITICAL_SECTION held around every cmd_text access */
#define EA319_CBUF_INSERT_TEXT   0x4F2BC0u  /* __usercall text in edi; puts a line at the front of cmd_text[0] */
#define EA319_CBUF_EXECUTE       0x4F2EC0u  /* cdecl (buffer, source); splits on ; and newline, runs each command */
/* Cbuf_AddText is not a function in this build: it is inlined at about 49 call sites (copy to data + cmdsize
 * under the lock). game.c appends the same way. */

/* --- commands --- */
#define EA319_CMD_FUNCTIONS      0x33303B0u /* the one command list: node {next, name, func}; the executor moves a hit to the front */
#define EA319_CMD_ARGC           0x3306994u /* int[], indexed by [EA319_CMD_LEVEL] */
#define EA319_CMD_ARGV_TABLE     0x33069D4u /* const char **[], indexed by [EA319_CMD_LEVEL] */
#define EA319_CMD_LEVEL          0x3306910u
#define EA319_CMD_ARGV           0x443F60u  /* cdecl const char *(int) */
#define EA319_CMD_ARGS           0x4F34D0u  /* __usercall first in eax, out in ecx, (size): joins the arguments */
#define EA319_CMD_TOKENIZE       0x4F36C0u  /* fills argc / argv for a new level */
#define EA319_CMD_END_TOKENIZE   0x4F3760u  /* drops the level again */
#define EA319_CMD_EXECUTE_SINGLE 0x4F3890u  /* cdecl (buffer, source, text): tokenize, find in the list, call */
#define EA319_CMD_FIND_COMMAND   0x4F37A0u  /* __usercall name in esi */
#define EA319_CMD_ADD_INTERNAL   0x4F37F0u  /* cdecl (name, func, node); no-op when the name exists */
#define EA319_CMD_REMOVE         0x4F3820u  /* cdecl (name) */
#define EA319_DVAR_COMMAND       0x66C1D0u  /* "<dvar> [value]" typed as a command: the executor's fallback */

/* --- dvars: storage --- */
#define EA319_DVAR_HASH          0x80F8E30u /* dvar *[1024] by name hash; chain next at +0x48 */
#define EA319_DVAR_POOL          0x80F9E30u /* dvar[4096], 0x4C bytes each */
#define EA319_DVAR_COUNT         0x80F4E10u

/* --- dvars: registration (find or create; an existing dvar is re-registered) --- */
#define EA319_DVAR_REGISTER_BOOL    0x770890u /* cdecl dvar *(name, char value, flags, desc); type 0 */
#define EA319_DVAR_REGISTER_INT     0x770940u /* cdecl dvar *(name, value, min, max, flags, desc); type 5 */
#define EA319_DVAR_REGISTER_FLOAT   0x7709F0u /* cdecl dvar *(name, float value, min, max, flags, desc); type 1 */
#define EA319_DVAR_REGISTER_STRING  0x770C60u /* cdecl dvar *(name, value, flags, desc); type 7 */
#define EA319_DVAR_REGISTER_VEC3    0x770A90u /* type 3 */
#define EA319_DVAR_REGISTER_VEC4    0x770BB0u /* type 4 */
#define EA319_DVAR_REGISTER_ENUM    0x770CB0u /* cdecl dvar *(name, const char **values, default, flags, desc); type 6 */
#define EA319_DVAR_REGISTER_COLOR   0x770D60u /* cdecl dvar *(name, r, g, b, a, flags, desc); type 8 */
#define EA319_DVAR_REGISTER_VARIANT 0x7707E0u /* cdecl dvar *(name, type, flags, value..., desc) */

/* --- dvars: lookup, read and set --- */
#define EA319_DVAR_FIND             0x76F190u /* cdecl dvar *(name), case-insensitive */
#define EA319_DVAR_GET_BOOL         0x76F270u /* cdecl bool(name) */
#define EA319_DVAR_GET_INT          0x76F2B0u /* cdecl int(name) */
#define EA319_DVAR_GET_STRING       0x76F330u /* __usercall name in eax */
#define EA319_DVAR_SET_VARIANT      0x76EDF0u /* cdecl (dvar, value[4], source): the setter every other one ends in */
#define EA319_DVAR_SET_INT          0x771320u /* cdecl (dvar, int) */
#define EA319_DVAR_SET_FLOAT        0x771060u /* __usercall dvar in esi, (float, source) */
#define EA319_DVAR_SET_STRING       0x771240u /* __usercall dvar in esi, (const char *) */
#define EA319_DVAR_SET_FROM_STRING  0x7713A0u /* (string, source); dvar in a register */
#define EA319_DVAR_SET_COMMAND      0x7716D0u /* by name, from text; what "<dvar> <value>" in the console ends in */

/* --- errors and formatting --- */
#define EA319_COM_ERROR             0x4F82E0u /* cdecl (code, fmt, ...); fmt is a numeric id plus arguments */
#define EA319_COM_SPRINTF           0x774900u /* cdecl (buf, size, fmt, ...) */
#define EA319_VA                    0x774980u
#define EA319_COM_EXEC_CONFIG       0x4F6A90u /* queues "exec config_mp.cfg" */

/* --- client --- */
#define EA319_CL_CONNECT_TO_HOST    0x470360u
#define EA319_CL_PARSE_GAMESTATE    0x473180u
#define EA319_CL_CHECK_FOR_RESEND   0x5C05F0u

#endif
