/* boot.c - from the platform's boot to the game (PLAN.md 5.5, 6 M0).
 *
 * xhw_main.c has opened the log, started the watchdog and put the loading
 * screen up; this runs its stages:
 *
 *   settings      settings.ini (per game), then the video line: which mode
 *                 is coming, why, and the probation note when it is new here
 *   the game      no game built in: the card says so and how to build one.
 *                 The test pack (games/test): "Preparing memory" (the RDRAM
 *                 window), "Loading code" (the function table), "Starting"
 *                 (the recompiled entrypoint runs on the window), then a card
 *                 with what its checks found. A game: the ROM (default.xbe's
 *                 $ROM section, or a file beside it; "Reading ROM",
 *                 "Checking the ROM (SHA-1)", xhw_rom.c against the pack's
 *                 dumps), the window with the boot block and the boot
 *                 segment the IPL3 would have copied, the function table,
 *                 the video mode, then the recompiled entrypoint on this
 *                 thread (it becomes the boot code; the game's first
 *                 osStartThread parks it, ultra/thread.c).
 *   the holds     xhw_splash_finish waits out the minimum hold and returns
 *                 what BACK / BACK + START latched; the settings apply it
 */
#include <stdio.h>
#include <string.h>

#include "rt.h"
#include "settings.h"
#include "xhw.h"
#include "xrc_build.h"
#include "xrom.h"
#include "ultra/ultra.h"

#define TEST_RES 0x80100000u
#define TEST_MAGIC 0x4E525831u   /* "NRX1": tools/xbox/make_test_rom.py */

static const char* aspect(const xhw_video_plan* p) {
    int ws = g_xrc_settings.widescreen;
    if (ws == XRC_WS_STRETCH) return "stretched";
    if (!strcmp(p->name, "720p")) return ws == XRC_WS_OFF ? "4:3" : "16:9";
    if (ws == XRC_WS_ON) return "16:9";
    if (ws == XRC_WS_AUTO && p->widescreen_dash) return "16:9";
    return "4:3";
}

static void video_line(void) {
    xhw_video_plan p;
    char line[72], note[72];
    xhw_video_set_pref(g_xrc_settings.output, g_xrc_settings.progressive);
    xhw_video_set_confirmed(g_xrc_settings.video_confirmed);
    xhw_video_predict(&p);
    snprintf(line, sizeof line, "Video: %s %s (%s).", p.name, aspect(&p), p.why);
    note[0] = '\0';
    if (p.probation) snprintf(note, sizeof note, "First time at %s here: press A within 15 s to keep it.", p.name);
    xhw_splash_video(line, note);
    xhw_logf("[BOOT] video line: %s%s%s", line, note[0] ? " " : "", note);
}

static void finish_loading_screen(void) {
    int hold = xhw_splash_finish();
    xrc_settings_apply_hold(hold);
}

static void no_game(void) {
    static const char* const lines[] = {
        "This XBE is the N64Recomp-X platform and runtime alone.",
        "",
        "To play a game, build it from your own ROM on a PC:",
        "    n64rx build \"<your ROM>.z64\"",
        "and copy the folder it makes to the Xbox.",
        NULL,
    };
    xhw_splash_card_set("No game built in", "n64rx build <your ROM>", NULL);
    xhw_splash_stage("No game in this build");
    xhw_splash_progress(1.0f);
    finish_loading_screen();
    xhw_exit_reason("card: no game built in");
    xhw_error_card("No game built in", lines, 30);
}

/* games/test: the recompiled test program on the window, then its result */
static void test_rom(void) {
    static char l[6][72];
    const char* lines[7];
    recomp_context ctx;
    uint32_t magic, fails, steps, last;
    uint64_t t0, us;
    xhw_splash_stage("Preparing memory");
    xhw_splash_progress(0.2f);
    if (!xrc_mem_init(xrc_game.rdram_mb ? xrc_game.rdram_mb : 4)) xhw_fatal("Out of memory", "No room for the N64's RAM.");
    xhw_splash_stage("Loading code");
    xhw_splash_progress(0.5f);
    xrc_trace_init();
    if (!xrc_ovl_init()) xhw_fatal("Can't load the code", "The function table could not be built.");
    snprintf(l[0], sizeof l[0], "Starting %s", xrc_game.title);
    xhw_splash_stage(l[0]);
    xhw_splash_progress(0.8f);
    xrc_ctx_init(&ctx, 0x80000000u + xrc_rdram_size - 16);   /* the N64 stack at the top of the window */
    set_cop1_cs(0);
    t0 = xhw_time_us();
    xrc_game.entry(xrc_rdram, &ctx);
    us = xhw_time_us() - t0;
    set_cop1_cs(0);
    magic = xrc_word(TEST_RES);
    fails = xrc_word(TEST_RES + 4);
    steps = xrc_word(TEST_RES + 8);
    last = xrc_word(TEST_RES + 12);
    xhw_logf("[RECOMP] test: magic %08x, %u checks, %u failed (last bad value %08x), %u us, gpr %d", (unsigned)magic,
             (unsigned)steps, (unsigned)fails, (unsigned)last, (unsigned)us, xrc_game.gpr_bits);
    xhw_splash_progress(1.0f);
    finish_loading_screen();
    snprintf(l[0], sizeof l[0], "The recompiled test program ran on the RDRAM window.");
    snprintf(l[1], sizeof l[1], "Checks: %u, failed: %u%s", (unsigned)steps, (unsigned)fails,
             magic == TEST_MAGIC ? "" : " (it did not finish)");
    snprintf(l[2], sizeof l[2], "Registers: %d-bit; %u functions; %u us", xrc_game.gpr_bits,
             (unsigned)xrc_ovl_function_count(), (unsigned)us);
    l[3][0] = '\0';
    if (fails) snprintf(l[3], sizeof l[3], "Last failing value: %08x", (unsigned)last);
    lines[0] = l[0]; lines[1] = l[1]; lines[2] = l[2]; lines[3] = l[3]; lines[4] = NULL;
    xhw_exit_reason(magic == TEST_MAGIC && !fails ? "card: test ROM passed" : "card: test ROM FAILED");
    xhw_error_card(magic == TEST_MAGIC && !fails ? "Test ROM: passed" : "Test ROM: FAILED", lines, 30);
}

/* the renderer's start once the mode is set (the interpreter, M1); weak until it exists */
__attribute__((weak)) void xrc_gfx_boot(void) {}

/* what the IPL3 and the boot ROM leave in low RDRAM (libultra's os globals) */
static void boot_block(void) {
    uint32_t i;
    wr32(0x80000300, 1);                 /* osTvType: NTSC */
    wr32(0x80000304, 0);                 /* osRomType: cartridge */
    wr32(0x80000308, 0xB0000000u);       /* osRomBase */
    wr32(0x8000030C, 0);                 /* osResetType: cold */
    wr32(0x80000310, xrc_game.cic ? xrc_game.cic : 6102);   /* osCicId: the chip's number (Conker, Banjo check it) */
    wr32(0x80000314, 0);                 /* osVersion */
    wr32(0x80000318, xrc_rdram_size);    /* osMemSize */
    for (i = 0; i < 64; i += 4) wr32(0x8000031C + i, 0);   /* osAppNMIBuffer */
}

/* The IPL3 ends by copying its own last part from SP DMEM to RDRAM 0 and
 * running it there (`lw t5,0(t0); sw t5,0(t1)` from DMEM X to Y, t1 from
 * 0xA0000000 + D, then `jr 0x80000000 + D`; D is 4 in the 6105's, 0 in the
 * others): DMEM 0x4C0..0x774 in the common one,
 * 0x4C0..0x768 in Banjo-Kazooie's, other ranges in four more; all 268 ROMs
 * of the library have the copy. Below the exception vectors' few words that
 * osInitialize writes, the code stays in RDRAM, and games check it:
 * Banjo-Kazooie's viewport.c reads 0xA00001D8 and, without IPL3's word
 * there, draws with near 750 and far 1250 (its anti-tamper: the world cut
 * off near and far; maintainer, vbk1 on the console); Donkey Kong 64's
 * heap.c reads 0xA00002E8 and otherwise never frees its timed objects. DMEM holds ROM
 * 0x000..0xFFF, so the range is read from the ROM. DIVERGES: none meant
 * (what the console leaves); -DXRC_IPL3_LEFT=0: low RDRAM left zero. */
#ifndef XRC_IPL3_LEFT
#define XRC_IPL3_LEFT 1
#endif
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void ipl3_left(void) {
    static uint8_t ipl[0x1000];
    uint32_t i, j, x, y, d;
    if (!XRC_IPL3_LEFT || xu_rom_read(0, ipl, sizeof ipl)) return;   /* 0: read */
    for (i = 0x40; i < sizeof ipl; i += 4) {
        if (be32(ipl + i) != 0x3C09A000u) continue;   /* lui t1, 0xA000: the copy's destination */
        x = y = 0;
        for (j = i >= 0x68 ? i - 0x28 : 0x40; j < i; j += 4) {
            uint32_t w = be32(ipl + j);
            if (w >> 16 == 0x2508u) x = w & 0xFFFFu;   /* addiu t0, t0, X: the start in DMEM */
            if (w >> 16 == 0x256Bu) y = w & 0xFFFFu;   /* addiu t3, t3, Y: the end */
        }
        /* addiu t1, t1, D up to the loop's lw t5, 0(t0): the destination's
         * offset (4 in the 6105's, which runs the copy from 0x80000004) */
        d = 0;
        for (j = i + 4; j < i + 0x30 && j < sizeof ipl && be32(ipl + j) != 0x8D0D0000u; j += 4)
            if (be32(ipl + j) >> 16 == 0x2529u) d += (uint32_t)(int32_t)(int16_t)(be32(ipl + j) & 0xFFFFu);
        if (x < 0x40 || y <= x || y > sizeof ipl || d > 0x100u || y - x + d > 0x400u ||
            y - x + d > (xrc_game.entrypoint & 0x1FFFFFFFu))
            continue;
        xu_rom_to_rdram(x, 0x80000000u + d, y - x);
        xhw_logf("[BOOT] IPL3's last part in RDRAM %03x..%03x (DMEM %03x..%03x)", (unsigned)d, (unsigned)(d + y - x),
                 (unsigned)x, (unsigned)y);
        return;
    }
    xhw_log("[BOOT] IPL3: no copy to RDRAM found; low RDRAM left zero");
}

/* the pack's [rom] pin ranges held in the page cache (no-op when resident) */
static void rom_pins(void) {
    uint32_t i, chunks = 0;
    for (i = 0; i < xrc_game.num_rom_pins; i++)
        chunks += xhw_rom_pin(xrc_game.rom_pins[2 * i], xrc_game.rom_pins[2 * i + 1]);
    if (xrc_game.num_rom_pins)
        xhw_logf("[PI] %u pinned ranges: %u KB held%s", (unsigned)xrc_game.num_rom_pins, (unsigned)(chunks * 64),
                 xhw_rom_resident() ? " (resident ROM: nothing to do)" : "");
}

int xu_threads_started(void);   /* ultra/thread.c */

static void game(void) {
    xrom_target t;
    xrom_info info;
    recomp_context ctx;
    uint32_t boot, i;
    char line[72];
    memset(&t, 0, sizeof t);
    snprintf(t.title, sizeof t.title, "%s", xrc_game.title);
    snprintf(t.code, sizeof t.code, "%s", xrc_game.code ? xrc_game.code : "");
    t.size = xrc_game.rom_size;
    for (i = 0; i < 4 && xrc_game.sha1[i]; i++) snprintf(t.sha1[t.nsha1++], sizeof t.sha1[0], "%s", xrc_game.sha1[i]);
    xhw_rom_load_verified(&t, &info);   /* resident, or the page cache (NULL): read through xu_rom_* */
    rom_pins();

    xhw_splash_stage("Preparing memory");
    xhw_splash_progress(0.3f);
    if (!xrc_mem_init(xrc_game.rdram_mb ? xrc_game.rdram_mb : 4))
        xhw_fatal("Out of memory", "No room for the N64's RAM.");
    ipl3_left();
    boot_block();
    xrc_tlb_init();   /* a TLB miss goes to the game's refill vector or fault thread (tlb.c) */
    boot = xrc_game.boot_size;
    if (boot > info.size - 0x1000) boot = info.size - 0x1000;
    xu_rom_to_rdram(0x1000, xrc_game.entrypoint, boot);   /* the IPL3's copy */
    /* DIVERGES: the 6105 boot chip's IPL3 leaves boot magic in RDRAM that
     * games check (OoT's CIC6105_SaveBootMagicValues reads 0x2FE1C0;
     * Donkey Kong 64's boot waits until it holds IPL3's own word at ROM
     * 0x750). The word is copied from the ROM's IPL3; the second value
     * (0x2FB1F4) is not known and stays 0. */
    if (xrc_game.cic == 6105) xu_rom_to_rdram(0x750, 0x802FE1C0u, 4);
    xhw_logf("[RECOMP] boot segment: %u KB from ROM 0x1000 to %08x", (unsigned)(boot >> 10),
             (unsigned)xrc_game.entrypoint);

    xhw_splash_stage("Loading code");
    xhw_splash_progress(0.6f);
    xrc_trace_init();
    if (!xrc_ovl_init()) xhw_fatal("Can't load the code", "The function table could not be built.");

    snprintf(line, sizeof line, "Starting %s", xrc_game.title);
    xhw_splash_stage(line);
    xhw_splash_progress(1.0f);
    finish_loading_screen();
    /* the hold may have changed the settings: the mode follows them */
    xhw_video_set_pref(g_xrc_settings.output, g_xrc_settings.progressive);
    xhw_video_set_confirmed(g_xrc_settings.video_confirmed);
    xhw_video_boot();
    xrc_gfx_boot();

    xrc_ctx_init(&ctx, 0x80000000u + xrc_rdram_size - 16);
    set_cop1_cs(0);
    xhw_logf("[RECOMP] entrypoint %08x, %d-bit registers", (unsigned)xrc_game.entrypoint, xrc_game.gpr_bits);
    xrc_game.entry(xrc_rdram, &ctx);
    if (xu_threads_started()) {   /* threads run (the game's own dispatch started them): the boot code is done */
        xhw_log("[OS] boot code returned after starting the game's threads: the boot thread parks");
        for (;;) xhw_sleep_ms(1000);
    }
    xhw_fatal("The game returned", "The boot code came back; the game's threads never started.");
}

void xrc_boot(void) {
    xhw_logf("[RECOMP] N64Recomp-X %s, game %s, title id " XRC_TITLE_ID_HEX, XRC_VERSION,
             xrc_game.id ? xrc_game.id : "(none)");
    xrc_settings_load();
    video_line();
    if (!xrc_game.id) no_game();
    xhw_splash_card_set(xrc_game.title, xrc_game.test_rom ? "the ROM-free end-to-end test" : xrc_game.id,
                        xrc_game.icon);
    if (xrc_game.test_rom) test_rom();
    game();
}
