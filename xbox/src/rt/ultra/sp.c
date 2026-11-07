/* sp.c - RSP tasks, run synchronously on the calling thread (PLAN.md 3.3).
 *
 * As in every sibling: osSpTaskStartGo runs the task at once (a graphics
 * task through the interpreter, an audio task through asp_hle, M2), then
 * posts SP done (and DP done for graphics), as the RSP and RDP interrupts
 * would. One CPU means a render thread buys nothing. osSpTaskYield is
 * ignored and osSpTaskYielded says no (upstream does the same).
 *
 * DIVERGES: the done events go to the front of their queue. On the N64 the
 * RSP ran beside the CPU, so the retraces that came during a task were
 * handled while it ran and its done interrupt came after them. Here the
 * task runs on the scheduler's own thread: those retraces wait in the queue
 * ahead of the done events, and a scheduler that sees them with its task
 * still "running" asks it to yield and then recovers from a task "that did
 * not yield" (Pilotwings 64's _uvScHandleRetrace, xemu: the first frame took
 * longer than a retrace). Done first, then the late retraces, is what the
 * scheduler would have seen. */
#include <stdio.h>

#include "ultra.h"

static unsigned s_gfx, s_aud;

unsigned xu_gfx_tasks(void) { return s_gfx; }
unsigned xu_aud_tasks(void) { return s_aud; }
void xu_count_task(int gfx) { gfx ? s_gfx++ : s_aud++; }   /* mmio.c: a task started through SP_STATUS */

/* until the interpreter and the audio HLE are linked: count and log */
__attribute__((weak)) void xrc_gfx_task(uint32_t task) {
    if (s_gfx <= 3)
        xhw_logf("[SP] gfx task %u: ucode %08x (%u bytes), data %08x (%u bytes)", s_gfx, (unsigned)rd32(task + TK_UCODE),
                 (unsigned)rd32(task + TK_UCODE_SIZE), (unsigned)rd32(task + TK_DATA_PTR),
                 (unsigned)rd32(task + TK_DATA_SIZE));
}

/* gfx.c: the task's list ended with a FullSync (weak: always, as before) */
__attribute__((weak)) int xrc_gfx_task_dp(void) { return 1; }

__attribute__((weak)) void xrc_audio_task(uint32_t task) {
    if (s_aud <= 1)
        xhw_logf("[SP] audio task %u: ucode %08x, data %08x (%u bytes)", s_aud, (unsigned)rd32(task + TK_UCODE),
                 (unsigned)rd32(task + TK_DATA_PTR), (unsigned)rd32(task + TK_DATA_SIZE));
}

ULTRA(osSpTaskLoad) {
    (void)rdram;
    xrc_mmio_sp_task(ARG(0));   /* mmio.c: a game's own osSpTaskStartGo finds it there */
}

/* A task runs to its end inside osSpTaskStartGo; its done events wait until
 * the thread that started it next blocks (or the next retrace): on the N64
 * the RCP takes milliseconds, and code that starts a task outside the
 * scheduler's thread records it after the call (Blast Corps' scheduler
 * then took a DP done with no task recorded and asserted). The events go
 * out together and in order: SP done ahead of DP done.
 * Kill switch: -DXRC_RCP_DEFER=0 (posted inside the call, as before).
 *
 * The events wait for their own thread: another thread that blocks does not
 * post them, and a retrace posts them once that thread is blocked (or after
 * two retraces, for a thread that polls). Mario Party's nnsched has two
 * threads on one SP queue: the audio thread yields the graphics task and
 * takes its done, the graphics thread waits for the audio thread to pass it
 * on, and the graphics thread blocking must not post (and take) the audio
 * task's done meanwhile. Kill switch: -DXRC_RCP_OWNER=0 (any thread that
 * blocks, and every retrace, posts them all). */
#ifndef XRC_DP_ON_FULLSYNC
#define XRC_DP_ON_FULLSYNC 1
#endif
#ifndef XRC_RCP_DEFER
#define XRC_RCP_DEFER 1
#endif
#ifndef XRC_RCP_OWNER
#define XRC_RCP_OWNER 1
#endif
/* The events wait for room, too: a queue that is full (the scheduler's
 * thread late by a few retraces, its queue filled with retrace messages)
 * keeps them held, and the next flush tries again, in order. Posting into
 * a full queue dropped the SP done, and Ocarina of Time's scheduler waited
 * for it forever (its intermittent title stall). On the N64 the scheduler
 * drains its queue long before eight messages build up. Kill switch:
 * -DXRC_RCP_ROOM=0 (posted regardless, dropped when full). */
#ifndef XRC_RCP_ROOM
#define XRC_RCP_ROOM 1
#endif
/* A graphics task's done events held at least this many retraces after it
 * ran, as the RDP takes its time: the task runs here at its start, and its
 * DP done came out as soon as the scheduler's thread blocked, before the
 * game's next frame began. Pokemon Snap's photo check queues the analysis
 * of the frame the RDP is drawing as the next frame starts and runs it at
 * that task's DP done (sched.c's post-process); the done came first, so it
 * read a context the game was refilling (red: a Pokemon in focus on a few
 * frames in thirty, the Beach tutorial's "Press Z to aim" never held).
 * DIVERGES: a graphics task's done waits up to n retraces. Default 0: at
 * once, as before (an A/B switch until red says) */
#ifndef XRC_DP_LATENCY
#define XRC_DP_LATENCY 0
#endif
/* A graphics task takes the RCP milliseconds; here it is over when its start returns. The scheduler got its done as
 * soon as it blocked, before the thread it had preempted ran again. Hydro Thunder's main thread sends the task and
 * then sets a flag "the RCP is busy", which on the N64 the scheduler clears at the done, milliseconds later: here the
 * flag was set after the done, and stayed. The scheduler took the next audio task for one to yield to, and waited for a
 * done that never came (21 frames, then nothing). Mortal Kombat 4's main swaps its frame index after the send, and its
 * done handler clears the flag of the index before the current one. The list runs on the CPU, above main: a retrace
 * in those few instructions ran the whole list before them, the handler cleared the wrong flag, and main spun for ever on
 * the right one (one run in four). On the N64 main goes on while the RCP works.
 * DIVERGES: a graphics task's done waits n us from the task's end (500: still stuck, later; 2000: its attract race, xemu),
 * and n us from when the task's sender comes out of its osSendMesg, if it was preempted inside. Longer while a thread above
 * the sender is runnable: the sender has not had the CPU. The sender is the lowest thread to send since the last graphics
 * task. Two retraces at most in all. Not over when the sender blocks: that stopped Hydro Thunder at 115 frames.
 * -DXRC_RCP_SENDER_US=0: as soon as the thread that started it blocks */
#ifndef XRC_RCP_SENDER_US
#define XRC_RCP_SENDER_US 2000
#endif
int xu_thr_in_send(void);   /* thread.c */
int xu_thr_above_runs(void* thr);
void* xu_thr_low_sender(void);
static void* s_sender;     /* the thread that sent the last graphics task, as far as we can tell */
#define MAX_PEND 16
static struct { void* thr; int dp; unsigned at; uint64_t due; } s_pend[MAX_PEND];   /* oldest first */
static int s_npend;

void* xu_self_thr(void);                 /* thread.c */
int xu_thr_blocked(void* thr);

#ifdef XRC_SP_RING   /* diagnosis: the last task starts and done events, for the stall report */
static struct { char what; uint32_t a, b; unsigned at; } s_ring[512];
static unsigned s_ring_n;
#define RING(w, x, y) do { unsigned i_ = s_ring_n++ % 512u; s_ring[i_].what = (w), s_ring[i_].a = (x), s_ring[i_].b = (y), s_ring[i_].at = xu_retrace(); } while (0)
void xu_sp_ring_note(int w, uint32_t a, uint32_t b);
void xu_sp_ring_note(int w, uint32_t a, uint32_t b) { RING((char)w, a, b); }
void xu_sp_ring_dump(void);
void xu_sp_ring_dump(void) {
    unsigned i, n = s_ring_n > 512u ? 512u : s_ring_n;
    for (i = 0; i < n; i++) {
        unsigned k = (s_ring_n - n + i) % 512u;
        xhw_logf("[SP] %c %08x %08x retrace %u", s_ring[k].what, (unsigned)s_ring[k].a, (unsigned)s_ring[k].b, s_ring[k].at);
    }
}
#else
#define RING(w, x, y) do {} while (0)
#endif

static void post_done(int dp) {
    RING('P', dp, 0);
    if (dp) xu_post_event_first(OS_EVENT_DP);   /* jammed in reverse: SP done is read first */
    xu_post_event_first(OS_EVENT_SP);
}

/* room for the SP done (and the DP done) in their queues; one queue for both needs two */
static int room(int dp) {
    int sp = xu_event_room(OS_EVENT_SP);
    if (!dp) return sp >= 1;
    return xu_event_room(OS_EVENT_DP) >= 1 && sp >= (xu_event_same(OS_EVENT_SP, OS_EVENT_DP) ? 2 : 1);
}

static void take(int i) {
    post_done(s_pend[i].dp);
    memmove(&s_pend[i], &s_pend[i + 1], (size_t)(s_npend - i - 1) * sizeof s_pend[0]);
    s_npend--;
}

/* under the lock: thread.c before `mine` sleeps (its own tasks' events),
 * vi.c every retrace with NULL (those whose thread is blocked, or two
 * retraces old). How many went out. */
/* Two tasks' events out in one flush: each take() jams its pair at the
 * front, so the second task's came out first and the scheduler paired the
 * audio done with the graphics task (docs/review-2026-10.md 5b.3). Those
 * that go are chosen oldest first (as far as there is room) and posted
 * newest first, so the oldest is read first. -DXRC_RCP_ORDER=0: as before */
#ifndef XRC_RCP_ORDER
#define XRC_RCP_ORDER 1
#endif
/* this one's done waits for a sender, or for the time after one */
static int held_for_sender(int i, uint64_t now, int* in_send) {
    int young = xu_retrace() - s_pend[i].at < 2;
    if (!s_pend[i].due) return 0;
    if (s_pend[i].due > now) return young;
    if (*in_send < 0) *in_send = xu_thr_in_send();
    return *in_send && young;
}

static int dp_past_full_sp(int waited);
int xu_event_waited(int e);   /* thread.c */

int xu_rcp_flush(void* mine) {
    int i = 0, n = 0, in_send = -1;
    uint64_t now = XRC_RCP_SENDER_US ? xu_count64() : 0;
    /* the retrace's: a thread already asleep on the DP done's queue, the task's own thread still running */
    if (!mine) dp_past_full_sp(xu_event_waited(OS_EVENT_DP));
    if (XRC_RCP_ORDER) {
        int sel[MAX_PEND], k = 0, j, sp = xu_event_room(OS_EVENT_SP), dp = xu_event_room(OS_EVENT_DP),
            same = xu_event_same(OS_EVENT_SP, OS_EVENT_DP);
        for (i = 0; i < s_npend; i++) {
            int go = !XRC_RCP_OWNER ||
                     (mine ? s_pend[i].thr == mine
                           : xu_thr_blocked(s_pend[i].thr) || xu_retrace() - s_pend[i].at >= 2);
            if (XRC_DP_LATENCY && s_pend[i].dp && xu_retrace() - s_pend[i].at < (unsigned)XRC_DP_LATENCY) go = 0;
            if (!go) continue;
            if (held_for_sender(i, now, &in_send)) break;   /* those after it ran after it */
            if (XRC_RCP_ROOM) {
                int nsp = 1 + (s_pend[i].dp && same), ndp = s_pend[i].dp && !same;
                if (sp < nsp || dp < ndp) break;   /* no room: this one and those after it wait */
                sp -= nsp, dp -= ndp;
            }
            sel[k++] = i;
        }
        for (j = k - 1; j >= 0; j--) post_done(s_pend[sel[j]].dp);
        for (i = j = 0, n = 0; i < s_npend; i++)   /* the rest, in order */
            if (n < k && sel[n] == i) n++;
            else s_pend[j++] = s_pend[i];
        s_npend = j;
        return k;
    }
    while (i < s_npend) {
        int go = !XRC_RCP_OWNER ||
                 (mine ? s_pend[i].thr == mine
                       : xu_thr_blocked(s_pend[i].thr) || xu_retrace() - s_pend[i].at >= 2);
        if (XRC_DP_LATENCY && s_pend[i].dp && xu_retrace() - s_pend[i].at < (unsigned)XRC_DP_LATENCY) go = 0;
        if (go && held_for_sender(i, now, &in_send)) break;
        if (go && XRC_RCP_ROOM && !room(s_pend[i].dp))
            break;   /* no room: this one and those after it wait for the next flush */
        if (go) take(i), n++;
        else i++;
    }
    return n;
}

/* A scheduler that is behind (its queue never empty, so it never blocks)
 * got its own task's done only from the retrace two retraces later: Donkey
 * Kong 64's audio tasks took three retraces each, its audio thread built
 * the next before the last was started, the scheduler's start timer (one
 * message per task on the N64) fired again for each retrace it waited, and
 * a stale message later found nothing to start: the audio thread waited for
 * a done that never came (silent from the first second, xemu). On the N64
 * the RCP's done arrives milliseconds after the start, behind what the
 * queue holds. -DXRC_RCP_RECV=1: the starting thread's own held events go
 * out (behind the queue's messages, as the interrupt sends them) when it
 * next receives with OS_MESG_BLOCK, blocking or not. Default 0: as before. */
#ifndef XRC_RCP_RECV
#define XRC_RCP_RECV 0
#endif
/* thread.c, under the lock: the last sender is out of its osSendMesg */
void xu_rcp_senders_out(void* who) {
    int i, any = 0;
    uint64_t due = xu_count64() + (uint64_t)XRC_RCP_SENDER_US * 46875u / 1000u;
    if (who != s_sender) return;
    for (i = 0; i < s_npend; i++)
        if (s_pend[i].due) s_pend[i].due = due, any = 1;
    if (any) xu_rcp_kick();
}

/* time.c's thread, under the lock: those whose time has come, and when the next one's does (0: none) */
uint64_t xu_rcp_tick(uint64_t now) {
    uint64_t next = 0;
    int i;
    if (!XRC_RCP_SENDER_US) return 0;
    if (s_sender && xu_thr_above_runs(s_sender)) {   /* the sender has not had the CPU */
        uint64_t later = now + 46875u;   /* (1 ms) */
        for (i = 0; i < s_npend; i++)
            if (s_pend[i].due && s_pend[i].due <= now && xu_retrace() - s_pend[i].at < 2) s_pend[i].due = later;
    }
    for (i = 0; i < s_npend; i++)
        if (s_pend[i].due && s_pend[i].due <= now) {
            xu_rcp_flush(NULL);
            break;
        }
    for (i = 0; i < s_npend; i++)
        if (s_pend[i].due > now && (!next || s_pend[i].due < next)) next = s_pend[i].due;
    return next;
}

int xu_rcp_flush_recv(void* mine) {
    int i = 0, n = 0;
    if (!XRC_RCP_RECV) return 0;
    while (i < s_npend) {
        if (s_pend[i].thr != mine) {
            i++;
            continue;
        }
        if (XRC_RCP_ROOM && !room(s_pend[i].dp)) break;
        RING('Q', s_pend[i].dp, 0);
        xu_post_event(OS_EVENT_SP);   /* in the order the RCP raises them: SP done, then DP done */
        if (s_pend[i].dp) xu_post_event(OS_EVENT_DP);
        memmove(&s_pend[i], &s_pend[i + 1], (size_t)(s_npend - i - 1) * sizeof s_pend[0]);
        s_npend--;
        n++;
    }
    return n;
}

/* The room rule above waits for a reader. A thread about to block for its DP done, with its SP done's queue full,
 * is that reader and is reading another queue: nothing would ever move. Mischief Makers starts more tasks than it
 * reads SP dones of (its queue holds one; two audio tasks a frame) and on the N64 the rest are dropped, as a full
 * queue does. Its tasks up to the graphics one lose their SP dones here, the same way, and the DP done goes out.
 *
 * The same thread, with no DP done held for it: a graphics task ran that showed no FullSync (s_dp_missed). A queue
 * that only the DP done goes to is waited on for that alone, and a task's FullSync is ours to miss when we do not
 * read its microcode (Dark Rift's second one, its lists read as something else). The done goes out. A game whose
 * lists really have none never waits so; Blast Corps' scheduler takes all its events on one queue and asserts on
 * a DP done with no RDP task, and it is not this case.
 * -DXRC_RCP_DP_WAIT=0: they wait (the games stall at their second or third frame) */
#ifndef XRC_RCP_DP_WAIT
#define XRC_RCP_DP_WAIT 1
#endif
static int s_dp_missed;   /* a graphics task since the last DP done showed no FullSync */
int xu_rcp_dp_wait(void* mine, uint32_t key) {
    int i;
    /* The other way round: a thread about to block on the SP done's queue, with the DP done's queue full. S.C.A.R.S. does
     * not read its first DP done; on the N64 the later ones are dropped until it does, and here its next SP dones waited
     * for room behind them. Its own oldest task's DP done is dropped, as the full queue does. */
    if (XRC_RCP_DP_WAIT && xu_event_is(OS_EVENT_SP, key) && !xu_event_same(OS_EVENT_SP, OS_EVENT_DP) && xu_event_room(OS_EVENT_DP) <= 0 &&
        xu_event_room(OS_EVENT_SP) > 0) {
        for (i = 0; i < s_npend; i++)
            if (s_pend[i].thr == mine && s_pend[i].dp) {
                s_pend[i].dp = 0;
                return xu_rcp_flush(mine);
            }
    }
    if (!XRC_RCP_DP_WAIT || !xu_event_is(OS_EVENT_DP, key) || xu_event_same(OS_EVENT_SP, OS_EVENT_DP) ||
        xu_event_same(OS_EVENT_VI, OS_EVENT_DP) || xu_event_room(OS_EVENT_DP) <= 0)
        return 0;
    if (s_dp_missed) {
        for (i = 0; i < s_npend; i++)
            if (s_pend[i].dp) return 0;   /* one is on its way */
        s_dp_missed = 0;
        xu_post_event(OS_EVENT_DP);
        return 1;
    }
    return dp_past_full_sp(1);
}

/* The SP done's queue full, a reader for the DP done's. Any thread's task: Madden's main thread starts them and spins on a
 * word, its graphics thread waits for the DP done, and nothing ever reads the SP done's one slot */
static int dp_past_full_sp(int waited) {
    int i, j, last = -1;
    if (!XRC_RCP_DP_WAIT || !XRC_RCP_ROOM || !s_npend || xu_event_same(OS_EVENT_SP, OS_EVENT_DP) || xu_event_same(OS_EVENT_VI, OS_EVENT_DP) ||
        xu_event_room(OS_EVENT_SP) > 0 || xu_event_room(OS_EVENT_DP) <= 0 || !xu_event_set(OS_EVENT_SP) || !waited)
        return 0;
    for (i = 0; i < s_npend && last < 0; i++)
        if (s_pend[i].dp) last = i;
    if (last < 0) return 0;
    for (i = j = 0; i < s_npend; i++)
        if (i > last) s_pend[j++] = s_pend[i];
    s_npend = j;
    xu_post_event(OS_EVENT_DP);
    return 1;
}

int xrc_jpeg_task(uint32_t task);   /* ../jpeg.c */
static void* volatile s_task_thr;   /* the thread inside xu_sp_run */
void* xu_task_thr(void);
void* xu_task_thr(void) { return s_task_thr; }   /* thread.c: task_gate */
void xu_sp_held(void* me);

#ifndef XRC_SP_BY_TYPE
#define XRC_SP_BY_TYPE 1
#endif

/* A graphics task with no display list is another kind of task: Resident
 * Evil 2's own microcode (0x80031460, its parameters in ucode_data, data_ptr
 * 0; six a frame from its scheduler's slot 0xB) was read as a list from RDRAM
 * 0. DIVERGES: its microcode does not run (what it draws is missing); SP done
 * goes out as for any task, no DP done (its scheduler waits for none).
 * -DXRC_GFX_NO_LIST=0: run as a graphics task, as before */
#ifndef XRC_GFX_NO_LIST
#define XRC_GFX_NO_LIST 1
#endif
int xu_task_has_list(uint32_t task) {
    return !XRC_GFX_NO_LIST || (rd32(task + TK_DATA_PTR) & 0x1FFFFFFFu) != 0;
}

/* the task run, its done events held for the starting thread: osSpTaskStartGo,
 * and mmio.c for a game's own osSpTaskStartGo through SP_STATUS */
static void sp_run_now(uint32_t task) {
    uint32_t type = rd32(task + TK_TYPE);
    int gfx = type == M_GFXTASK && xu_task_has_list(task);
    int takes_time = gfx;
    void* me = xu_self_thr();
    {   /* diagnosis: a task started beside another thread's, which the RSP could not do */
        static unsigned s_beside;
        if (s_task_thr && s_task_thr != me && s_beside < 8) {
            s_beside++;
            xhw_logf("[SP] task %08x (type %u) started by N64 thread %08x while thread %08x's runs (retrace %u)", (unsigned)task,
                     (unsigned)type, (unsigned)xu_thr_t(me), (unsigned)xu_thr_t(s_task_thr), xu_retrace());
        }
    }
    s_task_thr = me;
#ifdef XRC_LOG_SP_STARTS
    {   /* diagnosis: the first SP starts, their type and thread */
        static unsigned s_n;
        if (s_n++ < (unsigned)(XRC_LOG_SP_STARTS))
            xhw_logf("[SP] start %u: type %u task %08x by N64 thread %08x (retrace %u)", s_n, (unsigned)type,
                     (unsigned)task, (unsigned)xu_thr_t(xu_self_thr()), xu_retrace());
    }
#endif
    if (gfx) {
        s_gfx++;
        xrc_gfx_task(task);
        /* DIVERGES from the runtime's old way: DP done only when the list ended
         * with a FullSync, as the RDP raises it (Blast Corps' scheduler asserts
         * on a DP done with no RDP task). -DXRC_DP_ON_FULLSYNC=0: always. */
        if (XRC_DP_ON_FULLSYNC && !xrc_gfx_task_dp()) gfx = 0;
        s_dp_missed = !gfx;
    } else if (type == M_AUDTASK || !XRC_SP_BY_TYPE) {
        s_aud++;
        xrc_audio_task(task);
    } else if (type == M_NJPEGTASK && xrc_jpeg_task(task)) {
        /* ../jpeg.c: Ocarina of Time's JPEG decoder task, run */
    } else {
        /* DIVERGES: another kind of task (a JPEG or HVQ decoder) is not run;
         * its done events go out as for any task. It used to go to the audio
         * HLE, whose first task names the audio microcode: Ocarina of Time's
         * JPEG task came first and every audio task after it was skipped as
         * an "unknown audio microcode". -DXRC_SP_BY_TYPE=0: as before. */
        static uint32_t s_seen[4];
        int k;
        for (k = 0; k < 4 && s_seen[k] && s_seen[k] != type + 1; k++) {}
        if (k < 4 && !s_seen[k]) {
            s_seen[k] = type + 1;
            xhw_logf("[SP] task type %u (microcode %08x, %u bytes): not run", (unsigned)type,
                     (unsigned)rd32(task + TK_UCODE), (unsigned)rd32(task + TK_UCODE_SIZE));
        }
    }
    if (s_task_thr == me) s_task_thr = NULL;
    RING('S', task, (type << 8) | (gfx ? 1u : 0u));
    if (!XRC_RCP_DEFER) {
        post_done(gfx);
        return;
    }
    xu_lock();
    if (s_npend == MAX_PEND) take(0);   /* nobody blocked for 16 tasks: the oldest goes out */
    s_pend[s_npend].thr = xu_self_thr();
    s_pend[s_npend].dp = gfx;
    s_pend[s_npend].at = xu_retrace();
    s_pend[s_npend].due = 0;
    if (XRC_RCP_SENDER_US && takes_time) {
        s_pend[s_npend].due = xu_count64() + (uint64_t)XRC_RCP_SENDER_US * 46875u / 1000u;
        s_sender = xu_thr_low_sender();
        xu_rcp_kick();
    }
    s_npend++;
    xu_unlock();
}

/* A thread that raised its own priority: the task waits for its next runtime
 * call (thread.c, XRC_TASK_GATE says why) */
int xu_thr_raised(void* thr);
static uint32_t s_held_task;
static void* s_held_thr;
void xu_sp_held(void* me) {
    if (s_held_task && s_held_thr == me) {
        uint32_t task = s_held_task;
        s_held_task = 0;
        sp_run_now(task);
    }
}
void xu_sp_run(uint32_t task) {
    void* me = xu_self_thr();
    if (xu_thr_raised(me)) {
        xu_sp_held(me);
        s_held_task = task, s_held_thr = me;
        return;
    }
    sp_run_now(task);
}

ULTRA(osSpTaskStartGo) {
    (void)rdram;
    xrc_mmio_sp_task(0);   /* run here: a later raw SP start does not run it again */
    xu_sp_run(ARG(0));
}

ULTRA(osSpTaskYield) {
    (void)rdram;
    (void)ctx;
}

ULTRA(osSpTaskYielded) {
    (void)rdram;
    RET(0);
}

ULTRA(__osSpSetPc) {
    (void)rdram;
    RET(0);
}

/* the DP: always idle */
/* A list the game wrote itself, handed to the RDP with no RSP task. Without this Killer Instinct Gold's scheduler waited for ever
 * for the DP done of its first list, a clear of the screen. A list that does not read as RDP commands only gives its done: a game
 * whose microcode writes its list to RDRAM had the display list drawn at its task, and what lies here was not written by us.
 * -DXRC_DP_LIST=0: nothing, as before */
#ifndef XRC_DP_LIST
#define XRC_DP_LIST 1
#endif
void xrc_gfx_dp_list(uint32_t list, uint32_t bytes);   /* ../gfx.c */
ULTRA(osDpSetNextBuffer) {
    static unsigned s_n;
    uint32_t a = ARG(0), n = ARG(3), i;
    int sync = 0, plain = 1;
    (void)rdram;
    if (XRC_DP_LIST && n && !(n & 7u) && n <= 0x100000u) {
        for (i = 0; i < n && plain;) {
            uint32_t op = rd32(a + i) >> 24;
            if (op == 0xE9u) sync = 1;
            if (op >= 0xC8u && op <= 0xCFu) i += 32u + (op & 4u ? 64u : 0u) + (op & 2u ? 64u : 0u) + (op & 1u ? 16u : 0u);
            else if (op == 0xE4u || op == 0xE5u) i += 16u;
            else if (op >= 0xE6u) i += 8u;
            else plain = 0;
        }
        if (!plain) sync = 1;   /* (whatever it is, the RDP would have ended it) */
        if (s_n++ < 8)
            xhw_logf("[OS] osDpSetNextBuffer %08x, %u bytes, %s (retrace %u)", (unsigned)a, (unsigned)n, plain ? "drawn" : "not read",
                     xu_retrace());
        if (plain) {
            xrc_gfx_dp_list(xu_phys(a), n);
        }
        if (sync) {
            xu_lock();
            xu_post_event(OS_EVENT_DP);
            xu_unlock();
        }
    }
    RET(0);
}
ULTRA(osDpGetStatus) {
    (void)rdram;
    RET(0);
}
ULTRA(osDpSetStatus) {
    (void)rdram;
    (void)ctx;
}
