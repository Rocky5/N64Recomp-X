/* xrc_prelude.h - included first in every game-side unit (the runtime, the
 * interpreter, the generated code; tools/xbox/compile_game.py -include).
 *
 * The game triple is i686-pc-windows-gnu, where __int64 is not a keyword,
 * but nxdk's libc headers (stdlib_ext_.h) spell it (Wings64-X's prelude). */
#ifndef XRC_PRELUDE_H
#define XRC_PRELUDE_H
#define __int64 long long
#endif
