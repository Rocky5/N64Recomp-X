/* xhw_crash.c - CPU exception reporter (from OpenCrossing-Xbox xbox_crash.c).
 *
 * Every Xbox thread runs in kernel mode, so an unhandled page fault in game
 * code is a bugcheck: the console freezes or reboots and nothing reaches the
 * disk. xhw_crash_guard() puts an SEH registration record on the thread's
 * stack (fs:[0], what nxdk's own __try uses), so the kernel's dispatcher calls
 * on_exception() first. It writes the fault, the registers and the stack words
 * that point into the XBE to crash.log and to the screen, then parks the
 * thread. Symbolize with tools/xbox/sym.py and the build's n64recomp_x.map (n64rx keeps it in out/<game>/debug/).
 * Kill switch: -DXHW_CRASH_GUARD=0. */
#include <hal/debug.h>
#include <pbkit/pbkit.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xhw.h"
#include "xhw_internal.h"

#ifndef XHW_CRASH_GUARD
#define XHW_CRASH_GUARD 1
#endif

unsigned int xhw_image_base, xhw_image_end;

typedef struct Reg {
    struct Reg* prev;
    void* handler;
} Reg;

enum { DISP_CONTINUE_EXECUTION = 0, DISP_CONTINUE_SEARCH = 1 };

static volatile LONG s_in_crash;
static char s_rep[6144];
static int s_len;

static void rep(const char* fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(s_rep + s_len, sizeof s_rep - (size_t)s_len, fmt, ap);
    va_end(ap);
    if (n > 0) s_len += n;
    if (s_len > (int)sizeof s_rep - 1) s_len = (int)sizeof s_rep - 1;
}

static const char* code_name(ULONG c) {
    switch (c) {
        case 0xC0000005: return "access violation";
        case 0xC000001D: return "illegal instruction";
        case 0xC0000094: return "integer divide by zero";
        case 0xC0000095: return "integer overflow";
        case 0xC0000096: return "privileged instruction";
        case 0xC00000FD: return "stack overflow";
        case 0x80000003: return "breakpoint";
        default: return "exception";
    }
}

static void build_report(const EXCEPTION_RECORD* er, const CONTEXT* cx) {
    PKTHREAD t = KeGetCurrentThread();
    ULONG* sp = (ULONG*)cx->Esp;
    ULONG* top = (ULONG*)t->StackBase;
    int n = 0, i;

    s_len = 0;
    rep("[CRASH] %s (%08lx) at %08lx, frame %u, thread %p\n", code_name((ULONG)er->ExceptionCode),
        (unsigned long)er->ExceptionCode, (unsigned long)(ULONG)er->ExceptionAddress, xhw_frame_count(), (void*)t);
    if ((ULONG)er->ExceptionCode == 0xC0000005 && er->NumberParameters >= 2)
        rep("[CRASH] %s of address %08lx\n", er->ExceptionInformation[0] ? "write" : "read",
            (unsigned long)er->ExceptionInformation[1]);
    rep("[CRASH] eip %08lx esp %08lx ebp %08lx eflags %08lx\n", cx->Eip, cx->Esp, cx->Ebp, cx->EFlags);
    rep("[CRASH] eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx\n", cx->Eax, cx->Ebx, cx->Ecx, cx->Edx,
        cx->Esi, cx->Edi);
    rep("[CRASH] free %u KB\n", xhw_mem_free_kb());
    rep("[CRASH] stack:");
    if (sp && top && sp < top && top - sp < 0x40000) {
        for (; sp < top && n < 48; sp++) {
            ULONG v = *sp;
            if (v >= xhw_image_base + 0x1000 && v < xhw_image_end) {
                rep(" %08lx", v);
                n++;
            }
        }
    }
    rep("\n[CRASH] raw:");
    sp = (ULONG*)cx->Esp;
    for (i = 0; i < 16 && sp && top && sp + i < top; i++) rep(" %08lx", sp[i]);
    rep("\n[CRASH] end\n");
}

static void write_crash_log(void) {
    static char tail[4096];
    HANDLE h = CreateFileA(XHW_UDATA_DIR "crash.log", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           NULL);
    DWORD w;
    size_t tl;
    if (h == INVALID_HANDLE_VALUE) return;
    tl = xhw_log_tail(tail, sizeof tail);
    WriteFile(h, tail, (DWORD)tl, &w, NULL);
    WriteFile(h, s_rep, (DWORD)s_len, &w, NULL);
    xhw_flush_handle(h);
    CloseHandle(h);
}

static void show_screen(void) {
    char* line = s_rep;
    pb_show_debug_screen();
    debugClearScreen();
    debugPrint(XHW_TITLE " crashed. Please report it with\n");
    debugPrint(XHW_UDATA_DIR "crash.log\n\n");
    while (*line) {
        char* nl = strchr(line, '\n');
        char buf[112];
        int len = nl ? (int)(nl - line) : (int)strlen(line);
        if (len > (int)sizeof buf - 1) len = (int)sizeof buf - 1;
        memcpy(buf, line, (size_t)len);
        buf[len] = '\0';
        debugPrint("%s\n", buf);
        if (!nl) break;
        line = nl + 1;
    }
    debugPrint("\nHold the power button to switch off.\n");
}

/* ---- a write watch for diagnosis (-DXRC_WATCH=<N64 address> in the runtime,
 * mem.c): DR0 on the word's host address, writes only; every thread runs in
 * ring 0 under this guard, so each write raises a single-step exception here,
 * which logs the writer and the new value and carries on (the first 200). ---- */
static void (*s_extra)(void);

void xhw_crash_set_extra(void (*fn)(void)) { s_extra = fn; }

/* an access fault the runtime may resolve before it is a crash: the N64's
 * TLB miss (rt/tlb.c: the game's refill vector or its fault thread maps the
 * page, which is then aliased), retried when it returns 1. Not re-entered on
 * the thread already in it (a fault inside the fixup is a crash). */
static int (*s_fixup)(uint32_t host, int write);
static volatile PKTHREAD s_fixup_owner[8];
void xhw_crash_set_fixup(int (*fn)(uint32_t host, int write)) { s_fixup = fn; }

static int try_fixup(const EXCEPTION_RECORD* er) {
    PKTHREAD me = KeGetCurrentThread();
    int i, slot = -1, ok;
    if (!s_fixup || (ULONG)er->ExceptionCode != 0xC0000005u || er->NumberParameters < 2) return 0;
    if (KeGetCurrentIrql() >= DISPATCH_LEVEL) return 0;
    for (i = 0; i < 8; i++)
        if (s_fixup_owner[i] == me) return 0;   /* faulted inside the fixup */
    for (i = 0; i < 8 && slot < 0; i++)
        if (InterlockedCompareExchange((LONG*)&s_fixup_owner[i], (LONG)me, 0) == 0) slot = i;
    if (slot < 0) return 0;
    ok = s_fixup((uint32_t)er->ExceptionInformation[1], er->ExceptionInformation[0] != 0);
    s_fixup_owner[slot] = NULL;
    return ok;
}

static volatile uint32_t s_watch;
static volatile LONG s_watch_hits;

void xhw_watch_write(uint32_t host) {
    uint32_t dr7 = 0x1u | (0x1u << 16) | (0x3u << 18);   /* L0, RW0 = write, LEN0 = 4 bytes */
    s_watch = host;
    __asm__ volatile("movl %0, %%dr0" : : "r"(host));
    __asm__ volatile("movl %0, %%dr7" : : "r"(dr7));
    xhw_logf("[WATCH] writes to %08x", (unsigned)host);
}

#ifndef XHW_WATCH_IGNORE
#define XHW_WATCH_IGNORE 0u
#endif
static int on_watch(CONTEXT* cx) {
    ULONG* sp = (ULONG*)cx->Esp;
    ULONG up[4] = {0, 0, 0, 0};
    int n = 0, i;
    static volatile uint32_t s_last = 0xDEADBEEFu;
    uint32_t v = *(volatile uint32_t*)s_watch;
    LONG k;
    __asm__ volatile("movl %0, %%dr6" : : "r"(0u));
    if (v == s_last || ((v ^ s_last) & ~(uint32_t)(XHW_WATCH_IGNORE)) == 0)
        return DISP_CONTINUE_EXECUTION;   /* only changes are logged, outside the -DXHW_WATCH_IGNORE bits */
    s_last = v;
    k = InterlockedIncrement((LONG*)&s_watch_hits);
    for (i = 0; i < 64 && n < 4; i++)
        if (sp[i] >= xhw_image_base + 0x1000 && sp[i] < xhw_image_end) up[n++] = sp[i];
    if (k <= 200)
        xhw_logf("[WATCH] %08x = %08x by eip %08lx (up %08lx %08lx %08lx %08lx) thread %p at %u ms", (unsigned)s_watch,
                 (unsigned)*(volatile uint32_t*)s_watch, cx->Eip, up[0], up[1], up[2], up[3], (void*)KeGetCurrentThread(),
                 (unsigned)(xhw_time_us() / 1000u));
    __asm__ volatile("movl %0, %%dr6" : : "r"(0u));
    return DISP_CONTINUE_EXECUTION;
}

__attribute__((cdecl)) static int on_exception(EXCEPTION_RECORD* er, void* frame, CONTEXT* cx, void* dc) {
    (void)frame;
    (void)dc;
    if (er->ExceptionFlags & EXCEPTION_UNWIND) return DISP_CONTINUE_SEARCH;
    if (s_watch && (ULONG)er->ExceptionCode == 0x80000004u) return on_watch(cx);
    if (try_fixup(er)) return DISP_CONTINUE_EXECUTION;
    if (InterlockedExchange((LONG*)&s_in_crash, 1)) {
        if (KeGetCurrentIrql() < DISPATCH_LEVEL)
            for (;;) Sleep(1000);
        for (;;) {}
    }
    build_report(er, cx);
    show_screen();
    if (KeGetCurrentIrql() >= DISPATCH_LEVEL) {
        xhw_com1_raw("[CRASH] at raised IRQL\n", 24);
        xhw_com1_raw(s_rep, (size_t)s_len);
        for (;;) {}   /* no waits or file I/O at raised IRQL */
    }
    xhw_log(s_rep);
    if (s_extra) s_extra();   /* the runtime's own (a trace build's functions), into the log's tail */
    write_crash_log();
    xhw_exit_write("crash (crash.log)");
    for (;;) Sleep(1000);
    return DISP_CONTINUE_SEARCH;
}

__attribute__((noinline)) void xhw_crash_guard(void (*fn)(void*), void* arg) {
    Reg r;
    if (!XHW_CRASH_GUARD) {
        fn(arg);
        return;
    }
    r.handler = (void*)on_exception;
    if (s_watch) {   /* the debug registers are per thread: a watch set before covers this one too */
        uint32_t dr7 = 0x1u | (0x1u << 16) | (0x3u << 18);
        __asm__ volatile("movl %0, %%dr0" : : "r"(s_watch));
        __asm__ volatile("movl %0, %%dr7" : : "r"(dr7));
    }
    __asm__ volatile("movl %%fs:0, %0" : "=r"(r.prev));
    __asm__ volatile("movl %0, %%fs:0" : : "r"(&r) : "memory");
    fn(arg);
    __asm__ volatile("movl %0, %%fs:0" : : "r"(r.prev) : "memory");
}
