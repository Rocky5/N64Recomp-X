/* xrc_prelude.h - included first in every game-side unit (the runtime, the
 * interpreter, the generated code; tools/xbox/compile_game.py -include).
 *
 * The game triple is i686-pc-windows-gnu, where __int64 is not a keyword,
 * but nxdk's libc headers (stdlib_ext_.h) spell it (Wings64-X's prelude). */
#ifndef XRC_PRELUDE_H
#define XRC_PRELUDE_H
#define __int64 long long
/* memcpy, memmove, memset and memcmp as builtins here too (as xbuiltin.h does for the hw units; sqrtf and sqrt are
 * recomp.h's): the game side is built freestanding, so a fixed-size memcpy was a call to xhw_string.c's `rep movsb`. The
 * renderer's texture hashes read each word with memcpy(&w, p, 4): the console's profile of a WWF No Mercy 4-wrestler match
 * had memcpy at 7.4%, more than the display-list loop. -DXRC_GAME_BUILTINS=0: real calls, as before */
#ifndef XRC_GAME_BUILTINS
#define XRC_GAME_BUILTINS 1
#endif
#if XRC_GAME_BUILTINS
#include <string.h>
#define memcpy(d, s, n) __builtin_memcpy((d), (s), (n))
#define memmove(d, s, n) __builtin_memmove((d), (s), (n))
#define memset(d, c, n) __builtin_memset((d), (c), (n))
#define memcmp(a, b, n) __builtin_memcmp((a), (b), (n))
#endif
#endif
