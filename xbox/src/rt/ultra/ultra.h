/* ultra.h - shared by the libultra surface (xbox/src/rt/ultra, PLAN.md 3.3).
 *
 * The recompiler renames every libultra function in its reimplemented list
 * to <name>_recomp and never recompiles it; these files provide them.
 * Upstream's convention, kept: arguments in ctx->r4..r7, then at 0x10(sp)
 * onwards; 64-bit arguments in an aligned register pair (high word first,
 * O32); results in ctx->r2 (and r3 for a 64-bit result's low word).
 *
 * libultra's objects (OSThread, OSMesgQueue, OSTimer, OSIoMesg, OSTask,
 * OSContPad ...) live in the game's RDRAM in N64 layout, because game code
 * reads their fields inline (validCount, priority, task headers). They are
 * read and written here through the accessors below, on the word-swapped
 * image (words native, halfwords at ^2, bytes at ^3). Host state (kernel
 * threads, events, timers) hangs off side tables keyed by the struct's N64
 * address. Wings64-X's os_shim.c is the model; what changed is that data
 * model, and that the game's N64 stacks are real (they are RDRAM). */
#ifndef XRC_ULTRA_H
#define XRC_ULTRA_H

#include <stdint.h>
#include <string.h>

#include "rt.h"
#include "xhw.h"

/* ---- RDRAM by N64 address (KSEG0, KSEG1 or physical) ---- */
static inline uint8_t* xu_host(uint32_t v) { return xrc_rdram_host + ((v & 0x1FFFFFFFu) & (xrc_rdram_size - 1)); }
static inline uint32_t rd32(uint32_t v) { return *(uint32_t*)xu_host(v); }
static inline void wr32(uint32_t v, uint32_t x) { *(uint32_t*)xu_host(v) = x; }
static inline uint16_t rd16(uint32_t v) { return *(uint16_t*)(xrc_rdram_host + ((((v & 0x1FFFFFFFu) ^ 2)) & (xrc_rdram_size - 1))); }
static inline void wr16(uint32_t v, uint16_t x) { *(uint16_t*)(xrc_rdram_host + ((((v & 0x1FFFFFFFu) ^ 2)) & (xrc_rdram_size - 1))) = x; }
static inline uint8_t rd8(uint32_t v) { return xrc_rdram_host[((v & 0x1FFFFFFFu) ^ 3) & (xrc_rdram_size - 1)]; }
static inline void wr8(uint32_t v, uint8_t x) { xrc_rdram_host[((v & 0x1FFFFFFFu) ^ 3) & (xrc_rdram_size - 1)] = x; }
static inline uint64_t rd64(uint32_t v) { return ((uint64_t)rd32(v) << 32) | rd32(v + 4); }
static inline void wr64(uint32_t v, uint64_t x) {
    wr32(v, (uint32_t)(x >> 32));
    wr32(v + 4, (uint32_t)x);
}
/* bytes in N64 order to and from the image */
void xu_to_rdram(uint32_t dst, const uint8_t* src, uint32_t n);
void xu_from_rdram(uint8_t* dst, uint32_t src, uint32_t n);

/* ---- arguments and results (O32) ---- */
static inline uint32_t xu_arg(recomp_context* ctx, int i) {
    switch (i) {
    case 0: return (uint32_t)ctx->r4;
    case 1: return (uint32_t)ctx->r5;
    case 2: return (uint32_t)ctx->r6;
    case 3: return (uint32_t)ctx->r7;
    }
    return rd32((uint32_t)ctx->r29 + 0x10 + 4 * (uint32_t)(i - 4));
}
#define ARG(i) xu_arg(ctx, (i))
/* a 64-bit argument in slots i and i+1 (i even: a0/a1, a2/a3, or the stack) */
static inline uint64_t xu_arg64(recomp_context* ctx, int i) {
    return ((uint64_t)xu_arg(ctx, i) << 32) | xu_arg(ctx, i + 1);
}
#define RET(x) (ctx->r2 = (gpr)(int32_t)(uint32_t)(x))
#define RET64(x) do { uint64_t r64_ = (x); ctx->r2 = (gpr)(int32_t)(uint32_t)(r64_ >> 32); \
                      ctx->r3 = (gpr)(int32_t)(uint32_t)r64_; } while (0)
#define ULTRA(name) void name##_recomp(uint8_t* rdram, recomp_context* ctx)

/* ---- libultra's structs: field offsets (O32, libultra 2.0) ---- */
enum {
    MQ_MTQUEUE = 0, MQ_FULLQUEUE = 4, MQ_VALIDCOUNT = 8, MQ_FIRST = 12, MQ_MSGCOUNT = 16, MQ_MSG = 20,
    TH_NEXT = 0, TH_PRIORITY = 4, TH_QUEUE = 8, TH_TLNEXT = 12, TH_STATE = 16, TH_FLAGS = 18, TH_ID = 20,
    TM_NEXT = 0, TM_PREV = 4, TM_INTERVAL = 8, TM_VALUE = 16, TM_MQ = 24, TM_MSG = 28,
    IO_TYPE = 0, IO_PRI = 2, IO_RETQUEUE = 4, IO_DRAMADDR = 8, IO_DEVADDR = 12, IO_SIZE = 16,
    TK_TYPE = 0, TK_FLAGS = 4, TK_UCODE_BOOT = 8, TK_UCODE_BOOT_SIZE = 12, TK_UCODE = 16, TK_UCODE_SIZE = 20,
    TK_UCODE_DATA = 24, TK_UCODE_DATA_SIZE = 28, TK_DRAM_STACK = 32, TK_DRAM_STACK_SIZE = 36, TK_OUTPUT_BUFF = 40,
    TK_OUTPUT_BUFF_SIZE = 44, TK_DATA_PTR = 48, TK_DATA_SIZE = 52, TK_YIELD_DATA_PTR = 56, TK_YIELD_DATA_SIZE = 60,
};
enum { OS_STATE_STOPPED = 1, OS_STATE_RUNNABLE = 2, OS_STATE_RUNNING = 4, OS_STATE_WAITING = 8 };
enum { OS_MESG_NOBLOCK = 0, OS_MESG_BLOCK = 1 };
enum {
    OS_EVENT_SW1, OS_EVENT_SW2, OS_EVENT_CART, OS_EVENT_COUNTER, OS_EVENT_SP, OS_EVENT_SI, OS_EVENT_AI, OS_EVENT_VI,
    OS_EVENT_PI, OS_EVENT_DP, OS_EVENT_CPU_BREAK, OS_EVENT_SP_BREAK, OS_EVENT_FAULT, OS_EVENT_THREADSTATUS,
    OS_EVENT_PRENMI, OS_NUM_EVENTS
};
enum { M_GFXTASK = 1, M_AUDTASK = 2 };

/* ---- thread.c: the lock, threads, queues, events ---- */
/* The one recursive lock that stands for libultra's interrupt mask: every
 * queue, timer and event change happens under it. */
void xu_lock(void);
void xu_unlock(void);
/* under the lock, from any thread (VI, timers, tasks): a message, never blocking */
int xu_send(uint32_t mq, uint32_t msg, int block, int jam);
void xu_post_event(int event);
/* the same at the front of its queue (osJamMesg order): a synchronous
 * task's done events, ahead of the retraces that queued while it ran */
void xu_post_event_first(int event);
/* the N64 thread priority onto the kernel's levels 2..14 */
int xu_level_of(int pri);
void xu_stall_report(const char* why);   /* test builds: what each N64 thread waits on */
/* threads the runtime starts for itself (VI, timers, the save writer) */
xhw_thread* xu_spawn(void (*fn)(void*), void* arg, int level, uint32_t stack);

/* time.c */
uint64_t xu_count64(void);   /* the CPU counter: 46.875 MHz from the ACPI clock */

/* vi.c */
unsigned xu_retrace(void);
uint32_t xu_vi_mode(void);              /* the OSViMode the game set (N64 address), or 0 */
uint32_t xu_vi_framebuffer(void);       /* the one shown now */

/* sp.c: a graphics task for the interpreter (weak until it exists) */
void xrc_gfx_task(uint32_t task);
void xrc_audio_task(uint32_t task);
unsigned xu_gfx_tasks(void);

/* pi.c: the ROM the PI reads (z64 order), through xhw_rom_read (resident or
 * the page cache, PLAN.md 3.5); 0 = done, 1 = outside the ROM (nothing read) */
uint32_t xu_rom_to_rdram(uint32_t rom_off, uint32_t dram, uint32_t n);
uint32_t xu_rom_read(uint32_t rom_off, void* dst, uint32_t n);
const uint8_t* xu_rom_ptr(uint32_t rom_off, uint32_t n);   /* resident only: NULL with the page cache */

/* save.c: the save images and their files; the writer's flush (the exit
 * combos call it); the one lock every save access holds (recursive) */
void xu_save_flush(void);
/* the BACK menu's Paks page (save.c): port `port` holds XU_PAK_* `kind`
 * and pak `letter` (0..3, A..D; the port that held it takes this one's);
 * hot: as a swap the game sees (savedev.c xu_pak_pull) */
void xu_pak_insert(int port, int kind, int letter, int hot);
int xu_pak_letter(int port);
void xu_pak_apply_settings(int hot);   /* g_xrc_settings' pak and pak_file */
void xu_save_lock(void);
void xu_save_unlock(void);
uint8_t* xu_eep_image(uint32_t* size);   /* under the lock */
void xu_eep_dirty(void);

#endif
