/* Donkey Kong 64 (NDOE, US 1.0): its audio thread's loop (0x80601D24; the
 * decomp's code_6710.c names it func_global_asm_80601D24), written here
 * (MIT). n64rx builds it in place of the recompiled loop (games/patches/
 * README.md; N64RX_PATCHES=0: not used).
 *
 * The loop takes a start message (5) from its queue, starts the audio task
 * the scheduler holds (0x29E) if one waits, makes the next task's list and
 * waits for the last task's done. The scheduler sets the start timer at
 * every second retrace while a task waits. Here the loop takes longer than
 * two retraces (the scheduler runs display lists inside its own task start,
 * xemu), the timer fires again for a task already started, and that stale
 * message found no task waiting: the loop started nothing, then waited for
 * a done that never came. The game was silent from its first seconds.
 *
 * DIVERGES: a start message that finds no task waiting, after the first, is
 * taken and dropped. Only the loop starts the game's audio tasks, so each
 * wait for a done follows a start and nothing is lost when the loop keeps up.
 */
#include "recomp.h"

void osSendMesg_recomp(uint8_t* rdram, recomp_context* ctx);
void osRecvMesg_recomp(uint8_t* rdram, recomp_context* ctx);
void xhw_logf(const char* fmt, ...);

#define SCHED     0x80767A40u   /* the scheduler: its queue first, its waiting audio tasks at +0x264 */
#define CLIENT    0x8076D4C0u
#define STARTQ    0x8076D698u   /* start messages (5) and quit (10) */
#define DONEQ     0x8076D6D0u
#define BUFS      0x8076D4D8u   /* three audio buffers */
#define FRAME     0x807452C0u
#define ALGLOBALS 0x8076D708u

static void call(uint8_t* rdram, recomp_context* ctx, uint32_t vram) {
    LOOKUP_FUNC(vram)(rdram, ctx);
}

static void send(uint8_t* rdram, recomp_context* ctx, uint32_t mq, uint32_t msg, uint32_t block) {
    ctx->r4 = (gpr)(int32_t)mq, ctx->r5 = (gpr)(int32_t)msg, ctx->r6 = block;
    osSendMesg_recomp(rdram, ctx);
}

static uint32_t recv(uint8_t* rdram, recomp_context* ctx, uint32_t mq, gpr at) {
    ctx->r4 = (gpr)(int32_t)mq, ctx->r5 = at, ctx->r6 = 1;
    osRecvMesg_recomp(rdram, ctx);
    return (uint32_t)MEM_W(0, at);
}

XRC_PATCH(0x80601D24) {
    gpr sp = ctx->r29, at;
    uint32_t cur = 0, first = 1, n, stale = 0;
    ctx->r29 = ADD32(sp, -0x60);
    at = ADD32(ctx->r29, 0x58);
    MEM_W(0x10, ctx->r29) = 2;
    ctx->r4 = (gpr)(int32_t)SCHED, ctx->r5 = (gpr)(int32_t)CLIENT, ctx->r6 = (gpr)(int32_t)STARTQ, ctx->r7 = 2;
    call(rdram, ctx, 0x8060ED6Cu);   /* the scheduler's audio client: start messages every second retrace */
    send(rdram, ctx, STARTQ, 5, 0);
    for (;;) {
        uint32_t m = recv(rdram, ctx, STARTQ, at);
        if (m == 10) break;
        if (m != 5) continue;
        if (MEM_W(0x264, (gpr)(int32_t)SCHED)) {
            send(rdram, ctx, SCHED, 0x29E, 1);   /* start the waiting task */
        } else if (!first) {
            if (stale++ < 4) xhw_logf("[PATCH] DK64 audio: a start message with no task waiting, dropped");
            continue;
        }
        n = (uint32_t)MEM_W(0, (gpr)(int32_t)FRAME);
        ctx->r4 = (gpr)MEM_W(4u * (n % 3u), (gpr)(int32_t)BUFS), ctx->r5 = (gpr)(int32_t)cur;
        call(rdram, ctx, 0x80601EE4u);   /* the next task's list */
        n = (uint32_t)MEM_W(0, (gpr)(int32_t)FRAME);
        cur = (uint32_t)MEM_W(4u * ((n + 1u) % 3u), (gpr)(int32_t)BUFS);
        if (!first) {
            recv(rdram, ctx, DONEQ, at);   /* the last task's done */
            ctx->r4 = (gpr)(int32_t)cur;
            call(rdram, ctx, 0x80602104u);
        }
        first = 0;
        call(rdram, ctx, 0x8060A500u);
    }
    ctx->r4 = (gpr)(int32_t)ALGLOBALS;
    call(rdram, ctx, 0x8073938Cu);   /* n_alClose */
    ctx->r29 = sp;
}
