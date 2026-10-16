/* xhw.h - the Xbox hardware layer (xbox/src/hw, nxdk's i386-pc-win32 triple)
 * as seen by the game-side port code (xbox/src/game, xbox/src/gfx: the game
 * triple). From Melee-X's xhw.h, cut to what Wings64-X uses.
 *
 * Only scalars, pointers and structs without bit-fields or 64-bit members
 * cross this boundary, so both triples agree on every layout here. */
#ifndef XHW_H
#define XHW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Test builds (autopad's, or -DXHW_TEST_BUILD=1) keep the tools for console
 * rounds: BACK takes a screenshot and the frame-rate counter is on by
 * default. A plain build is a release. */
#ifndef XHW_TEST_BUILD
#if (defined(XHW_AUTOPAD) && XHW_AUTOPAD) || (defined(XHW_PROF) && XHW_PROF)
#define XHW_TEST_BUILD 1
#else
#define XHW_TEST_BUILD 0
#endif
#endif

/* ---- logging (COM1 when present, E:\UDATA\<title id>\boot.log) ---- */
void xhw_log(const char* line);
void xhw_logf(const char* fmt, ...) __attribute__((format(__printf__, 1, 2)));
/* Unrecoverable: draw the message on screen, log it, wait, go to the dashboard. */
void xhw_fatal(const char* title, const char* msg) __attribute__((noreturn));

/* ---- time (xhw_clock.c) ----
 * Every clock here is the ACPI power-management timer (3.375 MHz from its
 * own crystal) through KeQueryPerformanceCounter: never the CPU's TSC, whose
 * rate nxdk guesses from the multiplier and FSB (wrong on an upgraded or
 * overclocked CPU). The TSC is only measured against it at boot for the
 * [CLOCK] line. */
uint64_t xhw_ticks(void);            /* ACPI timer ticks */
uint64_t xhw_ticks_per_sec(void);    /* 3375000 */
uint64_t xhw_time_ns(void);          /* monotonic */
uint64_t xhw_time_us(void);
void xhw_clock_calibrate(void);      /* boot: [CLOCK] cpu N MHz */
uint32_t xhw_cpu_mhz(void);          /* measured at boot (log and diagnostics only) */
void xhw_sleep_ms(uint32_t ms);
void xhw_yield(void);

/* ---- threads and locks ---- */
typedef struct xhw_mutex xhw_mutex;
xhw_mutex* xhw_mutex_create(void);   /* recursive */
void xhw_mutex_lock(xhw_mutex* m);
void xhw_mutex_unlock(xhw_mutex* m);
typedef struct xhw_event xhw_event;
xhw_event* xhw_event_create(void);   /* auto-reset */
void xhw_event_signal(xhw_event* e);
int xhw_event_wait(xhw_event* e, uint32_t timeout_ms);   /* 1 signalled, 0 timeout */
#define XHW_INFINITE 0xFFFFFFFFu
uint32_t xhw_tls_alloc(void);
void* xhw_tls_get(uint32_t slot);
void xhw_tls_set(uint32_t slot, void* value);
/* priority: -2 (lowest) .. +2 (highest), +15 time critical; relative to normal */
int xhw_thread_start(void (*fn)(void*), void* arg, int priority, uint32_t stack_bytes);
void xhw_thread_set_priority(int priority);   /* the calling thread */
/* Threads at absolute kernel priority levels (base priority, 1..14; normal
 * is 8, the watchdog runs at 15): the libultra shim maps N64 priorities
 * onto these so the kernel's strict priority preemption orders them as the
 * N64 did. The token stays valid for the program's life. */
typedef struct xhw_thread xhw_thread;
xhw_thread* xhw_thread_spawn(void (*fn)(void*), void* arg, int level, uint32_t stack_bytes);
void xhw_thread_set_level(xhw_thread* t, int level);
xhw_thread* xhw_thread_self(void);    /* NULL for a thread not made by xhw_thread_spawn */
void xhw_thread_adopt_self(int level);   /* the calling thread becomes one (the boot thread) */

/* ---- memory ---- */
uint32_t xhw_mem_free_kb(void);
/* The game's RDRAM arena: zeroed contiguous memory, cached, page-aligned, at
 * 0x80000000 | physical (bit 31 set: the game's RAM-not-ROM test). NULL if
 * there is not that much contiguous memory. Never freed. */
void* xhw_rdram_alloc(uint32_t bytes);
/* 128 MB consoles run as 64 MB ones (xhw_ramlock.c): call first in main_body.
 * _log writes the [MEM] line once the log is open. */
void xhw_ram_lock64(void);
void xhw_ram_lock64_log(void);
/* Memory the kernel writes (file reads) must be committed: nxdk's malloc is
 * committed already, so this is a no-op kept for the Melee-X call sites. */
void xhw_commit(const void* p, uint32_t bytes);
uint32_t xhw_lazy_committed_kb(void);

/* ---- files and paths ---- */
const char* xhw_game_dir(void);      /* "D:\\" */
const char* xhw_save_dir(void);      /* "E:\\UDATA\\<title id>\\" */
int xhw_mkdir(const char* path);
int xhw_copy_file(const char* from, const char* to);
enum { XHW_REPLACE_FAILED = 0, XHW_REPLACE_OK = 1, XHW_REPLACE_LOST_TO = 2 };
int xhw_replace_file(const char* from, const char* to);
typedef struct xhw_dir_entry { char name[64]; int is_dir; uint32_t size; } xhw_dir_entry;
void* xhw_dir_first(const char* pattern, xhw_dir_entry* out);
int xhw_dir_next(void* handle, xhw_dir_entry* out);
void xhw_flush(void* stdio_file);
unsigned xhw_log_fallback(void);
/* Two lines at the top of the picture for 10 s, any thread (logged too). */
void xhw_notice(const char* line1, const char* line2);
/* Test builds: an integer switch from E:\UDATA\<title id>\debug.txt
 * ("name = value"), else `def` (xhw_debug.c). Release builds: always `def`. */
int xhw_debug_int(const char* name, int def);
void* xhw_file_open(const char* path);
int xhw_file_read(void* file, uint32_t off, void* dst, uint32_t len);
uint32_t xhw_file_size(void* file);
void xhw_file_close(void* file);
/* Whole-file write through a temp file, read back and renamed over `path`
 * (power-cut safe). 1 on success. */
int xhw_write_file_atomic(const char* path, const void* data, uint32_t len);

/* ---- controllers ---- */
typedef struct xhw_pad {
    int connected;
    uint32_t buttons;            /* XHW_BTN_* */
    int16_t lx, ly, rx, ry;      /* -32768..32767, y up */
    uint8_t lt, rt;              /* 0..255 */
} xhw_pad;
enum {
    XHW_BTN_A = 1u << 0, XHW_BTN_B = 1u << 1, XHW_BTN_X = 1u << 2, XHW_BTN_Y = 1u << 3,
    XHW_BTN_BLACK = 1u << 4, XHW_BTN_WHITE = 1u << 5, XHW_BTN_START = 1u << 6,
    XHW_BTN_BACK = 1u << 7, XHW_BTN_LSTICK = 1u << 8, XHW_BTN_RSTICK = 1u << 9,
    XHW_BTN_UP = 1u << 10, XHW_BTN_DOWN = 1u << 11, XHW_BTN_LEFT = 1u << 12,
    XHW_BTN_RIGHT = 1u << 13,
};
/* poll + get run under the pad lock (SDL is not thread-safe); the VI thread
 * and the watchdog only ever try it */
int xhw_pad_trylock(void);
void xhw_pad_lock(void);
void xhw_pad_unlock(void);
void xhw_pad_poll(void);
void xhw_pad_set_shots(int on);                /* BACK screenshots (always on in test builds) */
void xhw_pad_rumble(int port, int strength, uint32_t ms);   /* both motors, 0..65535; no-op without a pad */
int xhw_pad_get(int port, xhw_pad* out);       /* port 0..3; returns connected */

/* ---- the exit combos, in every game and every state (PLAN.md 3.9; xhw_combo.c) ----
 * L + R + BACK + BLACK returns to the dashboard, L + R + BACK + WHITE
 * restarts the XBE: any port, both triggers past XHW_COMBO_TRIG, read from
 * the raw pad before any N64 mapping. Three detection points: every
 * controller read the game makes (xhw_combo_check), the VI thread when the
 * game has not read for XHW_COMBO_VI_RETRACES (xhw_combo_vi_look), and the
 * watchdog's tick when nothing has looked for XHW_COMBO_WATCHDOG_MS
 * (xhw_combo_watchdog_look). The loading screen and the error cards look
 * through xhw_combo_look. -DXRC_COMBOS=0 (diagnosis builds only) turns them
 * off; no game pack can. */
#define XHW_COMBO_TRIG 200
#define XHW_COMBO_VI_RETRACES 30
#define XHW_COMBO_WATCHDOG_MS 2000
enum { XHW_COMBO_NONE = 0, XHW_COMBO_DASHBOARD = 1, XHW_COMBO_RESTART = 2 };
/* one pad state from a read the caller made (holding the pad lock): 1 when
 * it shows a combo's buttons, so the caller gives the game neutral input */
int xhw_combo_check(int port, const xhw_pad* p);
/* the game read the controllers (the VI watch counts from here) */
void xhw_combo_note_read(unsigned retrace);
/* poll the four ports under the pad lock and check them; then act */
void xhw_combo_look(void);
/* VI thread, every retrace: looks when the game has not read for a while;
 * try-lock only */
void xhw_combo_vi_look(unsigned retrace);
/* carry out a pending combo: waits for its buttons to be let go (at most
 * 3 s), 200 ms, flushes a dirty save (xhw_combo_set_flush, bounded to 1 s),
 * then the dashboard or the restart. Returns when nothing is pending; a
 * thread that finds another one already carrying it out parks for good.
 * Call without the pad lock. */
void xhw_combo_act(void);
int xhw_combo_pending(void);
void xhw_combo_set_flush(void (*flush)(void));

/* ---- video ---- */
typedef struct xhw_video_mode {
    int width, height;           /* framebuffer */
    int bpp;                     /* 32, or 16 at 720p/1080i */
    int widescreen;              /* 1: the dashboard's 16:9 flag (or 720p/1080i) */
    int progressive;
} xhw_video_mode;
const xhw_video_mode* xhw_video(void);
int xhw_video_480p_allowed(void);
int xhw_video_720p_allowed(void);
int xhw_video_1080i_allowed(void);
int xhw_video_widescreen_set(void);
/* Before xhw_video_boot (settings.ini): XHW_OUT_* and 0 to force 480i where
 * the dashboard allows 480p. */
enum { XHW_OUT_AUTO = 0, XHW_OUT_480I, XHW_OUT_480P, XHW_OUT_720P, XHW_OUT_1080I };
/* 1080i (maintainer, console run 15): it works, but the frame rate it costs
 * isn't worth it, so 720p is the top mode: Auto stops there, the settings
 * panel doesn't offer 1080i, and "output = 1080i" in settings.ini runs
 * 720p. -DXHW_VIDEO_1080I=1 brings it back. */
#ifndef XHW_VIDEO_1080I
#define XHW_VIDEO_1080I 0
#endif
#define XHW_OUT_COUNT (XHW_VIDEO_1080I ? 5 : 4)
void xhw_video_set_pref(int output, int progressive);
/* Sets the mode once, before pbkit starts. Ends the splash. */
void xhw_video_boot(void);
void xhw_video_fallback_480(void);
/* The confirm-or-revert net (PLAN.md 3.3, xhw_video.c). Before
 * xhw_video_boot: settings.ini's video_confirmed. After it: the mode's name
 * ("480i", "480p", "720p", "1080i"); 1 when this boot is 480i because the
 * last one never confirmed its mode (the caller saves 480i); 1 while the
 * prompt waits for A; xhw_video_confirm ends it (the caller saves
 * video_confirmed); 1 once it has waited its 15 s (the caller restarts the
 * XBE, and the next boot runs 480i). */
void xhw_video_set_confirmed(const char* mode);
const char* xhw_video_mode_name(void);
int xhw_video_reverted(void);
int xhw_video_probation(void);
void xhw_video_confirm(void);
int xhw_video_probation_expired(void);
/* Retrace: waits for the next vblank and counts it (xhw_frame_count). */
void xhw_wait_vblank(void);
unsigned xhw_frame_count(void);
void xhw_note_present(void);         /* the renderer: once per presented frame */
unsigned xhw_present_count(void);
/* The loading screen (xhw_splash.c, PLAN.md 5.5): drawn by the CPU in the
 * dashboard's 640x480 mode until xhw_video_boot changes it; every call here
 * does nothing once the mode is set. */
void xhw_splash_stage(const char* label);     /* the stage line; the last stage's time is logged */
void xhw_splash_progress(float f);            /* the bar, 0..1 */
/* the game card: title, one line under it, the pack's 64x64 ARGB icon or NULL */
void xhw_splash_card_set(const char* title, const char* sub, const uint32_t* icon64);
/* the video line ("Video: 720p 16:9 (the dashboard allows 720p).") and the
 * probation note, or NULL */
void xhw_splash_video(const char* line, const char* note);
/* the safe-video holds the loading screen latched */
enum { XHW_HOLD_NONE = 0, XHW_HOLD_SAFE = 1, XHW_HOLD_SAFE_RESET = 2 };
/* waits out the minimum hold (XHW_SPLASH_MS; any button but BACK and START
 * skips it) and a BACK hold in progress; returns XHW_HOLD_*. Without a
 * splash, waits 1.5 s for the controllers and reads the holds then. */
int xhw_splash_finish(void);
int xhw_splash_hold(void);
/* What xhw_video_boot will pick, without setting anything: the mode's name
 * ("480i", "480p", "720p"), the reason in a few words, whether it will be
 * on probation (not yet confirmed on this console). Needs set_pref and
 * set_confirmed first. */
typedef struct xhw_video_plan {
    char name[8];
    char why[48];
    int widescreen_dash;      /* the dashboard's 16:9 flag */
    int probation;
} xhw_video_plan;
void xhw_video_predict(xhw_video_plan* out);
/* Screenshot requests (BACK, autopad SHOT); the renderer takes them at present. */
void xhw_shot_request(int to_serial);
int xhw_shot_take(void);             /* 0 none, 1 BMP to E:, 2 [FBDUMP] to COM1 */
int xhw_running_in_xemu(void);

/* ---- system ---- */
/* ---- frame profile: a [PERF] line every 5 s (xhw_perf.c) ----
 * Coarse sections timed on the ACPI clock, added from any thread. DL and
 * PRESENT are whole sections; TEX is the part of DL spent converting and
 * uploading textures, WAIT the part of PRESENT (and of the next frame's
 * start) spent waiting for the GPU or the flip. */
enum { XHW_PERF_DL, XHW_PERF_TEX, XHW_PERF_PRESENT, XHW_PERF_WAIT, XHW_PERF_AUDIO, XHW_PERF_N };
void xhw_perf_add(int bucket, uint64_t us);
void xhw_perf_frame(void);   /* once per presented frame (nv2a.c) */

/* ---- audio: the AI output, s16 stereo (xhw_audio.c, Melee-X's driver) ---- */
int xhw_audio_init(uint32_t rate);            /* 1: AC97 or the xemu APU voice runs */
void xhw_audio_set_rate(uint32_t rate);       /* osAiSetFrequency */
uint32_t xhw_audio_space(void);               /* frames that fit the ring now */
void xhw_audio_write(const int16_t* stereo, uint32_t frames);   /* drops what does not fit */
void xhw_audio_stop(void);
uint32_t xhw_audio_skipped(void);             /* samples the latency cap skipped (stereo, both channels) */

void xhw_quit_to_dashboard(void) __attribute__((noreturn));
void xhw_reboot_self(void) __attribute__((noreturn));
void xhw_exit_reason(const char* why);
/* the same, and no later xhw_exit_reason renames it (the exit combos) */
void xhw_exit_reason_final(const char* why);
/* A full-screen message card (pbkit's debug screen): title + lines (NULL
 * ends), "Press any button or wait to return to the dashboard", then the
 * dashboard after `secs`. */
void xhw_error_card(const char* title, const char* const* lines, int secs) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
#endif
