/* r1-mod's compatibility number with r1-server, shared by the client DLL and the launcher.
 * Raised by one whenever what r1-mod expects from a server, or sends to it, changes in a way the other side has to
 * know about. docs/COMPATIBILITY.md lists every item and the number it arrived in; tools/check_compat.py checks that
 * the two agree. */
#ifndef R1_COMPAT_H
#define R1_COMPAT_H

#define R1_COMPAT 1

#endif
