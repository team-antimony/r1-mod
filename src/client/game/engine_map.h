/* Engine reference map for the CoD Online 3.3.3.3 QA build (qa3333.exe, IDA base 0x400000).
 *
 * This is the researched engine API as code: named addresses with what each routine is, so the
 * knowledge lives in the repository rather than in a private disassembly. These are REFERENCE
 * constants for anyone extending the mod; a component that actually calls one of these still names
 * it locally (see the convention in game.h). Every address is an IDA address and must be rebased at
 * runtime with A() from game.h, because the image relocates (ASLR).
 *
 * Deliberately not here: the Demonware transport, address-discovery and connection path (they live
 * with the component that uses them, e.g. direct_connect.c), the IFS file layer, and the WinMain
 * start-up gate. Those are not part of the generic mod API.
 *
 * Dvar value layout (confirmed live): name pointer at dvar+0x00, type byte at dvar+0x08
 * (0 bool, 1 float, 5 int, 6 enum, 7 string), value dword at dvar+0x0C. */
#ifndef R1_ENGINE_MAP_H
#define R1_ENGINE_MAP_H

/* --- commands --- */
#define EA_CMD_FIND_COMMAND     0x58E410u /* __usercall name in esi; walks the command-list head {next, name, func} */
#define EA_CMD_EXECUTE_TOKENIZED 0x58D910u /* runs a tokenized command line against the dispatch list */

/* --- dvars: registration (type in the last-but-one column of each row) --- */
#define EA_DVAR_REGISTER_BOOL   0x633A00u /* cdecl dvar *(name, char value, flags, desc); type 0 */
#define EA_DVAR_REGISTER_INT    0x633AB0u /* cdecl dvar *(name, value, min, max, flags, desc); type 5 */
#define EA_DVAR_REGISTER_FLOAT  0x633B60u /* cdecl dvar *(name, float value, min, max, flags, desc); type 1 */
#define EA_DVAR_REGISTER_STRING 0x633DD0u /* wraps Dvar_RegisterVariant with type 7 */
#define EA_DVAR_REGISTER_VARIANT 0x633950u /* cdecl dvar *(name, type, flags, ...); finds or creates */

/* --- dvars: lookup and set --- */
#define EA_DVAR_FIND            0x632440u /* cdecl dvar *(name), case-insensitive; also game.h VA_DVAR_FIND */
#define EA_DVAR_SET_VALUE_CORE  0x632150u /* cdecl (dvar, value, ...); validates then stores */
#define EA_DVAR_SET_FLOAT       0x6341C0u /* __usercall dvar in esi */
#define EA_DVAR_SET_INT         0x634480u /* __usercall dvar in esi */
#define EA_DVAR_SET_STRING      0x6343A0u /* __usercall dvar in esi */

/* --- errors and formatting --- */
#define EA_COM_ERROR            0x597D20u /* cdecl (code, fmt, ...); ~592 callers */
#define EA_SYS_FATAL_ERROR      0x63F9B0u /* noreturn; shows a message box */
#define EA_COM_SPRINTF          0x637BE0u /* cdecl (buf, size, fmt, ...), a _vsnprintf wrapper; ~526 callers */
#define EA_VA                   0x637C60u /* formats into a per-thread 2x1024-byte ring buffer; ~483 callers */

/* --- assets and content (asset types: 36 rawfile, 37 stringtable, 39 structured-data def) --- */
#define EA_DB_REQUEST_ASSET     0x49E7D0u /* cdecl void *(assetType, name, ...); also game.h VA_DB_REQUEST */
#define EA_DB_FIND_ASSET        0x49EC70u /* cdecl void *(assetType, name); lookup without a load */
#define EA_DB_LOAD_FASTFILE     0x4A0430u /* loads a fastfile by name (%s.ff); handles patch/ez_ prefixes */
#define EA_SCR_LOAD_FX_SCRIPT   0x435E20u /* loads maps/mp/%s_fx.gsc */
#define EA_SCR_LOAD_CREATEFX    0x435920u /* loads maps/createfx/%s_fx.gsc */
#define EA_PLAYLIST_PARSE       0x6111D0u /* parses the playlist publisher file */
#define EA_PLAYLIST_FF_PATH     0x592290u /* builds client_shipRetail\<lang>\mp_playlists.ff */

/* --- onboarding --- */
#define EA_IS_ONBOARD_COMPLETE  0x5C36F0u /* true when stat onboard_complete is set or dvar "3233" is on */

#endif
