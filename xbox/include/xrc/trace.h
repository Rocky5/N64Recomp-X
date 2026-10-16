/* trace.h - N64Recomp's trace_mode hooks (n64rx: N64RX_TRACE=1, a diagnosis
 * build; docs/testing.md). The generator puts TRACE_ENTRY() at the top of
 * every function and TRACE_RETURN() before every return; the runtime keeps
 * the function names entered last in a ring (rt/trace.c), which the crash
 * guard logs with the crash ([TRACE]). */
#ifndef XRC_TRACE_H
#define XRC_TRACE_H

void xrc_trace_enter(const char* name);

#define TRACE_ENTRY() xrc_trace_enter(__func__);
#define TRACE_RETURN()

#endif
