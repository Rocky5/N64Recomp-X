/* Conker's Bad Fur Day (NFUE, US): its main loop's step (0x1501878C), run with one check before it, written here (MIT).
 * n64rx builds it in place of the recompiled routine (N64RX_PATCHES=0: not used).
 *
 * The main thread takes messages from its queue: "a job started" (build the next frame) and "a job finished", which takes
 * one off the count of jobs in flight at 0x800BEAA8. To change scene the game's logic sets 0x800BE615 and posts no more
 * jobs; the step leaves the loop on the "finished" message that brings the count to 0. Host speed changes the order at the
 * first frame: the first "started" can skip its frame, the count then runs one low for the rest of the scene, the last
 * "finished" takes it to -1 and not 0, and every thread waits (about one boot in three in xemu, at frame 200 or so).
 *
 * DIVERGES: with the scene change asked for and the count at or under 0, the count is taken as the one job left, and if
 * nothing is left to finish, the "finished" message is sent here. The N64's count never gets there.
 */
#include "recomp.h"

void xrc_interp(uint8_t* rdram, recomp_context* ctx, uint32_t entry);
void osSendMesg_recomp(uint8_t* rdram, recomp_context* ctx);

#define MAIN_QUEUE 0x800BEA10u
#define JOB_QUEUE 0x8003B1E8u
#define JOB_DONE_MSG 0x800BEA68u   /* a message of type 2 (it has two, one per job slot) */
#define SCENE_CHANGE 0x800BE615u
#define JOBS 0x800BEAA8u
#define SCHED_STATE 0x8003A582u    /* 0: no job running */

XRC_PATCH(0x1501878C) {
    gpr jobs = (gpr)(int32_t)JOBS;
    if (MEM_BU(0, (gpr)(int32_t)SCENE_CHANGE) && MEM_H(0, jobs) <= 0) {
        MEM_H(0, jobs) = 1;
        if (!MEM_BU(0, (gpr)(int32_t)SCHED_STATE) && !MEM_W(8, (gpr)(int32_t)JOB_QUEUE) && !MEM_W(8, (gpr)(int32_t)MAIN_QUEUE)) {
            ctx->r4 = (gpr)(int32_t)MAIN_QUEUE;
            ctx->r5 = (gpr)(int32_t)JOB_DONE_MSG;
            ctx->r6 = 0;
            osSendMesg_recomp(rdram, ctx);
        }
    }
    xrc_interp(rdram, ctx, 0x1501878Cu);
}
