/* dryboot.c - the build-time dry boot (docs/roadmap.md B2).
 *
 * The ROM's boot run on the PC before the first build, so code the analyser
 * cannot see (decompressed by the CPU, copied out of the boot image, put
 * behind the TLB) is learnt without a console round. A CPU-only N64: the
 * VR4300 (the integer ISA with its 64-bit half, the FPU in both register
 * modes, COP0 with its exceptions, interrupts and 32-entry TLB) over RDRAM,
 * SP memory and a register model of the RCP that answers as the hardware
 * would and finishes every task at once:
 *   - PI DMA from the ROM (logged: where code came from), its interrupt
 *   - SI and the PIF: controller 1 there (no pak; START and A pressed on a
 *     schedule from frame 90, --press), a 4 Kbit EEPROM, nothing on the
 *     other channels
 *   - SP: a task "runs" when the halt bit clears and ends at once (halt,
 *     broke, the task-done signal); its interrupt, and the DP's for a
 *     graphics task, a little later
 *   - VI: a retrace every 781,250 instructions (one per Count tick: NTSC's
 *     46.875 MHz over 60 Hz), 937,500 for PAL
 *   - AI: two buffers in its FIFO, each done half a frame after it starts
 *   - MI: the interrupt lines and their mask
 * Nothing is drawn or played. The game's own entry code runs from the
 * IPL3's state (xbox/src/rt/boot.c: the boot segment, the os globals at
 * 0x80000300, the IPL3's tail at RDRAM 0), with its own libultra, threads
 * and exception handler, so its loaders and decompressors run as they would
 * on the console.
 *
 * What it writes is what the console's safety net writes (xbox/src/rt/
 * learn.c), into <out>/learn.txt and <out>/learn_code.bin, for n64rx's
 * --learn: each function entered by a call (or a thread's first dispatch)
 * that the analysis does not have (--known), with the newest PI DMA that put
 * its bytes there ("dma rom ram size", then "func vram 4 from caller"), or,
 * when the CPU wrote them, its 4 KB page as a code record; "kuseg" for each
 * 1 MB of KUSEG the TLB maps code into; "rdram 8" when code runs past 4 MB.
 *
 * Usage: dryboot <rom.z64> <out dir> --entry X --boot-size N [--cic N]
 *        [--tv 0|1|2] [--rdram 4|8] [--known file] [--frames N] [--press F|0] [--want file] [-v]
 * The ROM in z64 order (n64rx's rom.z64: the image the console runs).
 * Exit 0 with the files written, 1 on bad arguments, 2 when the CPU met an
 * instruction it does not know (said on stderr; the files are still
 * written). Host C99, no dependencies. */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- memory */
static uint8_t* s_rom;
static uint32_t s_romsize;
static uint8_t* s_ram;      /* N64 byte order */
static uint32_t s_ramsize;
/* what the CPU reaches of it through KSEG1: with --rdram 4, 4 MB, as the
 * console runtime's uncached alias of its window (mem.c) and a 4 MB N64;
 * past that a load reads 0 and a store goes nowhere. F-Zero X sizes RDRAM
 * itself (libultra's osGetMemSize: a word at each MB through KSEG1, read
 * back): it found 8 MB here, ran code past 4 MB and spun there (no RSP
 * task in 1,800 frames; now its audio tasks run, its graphics still wait
 * on something else). The console runtime answers osGetMemSize itself. KSEG0 keeps all 8 MB: Army Men and Supercross 2000 run a little
 * past 4 MB there, as on the console (its guard and the pages after it).
 * --window keep: KSEG1 too, as before */
static uint32_t s_k1ram;
static uint8_t s_sp[0x2000];
static uint8_t s_pif[64];
static uint8_t s_eep[2048];
static int s_eep16;   /* --eep 16: a 16 Kbit EEPROM (2 KB), else 4 Kbit */
/* --versions: which superseded versions of a page are kept (write_captures): 0 last (only an older
 * one that comes back), 1 all, 2 boot (the default: all but those saved before the first graphics task) */
static int s_versions = 2;
static uint32_t* s_cpuw;    /* a bit per RDRAM word: written by the CPU (or the RSP) since a DMA from the ROM */
static int s_verbose;

static inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void wbe32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }

static void say(const char* fmt, ...) {
    va_list ap;
    if (!s_verbose) return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* -------------------------------------------------------------- the CPU */
typedef struct { uint32_t mask, hi, lo0, lo1; } Tlb;
static struct {
    uint64_t r[32], hi, lo;
    uint32_t pc;            /* the instruction now */
    int delay;              /* pc is a branch's delay slot */
    uint32_t target;        /* where a taken branch goes after its slot */
    int taken, skip, eret;  /* this instruction: a branch taken, a likely one's slot skipped, an eret */
    uint32_t eret_to;
    uint64_t cp0[32];
    uint64_t f[32];
    uint32_t fcr31;
    Tlb tlb[32];
    int exc;                /* an exception the instruction raised (its code + 1) */
    uint32_t badv;
    int llbit;
} C;
static uint64_t s_cycles, s_next_event, s_exec;
static int s_unknown;       /* an instruction the CPU does not know: stop */
static int s_exl_calls;     /* --exl-calls 1: calls made in exception context are learnt too (as before) */
static int s_in_exc;        /* between an exception and its eret: the game's exception code runs */
static int s_tables = 1;    /* --jump-tables 0: a jr's table page is not captured (as before) */
static uint32_t s_lw_ea[32], s_lw_val[32];   /* each register's last lw: where from, what */
static uint32_t s_bad_w, s_bad_pc;
enum { INDEX, RANDOM, ENTRYLO0, ENTRYLO1, CONTEXT, PAGEMASK, WIRED, BADVADDR = 8, COUNT, ENTRYHI, COMPARE, STATUS,
       CAUSE, EPC, PRID, CONFIG, LLADDR, XCONTEXT = 20, ERROREPC = 30 };
enum { EXC_INT = 0, EXC_MOD = 1, EXC_TLBL = 2, EXC_TLBS = 3, EXC_ADEL = 4, EXC_ADES = 5, EXC_SYS = 8, EXC_BP = 9,
       EXC_RI = 10, EXC_CPU = 11, EXC_OV = 12, EXC_TR = 13, EXC_FPE = 15 };
#define SR_IE 1u
#define SR_EXL 2u
#define SR_ERL 4u
#define SR_CU1 0x20000000u
#define SR_FR 0x04000000u
#define SX32(v) ((uint64_t)(int64_t)(int32_t)(uint32_t)(v))

/* ----------------------------------------------------------- the RCP */
enum { MI_SP = 1, MI_SI = 2, MI_AI = 4, MI_VI = 8, MI_PI = 0x10, MI_DP = 0x20 };
static uint32_t s_mi_intr, s_mi_mask, s_mi_mode;
static uint32_t s_sp_reg[8], s_sp_status = 1, s_sp_pc;   /* halted */
static uint32_t s_dp_reg[8];
static uint32_t s_vi_reg[14];
static uint32_t s_ai_dram, s_ai_len[2], s_ai_n, s_ai_ctl, s_ai_rate;
static uint64_t s_ai_end;
static uint32_t s_pi_reg[13];
static uint32_t s_si_dram;
static uint32_t s_frame = 781250;
static uint64_t s_next_vi, s_sp_done_at, s_dp_done_at;
static unsigned s_frames, s_tasks, s_gfx_tasks;
static unsigned s_snap_at;         /* --snap: RDRAM at the start of this graphics task (1 = the first) */
static const char* s_snap_path;

/* the PI DMAs from the ROM, newest last (where code came from) */
typedef struct { uint32_t rom, ram, size; } Dma;
static Dma* s_dma;
static unsigned s_ndma, s_cap_dma;

/* A 4 KB page: its code overwritten since its capture (a DMA over it, or a CPU store to a word that
 * ran: data beside code changing is no new version of the code, and versions of every page made the
 * naming see each libultra function twice); captured; code ran there (KSEG0). s_pgen: the same
 * overwrites counted, for TLB-mapped pages */
static uint8_t s_pdirty[2048], s_pcap[2048], s_pexec[2048];
static uint32_t s_pgen[2048];
static uint32_t* s_ran;     /* a bit per RDRAM word: an instruction ran there */
static void cpuw_mark(uint32_t pa, uint32_t n, int cpu) {
    uint32_t w = pa >> 2, e = (pa + n + 3) >> 2;
    if (e > s_ramsize >> 2) e = s_ramsize >> 2;
    if (!cpu && pa < s_ramsize) {
        uint32_t pg;
        for (pg = pa >> 12; pg <= ((pa + n - 1) >> 12) && pg < 2048; pg++) s_pgen[pg]++, s_pdirty[pg] = 1;
    }
    for (; cpu && w < e; w++)
        if (s_ran[w >> 5] & (1u << (w & 31))) {
            s_pgen[w >> 10]++, s_pdirty[w >> 10] = 1;
            break;
        }
    w = pa >> 2;
    for (; w < e; w++) {
        if (cpu) s_cpuw[w >> 5] |= 1u << (w & 31);
        else s_cpuw[w >> 5] &= ~(1u << (w & 31));
    }
}

/* a page the CPU made: most of its words written by it (a patched word or the exception vectors
 * osInitialize writes over the boot image leave the page the ROM's, which the analysis has) */
static int page_cpu(uint32_t pa) {
    unsigned bits = 0, k;
    if (pa >= s_ramsize) return 0;
    for (k = 0; k < 32; k++) bits += (unsigned)__builtin_popcount(s_cpuw[(pa >> 12) * 32 + k]);
    return bits >= 512;
}

/* a page that looks like code: a jr $ra and a stack frame (addiu sp, sp, -n), or two returns (the
 * block around code that ran is taken page by page while it does: Banjo-Kazooie's heap is CPU-written too) */
static int page_code(uint32_t pa) {
    uint32_t k, jr = 0, frame = 0;
    if (pa + 4096 > s_ramsize) return 0;
    for (k = 0; k < 4096; k += 4) {
        uint32_t w = be32(&s_ram[(pa & ~0xFFFu) + k]);
        if (w == 0x03E00008u) jr++;
        else if ((w >> 16) == 0x27BDu && (w & 0x8000u)) frame++;
    }
    return (jr && frame) || jr >= 2;
}

static void update_ip(void) {
    if (s_mi_intr & s_mi_mask) C.cp0[CAUSE] |= 0x400;
    else C.cp0[CAUSE] &= ~(uint64_t)0x400;
}
static void raise_mi(uint32_t b) { s_mi_intr |= b; update_ip(); }
static void clear_mi(uint32_t b) { s_mi_intr &= ~b; update_ip(); }

static void pi_dma(void) {   /* PI_WR_LEN: the cartridge to RDRAM */
    uint32_t ram = s_pi_reg[0] & 0x00FFFFFEu, cart = s_pi_reg[1] & 0x1FFFFFFEu, n = (s_pi_reg[3] & 0x00FFFFFFu) + 1, i;
    if (cart >= 0x10000000u && cart < 0x1FC00000u) {
        uint32_t off = cart - 0x10000000u;
        for (i = 0; i < n && ram + i < s_ramsize; i++) s_ram[ram + i] = off + i < s_romsize ? s_rom[off + i] : 0;
        cpuw_mark(ram, n, 0);
        if (s_ndma == s_cap_dma) {
            s_cap_dma = s_cap_dma ? s_cap_dma * 2 : 4096;
            s_dma = (Dma*)realloc(s_dma, s_cap_dma * sizeof *s_dma);
            if (!s_dma) exit(3);
        }
        s_dma[s_ndma++] = (Dma){off, ram, n};
        say("[PI] dma rom %06x -> %08x, %x bytes", off, ram | 0x80000000u, n);
    } else {   /* the save domain, the 64DD: nothing there (all ones) */
        for (i = 0; i < n && ram + i < s_ramsize; i++) s_ram[ram + i] = 0xFF;
        cpuw_mark(ram, n, 1);
    }
    raise_mi(MI_PI);
}

/* Controller 1's buttons: from --press's frame on, START, then A, A, A, every 45 frames, each held
 * for 4 (title screens and menus give way to the game, whose code then loads: Mario Tennis's
 * matches); none before it (the boot), none with --press 0 */
static uint32_t s_press_from = 90;
static int s_press_mode;   /* --press-mode 1: START once, then A only */
static unsigned s_pad_reads, s_pad_pressed, s_pif_runs, s_pif_cmds[256];
/* --script: an autopad script (docs/testing.md "Scripted input": `t<ms> BUTTONS [for <ms>]` or
 * `<frame> BUTTONS [for <frames>]`, ms at 60 frames a second from the boot), the Xbox buttons in the
 * port's default layout: A, B, START, X as B, LT as Z, RT as R, the d-pad as the stick at full
 * tilt; BACK, WHITE, BLACK and SHOT are the port's own and do nothing here */
#define MAX_EVENTS 1024
static struct { uint32_t from, to; uint16_t buttons; int8_t x, y; } s_ev[MAX_EVENTS];
static unsigned s_nev;
static int8_t s_stick_x, s_stick_y;
static void script_load(const char* path) {
    FILE* f = fopen(path, "r");
    char line[256];
    if (!f) { fprintf(stderr, "dryboot: cannot read the script %s\n", path); return; }
    while (fgets(line, sizeof line, f) && s_nev < MAX_EVENTS) {
        char* t = strtok(line, " \t\r\n");
        int ms;
        uint32_t at, len = 6;
        uint16_t b = 0;
        int8_t x = 0, y = 0;
        if (!t || t[0] == '#') continue;
        ms = t[0] == 't';
        at = (uint32_t)strtoul(t + ms, NULL, 10);
        while ((t = strtok(NULL, " \t\r\n")) != NULL) {
            if (!strcmp(t, "for")) {
                if ((t = strtok(NULL, " \t\r\n")) != NULL) len = (uint32_t)strtoul(t, NULL, 10);
                break;
            }
            if (!strcmp(t, "A")) b |= 0x8000;
            else if (!strcmp(t, "B") || !strcmp(t, "X")) b |= 0x4000;
            else if (!strcmp(t, "START")) b |= 0x1000;
            else if (!strcmp(t, "L")) b |= 0x2000;   /* the left trigger: Z */
            else if (!strcmp(t, "R")) b |= 0x0010;
            else if (!strcmp(t, "LEFT") || !strcmp(t, "SL")) x = -80;
            else if (!strcmp(t, "RIGHT") || !strcmp(t, "SR")) x = 80;
            else if (!strcmp(t, "UP") || !strcmp(t, "SU")) y = 80;
            else if (!strcmp(t, "DOWN") || !strcmp(t, "SD")) y = -80;
        }
        if (ms) at = at * 60u / 1000u, len = len * 60u / 1000u;
        if (!len) len = 1;
        s_ev[s_nev].from = at, s_ev[s_nev].to = at + len, s_ev[s_nev].buttons = b, s_ev[s_nev].x = x, s_ev[s_nev].y = y;
        s_nev++;
    }
    fclose(f);
}

static uint16_t pad_buttons(void) {
    uint32_t f;
    if (s_nev) {
        uint16_t b = 0;
        unsigned i;
        s_stick_x = s_stick_y = 0;
        for (i = 0; i < s_nev; i++)
            if (s_frames >= s_ev[i].from && s_frames < s_ev[i].to) {
                b |= s_ev[i].buttons;
                if (s_ev[i].x) s_stick_x = s_ev[i].x;
                if (s_ev[i].y) s_stick_y = s_ev[i].y;
            }
        return b;
    }
    if (!s_press_from || s_frames < s_press_from) return 0;
    f = s_frames - s_press_from;
    if (f % 45 >= 4) return 0;
    if (s_press_mode == 1) return f / 45 == 0 ? 0x1000 : 0x8000;   /* START once, then A */
    return (f / 45) % 4 == 0 ? 0x1000 : 0x8000;   /* START, A */
}

/* the PIF's joybus block: controller 1 (no pak), the EEPROM on 4 */
static void pif_run(void) {
    int i = 0, ch = 0;
    s_pif_runs++;
    while (i < 63) {
        int tx = s_pif[i], rx, at;
        if (tx == 0xFE) break;
        if (tx == 0xFF || tx == 0xFD) { i++; continue; }
        if (tx == 0) { ch++, i++; continue; }
        if (i + 1 >= 63) break;
        s_pif[i + 1] &= 0x3F;   /* the error bits: this run's */
        rx = s_pif[i + 1];
        at = i + 2 + (tx & 0x3F);
        if (at + rx > 63) break;
        {
            uint8_t cmd = s_pif[i + 2], *res = &s_pif[at];
            s_pif_cmds[(ch << 4 | (cmd & 15)) & 255]++;
            int ok = 1;
            if (ch == 0 && (cmd == 0x00 || cmd == 0xFF) && rx >= 3) res[0] = 0x05, res[1] = 0x00, res[2] = 0x00;
            else if (ch == 0 && cmd == 0x01 && rx >= 4) {   /* the buttons: the press schedule's (--press) */
                uint16_t b = pad_buttons();
                s_pad_reads++, s_pad_pressed += b != 0;
                res[0] = (uint8_t)(b >> 8), res[1] = (uint8_t)b, res[2] = (uint8_t)s_stick_x, res[3] = (uint8_t)s_stick_y;
            }
            else if (ch == 4 && (cmd == 0x00 || cmd == 0xFF) && rx >= 3)
                res[0] = 0x00, res[1] = s_eep16 ? 0xC0 : 0x80, res[2] = 0x00;   /* the EEPROM's type */
            else if (ch == 4 && cmd == 0x04 && rx >= 8) memcpy(res, &s_eep[(s_pif[i + 3] & (s_eep16 ? 255 : 63)) * 8], 8);
            else if (ch == 4 && cmd == 0x05 && tx >= 10)
                memcpy(&s_eep[(s_pif[i + 3] & (s_eep16 ? 255 : 63)) * 8], &s_pif[i + 4], 8), res[0] = 0;
            else ok = 0;
            if (!ok) s_pif[i + 1] |= 0x80;   /* no answer */
        }
        i = at + rx;
        ch++;
    }
}

/* the joybus block runs on every read of PIF RAM once a write armed it (bit 0 of its last byte):
 * libultra writes the read command once and then only reads (osContStartReadData) */
static int s_pif_armed;
static void si_dma(int to_pif) {
    uint32_t ram = s_si_dram & 0x00FFFFFCu, i;
    if (to_pif) {
        for (i = 0; i < 64 && ram + i < s_ramsize; i++) s_pif[i] = s_ram[ram + i];
        s_pif_armed = s_pif[63] & 1;
        if (s_pif_armed) pif_run();
        s_pif[63] = 0;
    } else {
        if (s_pif_armed) pif_run();
        for (i = 0; i < 64 && ram + i < s_ramsize; i++) s_ram[ram + i] = s_pif[i];
        cpuw_mark(ram, 64, 1);
    }
    raise_mi(MI_SI);
}

static void sp_dma(int to_ram, uint32_t v) {
    uint32_t len = (v & 0xFFF) + 1, count = ((v >> 12) & 0xFF) + 1, skip = v >> 20, k, i;
    uint32_t mem = s_sp_reg[0] & 0x1FF8u, dram = s_sp_reg[1] & 0x00FFFFF8u;
    len = (len + 7) & ~7u;
    for (k = 0; k < count; k++) {
        for (i = 0; i < len; i++) {
            uint32_t m = (mem & 0x1000u) | ((mem + i) & 0xFFFu), d = dram + i;
            if (d >= s_ramsize) break;
            if (to_ram) s_ram[d] = s_sp[m];
            else s_sp[m] = s_ram[d];
        }
        if (to_ram) cpuw_mark(dram, len, 1);
        mem += len, dram += len + skip;
    }
}

static void sp_status_write(uint32_t v) {
    int was_halted = s_sp_status & 1, b;
    if (v & 1) s_sp_status &= ~1u;
    if (v & 2) s_sp_status |= 1u;
    if (v & 4) s_sp_status &= ~2u;
    if (v & 8) clear_mi(MI_SP);
    if (v & 0x10) raise_mi(MI_SP);
    if (v & 0x80) s_sp_status &= ~0x40u;
    if (v & 0x100) s_sp_status |= 0x40u;
    for (b = 0; b < 8; b++) {
        if (v & (0x200u << (2 * b))) s_sp_status &= ~(0x80u << b);
        if (v & (0x400u << (2 * b))) s_sp_status |= 0x80u << b;
    }
    if (was_halted && !(s_sp_status & 1)) {   /* a task: done at once */
        uint32_t type = be32(&s_sp[0xFC0]);
        s_tasks++;
        s_sp_status |= 1u | 2u | (0x80u << 2);   /* halt, broke, sig2 (task done) */
        if (s_sp_status & 0x40) s_sp_done_at = s_cycles + 3000;
        if (type == 1) s_dp_done_at = s_cycles + 6000, s_gfx_tasks++;
        if (type == 1)
            say("[SP] graphics task %u: data %08x, %x bytes, frame %u", s_gfx_tasks, be32(&s_sp[0xFF0]), be32(&s_sp[0xFF4]), s_frames);
        if (type == 1 && s_snap_path && s_gfx_tasks == s_snap_at) {   /* the display list as the CPU left it */
            FILE* f = fopen(s_snap_path, "wb");
            char tp[1024];
            if (f) fwrite(s_ram, 1, s_ramsize, f), fclose(f);
            snprintf(tp, sizeof tp, "%s.task", s_snap_path);   /* and its OSTask, as DMEM has it (dryboot_frame.py) */
            if ((f = fopen(tp, "wb"))) fwrite(&s_sp[0xFC0], 1, 64, f), fclose(f);
            fprintf(stderr, "dryboot: RDRAM at graphics task %u (data %08x, frame %u) to %s\n", s_gfx_tasks, be32(&s_sp[0xFF0]), s_frames, s_snap_path);
        }
    }
}

static void ai_start(uint32_t len) {
    if (!len) return;
    if (s_ai_n < 2) s_ai_len[s_ai_n++] = len;
    if (s_ai_n == 1) s_ai_end = s_cycles + s_frame / 2;
}

/* ---------------------------------------------------------- the bus */
static uint32_t reg_read(uint32_t pa) {
    uint32_t r = (pa & 0xFFFFFu) >> 2;
    switch (pa >> 20) {
    case 0x040:
        if (pa < 0x04040000u) return be32(&s_sp[pa & 0x1FFCu]);
        if (pa >= 0x04080000u) return s_sp_pc;
        r = (pa & 0x1Fu) >> 2;   /* SP_MEM_ADDR .. SP_SEMAPHORE */
        if (r == 4) return s_sp_status;
        if (r == 7) { uint32_t s = s_sp_reg[7]; s_sp_reg[7] = 1; return s; }
        if (r == 5 || r == 6) return 0;
        return r < 8 ? s_sp_reg[r] : 0;
    case 0x041: return r == 3 ? 0x80u : r < 8 ? s_dp_reg[r] : 0;   /* DPC_STATUS: the command buffer ready */
    case 0x043:
        if (r == 0) return s_mi_mode;
        if (r == 1) return 0x02020102u;
        if (r == 2) return s_mi_intr;
        if (r == 3) return s_mi_mask;
        return 0;
    case 0x044:
        if (r == 4) return (uint32_t)(((s_cycles % s_frame) * 525u / s_frame) & ~1u);
        return r < 14 ? s_vi_reg[r] : 0;
    case 0x045:
        if (r == 1) {
            if (!s_ai_n) return 0;
            if (s_ai_end <= s_cycles) return 0;
            return (uint32_t)((uint64_t)s_ai_len[0] * (s_ai_end - s_cycles) / (s_frame / 2)) & ~7u;
        }
        if (r == 3) return (s_ai_n >= 2 ? 0x80000000u : 0) | (s_ai_n ? 0x40000000u : 0);
        return 0;
    case 0x046: return r == 4 ? 0 : r < 13 ? s_pi_reg[r] : 0;
    case 0x047: return 0;
    case 0x048: return r == 6 ? ((s_mi_intr & MI_SI) ? 0x1000u : 0) : r == 0 ? s_si_dram : 0;
    }
    return 0;
}

static void reg_write(uint32_t pa, uint32_t v) {
    uint32_t r = (pa & 0xFFFFFu) >> 2;
    switch (pa >> 20) {
    case 0x040:
        if (pa < 0x04040000u) { wbe32(&s_sp[pa & 0x1FFCu], v); return; }
        if (pa >= 0x04080000u) { s_sp_pc = v & 0xFFC; return; }
        r = (pa & 0x1Fu) >> 2;
        if (r == 0 || r == 1) s_sp_reg[r] = v;
        else if (r == 2) sp_dma(0, v);
        else if (r == 3) sp_dma(1, v);
        else if (r == 4) sp_status_write(v);
        else if (r == 7) s_sp_reg[7] = 0;
        return;
    case 0x041: if (r < 8) s_dp_reg[r] = v; return;
    case 0x043:
        if (r == 0) {
            s_mi_mode = v & 0x7F;
            if (v & 0x800) clear_mi(MI_DP);
        } else if (r == 3) {
            static const uint32_t bit[6] = {MI_SP, MI_SI, MI_AI, MI_VI, MI_PI, MI_DP};
            int i;
            for (i = 0; i < 6; i++) {
                if (v & (1u << (2 * i))) s_mi_mask &= ~bit[i];
                if (v & (2u << (2 * i))) s_mi_mask |= bit[i];
            }
            update_ip();
        }
        return;
    case 0x044:
        if (r == 4) clear_mi(MI_VI);
        else if (r < 14) s_vi_reg[r] = v;
        return;
    case 0x045:
        if (r == 0) s_ai_dram = v;
        else if (r == 1) ai_start(v & 0x3FFF8u);
        else if (r == 2) s_ai_ctl = v;
        else if (r == 3) clear_mi(MI_AI);
        else if (r == 4) s_ai_rate = v;
        return;
    case 0x046:
        if (r == 4) { if (v & 2) clear_mi(MI_PI); return; }
        if (r < 13) s_pi_reg[r] = v;
        if (r == 3) pi_dma();
        return;
    case 0x048:
        if (r == 0) s_si_dram = v;
        else if (r == 1) si_dma(0);   /* PIF_ADDR_RD64B */
        else if (r == 4) si_dma(1);   /* PIF_ADDR_WR64B */
        else if (r == 6) clear_mi(MI_SI);
        return;
    }
}

/* physical reads and writes, n bytes (1, 2, 4, 8), big-endian values */
static uint64_t phys_read(uint32_t pa, int n) {
    if (pa < s_ramsize) {
        uint64_t v = 0;
        int i;
        if (pa + (uint32_t)n > s_ramsize) return 0;
        for (i = 0; i < n; i++) v = v << 8 | s_ram[pa + i];
        return v;
    }
    if (pa >= 0x10000000u && pa < 0x1FC00000u) {
        uint32_t off = pa - 0x10000000u;
        uint64_t v = 0;
        int i;
        for (i = 0; i < n; i++) v = v << 8 | (off + i < s_romsize ? s_rom[off + i] : 0);
        return v;
    }
    if (pa >= 0x1FC007C0u && pa < 0x1FC00800u) {
        uint64_t v = 0;
        int i;
        for (i = 0; i < n; i++) v = v << 8 | s_pif[(pa - 0x1FC007C0u + i) & 63];
        return v;
    }
    if (pa >= 0x04000000u && pa < 0x04900000u) {
        if (pa < 0x04040000u && n != 4) {   /* SP memory, any width */
            uint64_t v = 0;
            int i;
            for (i = 0; i < n; i++) v = v << 8 | s_sp[(pa + i) & 0x1FFF];
            return v;
        }
        {
            uint32_t w = reg_read(pa & ~3u);
            if (n == 4) return w;
            if (n == 8) return (uint64_t)w << 32 | reg_read((pa & ~3u) + 4);
            return (w >> (8 * (4 - n - (pa & 3)))) & ((1u << (8 * n)) - 1);
        }
    }
    return 0;   /* the 64DD, the save domain, open bus */
}

static void phys_write(uint32_t pa, uint64_t v, int n) {
    if (pa < s_ramsize) {
        int i;
        if (pa + (uint32_t)n > s_ramsize) return;
        for (i = n - 1; i >= 0; i--, v >>= 8) s_ram[pa + i] = (uint8_t)v;
        cpuw_mark(pa, (uint32_t)n, 1);
        return;
    }
    if (pa >= 0x1FC007C0u && pa < 0x1FC00800u) {
        int i;
        for (i = n - 1; i >= 0; i--, v >>= 8) s_pif[(pa - 0x1FC007C0u + i) & 63] = (uint8_t)v;
        return;
    }
    if (pa >= 0x04000000u && pa < 0x04040000u) {
        int i;
        for (i = n - 1; i >= 0; i--, v >>= 8) s_sp[(pa + i) & 0x1FFF] = (uint8_t)v;
        return;
    }
    if (pa >= 0x04040000u && pa < 0x04900000u) {
        if (n == 8) reg_write(pa & ~7u, (uint32_t)(v >> 32)), reg_write((pa & ~7u) + 4, (uint32_t)v);
        else reg_write(pa & ~3u, (uint32_t)v << (8 * (4 - n - (pa & 3))));
    }
}

/* ------------------------------------------------------- the TLB, exceptions */
static unsigned s_excs[32];
/* every EPC an exception saved: an eret back to one resumes a thread where it was stopped (no
 * function starts there, though the instruction may not have run yet: an interrupt right after
 * a store that raised it) */
#define MAX_EPC (1u << 16)
static uint32_t s_epc[MAX_EPC];
static void epc_add(uint32_t v) {
    uint32_t h = (v >> 2) * 2654435761u, i;
    for (i = 0; i < 64; i++) {
        uint32_t* e = &s_epc[(h + i) & (MAX_EPC - 1)];
        if (*e == v) return;
        if (!*e) { *e = v; return; }
    }
}
static int epc_has(uint32_t v) {
    uint32_t h = (v >> 2) * 2654435761u, i;
    for (i = 0; i < 64; i++) {
        uint32_t e = s_epc[(h + i) & (MAX_EPC - 1)];
        if (e == v) return 1;
        if (!e) return 0;
    }
    return 1;   /* (a crowded run: taken as a resume point) */
}
#define NHIST 4096
static uint32_t s_hist_pc[NHIST], s_hist_n[NHIST];
static void sample(uint32_t pc) {
    uint32_t h = (pc >> 2) * 2654435761u, i;
    for (i = 0; i < NHIST; i++) {
        uint32_t k = (h + i) & (NHIST - 1);
        if (s_hist_pc[k] == pc || !s_hist_pc[k]) { s_hist_pc[k] = pc, s_hist_n[k]++; return; }
    }
}
static void exception(int code, int bd_vec_refill) {
    uint64_t sr = C.cp0[STATUS];
    s_excs[code & 31]++;
    uint32_t vec = 0x80000180u;
    if (s_verbose && code && s_excs[code & 31] <= 8)   /* -v: what faulted, the first of each kind */
        fprintf(stderr, "exception %d at %08x%s (badvaddr %08llx, ra %08llx, frame %u)\n", code, C.pc,
                C.delay ? " (delay slot)" : "", (unsigned long long)C.cp0[BADVADDR], (unsigned long long)C.r[31], s_frames);
    if (!(sr & SR_EXL)) {
        C.cp0[EPC] = SX32(C.delay ? C.pc - 4 : C.pc);
        epc_add((uint32_t)C.cp0[EPC]);
        if (C.delay) C.cp0[CAUSE] |= 0x80000000u;
        else C.cp0[CAUSE] &= ~(uint64_t)0x80000000u;
        if (bd_vec_refill) vec = 0x80000000u;
    }
    if (sr & 0x400000) vec += 0x3FC00000u;   /* BEV: the boot vectors */
    C.cp0[CAUSE] = (C.cp0[CAUSE] & ~(uint64_t)0x7C) | (uint64_t)(code << 2);
    C.cp0[STATUS] |= SR_EXL;
    C.pc = vec;
    C.delay = 0;
    s_in_exc = 1;
}

static void tlb_fault(uint32_t va, int code, int refill) {
    C.cp0[BADVADDR] = SX32(va);
    C.cp0[CONTEXT] = (C.cp0[CONTEXT] & ~(uint64_t)0x7FFFF0) | ((va >> 9) & 0x7FFFF0);
    C.cp0[XCONTEXT] = (C.cp0[XCONTEXT] & ~(uint64_t)0x7FFFFFF0) | ((va >> 9) & 0x7FFFF0);
    C.cp0[ENTRYHI] = SX32((va & 0xFFFFE000u) | (C.cp0[ENTRYHI] & 0xFF));
    C.exc = code + 1;
    C.badv = va;
    C.cp0[CAUSE] |= refill ? 0x100000000ull : 0;   /* (a marker, cleared at the exception) */
}

/* the virtual address to a physical one; -1 (and C.exc, unless quiet) on a miss */
static int64_t translate_q(uint32_t va, int store, int quiet) {
    int i;
    if ((va & 0xC0000000u) == 0x80000000u) return va & 0x1FFFFFFFu;   /* KSEG0, KSEG1 */
    for (i = 0; i < 32; i++) {
        const Tlb* t = &C.tlb[i];
        uint32_t m = t->mask | 0x1FFFu;
        if ((va & ~m) != (t->hi & ~m)) continue;
        if (!((t->lo0 & t->lo1) & 1) && (t->hi & 0xFF) != (C.cp0[ENTRYHI] & 0xFF)) continue;
        {
            uint32_t half = (m + 1) >> 1, lo = (va & half) ? t->lo1 : t->lo0;
            if (!(lo & 2)) { if (!quiet) tlb_fault(va, store ? EXC_TLBS : EXC_TLBL, 0); return -1; }
            if (store && !(lo & 4)) { if (!quiet) tlb_fault(va, EXC_MOD, 0); return -1; }
            return (int64_t)((((lo >> 6) & 0xFFFFFu) << 12) + (va & (half - 1)));
        }
    }
    if (!quiet) tlb_fault(va, store ? EXC_TLBS : EXC_TLBL, 1);
    return -1;
}
static int64_t translate(uint32_t va, int store) { return translate_q(va, store, 0); }

static int rd(uint32_t va, int n, uint64_t* out) {
    int64_t pa;
    if (va & (uint32_t)(n - 1)) {
        C.cp0[BADVADDR] = SX32(va), C.exc = EXC_ADEL + 1;
        return 0;
    }
    pa = translate(va, 0);
    if (pa < 0) return 0;
    *out = (va >> 29) == 5 && pa >= s_k1ram && pa < s_ramsize ? 0 : phys_read((uint32_t)pa, n);
    return 1;
}
static uint32_t s_watch_lo, s_watch_hi;   /* --watch: the CPU's stores to these physical bytes, logged */
static unsigned s_watch_n;
static int wr(uint32_t va, uint64_t v, int n) {
    int64_t pa;
    if (va & (uint32_t)(n - 1)) {
        C.cp0[BADVADDR] = SX32(va), C.exc = EXC_ADES + 1;
        return 0;
    }
    pa = translate(va, 1);
    if (pa < 0) return 0;
    if (s_watch_hi && (uint32_t)pa < s_watch_hi && (uint32_t)pa + (uint32_t)n > s_watch_lo && s_watch_n < 400) {
        s_watch_n++;
        fprintf(stderr, "watch: %08x <- %0*llx (%d bytes) at pc %08x, ra %08x, frame %u\n", (unsigned)pa, n * 2,
                (unsigned long long)v, n, C.pc, (uint32_t)C.r[31], s_frames);
    }
    if (!((va >> 29) == 5 && pa >= s_k1ram && pa < s_ramsize)) phys_write((uint32_t)pa, v, n);
    return 1;
}

static void tlb_write(int i) {
    static unsigned s_said;
    Tlb* t = &C.tlb[i & 31];
    t->mask = (uint32_t)C.cp0[PAGEMASK] & 0x01FFE000u;
    t->hi = (uint32_t)C.cp0[ENTRYHI] & ~(t->mask | 0x1F00u);
    t->lo0 = (uint32_t)C.cp0[ENTRYLO0] & 0x3FFFFFFFu;
    t->lo1 = (uint32_t)C.cp0[ENTRYLO1] & 0x3FFFFFFFu;
    if (s_verbose && s_said++ < 48)
        fprintf(stderr, "tlb %2d: mask %08x hi %08x lo0 %08x lo1 %08x (pc %08x)\n", i & 31, t->mask, t->hi, t->lo0, t->lo1, C.pc);
}

/* ---------------------------------------------------- what is learnt */
static uint32_t* s_known;   /* sorted */
static size_t s_nknown;
static uint32_t s_boot_lo, s_boot_hi;   /* the IPL3's copy, physical */
static FILE* s_txt;
static FILE* s_bin;
#define MAX_SEEN (1u << 16)
static uint32_t s_seen[MAX_SEEN];   /* function entries handled (open addressing, 0 free) */
static unsigned s_nseen, s_nfunc, s_ncode;
static uint32_t s_kuseg_seen[64];
static unsigned s_nkuseg;
static int s_rdram8;
static uint32_t s_rdram_mb = 4;   /* osMemSize's (--rdram) */
#define MAX_PAGES 4096
static struct { uint32_t va, hash; } s_pages[MAX_PAGES];
static unsigned s_npages;

static int known(uint32_t v) {
    size_t lo = 0, hi = s_nknown;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (s_known[m] < v) lo = m + 1;
        else hi = m;
    }
    return lo < s_nknown && s_known[lo] == v;
}
static int seen(uint32_t v) {
    uint32_t h = (v >> 2) * 2654435761u, i;
    for (i = 0; i < MAX_SEEN; i++) {
        uint32_t* s = &s_seen[(h + i) & (MAX_SEEN - 1)];
        if (*s == v) return 1;
        if (!*s) {
            if (s_nseen + 1 >= MAX_SEEN * 3 / 4) return 1;   /* full: say seen */
            *s = v, s_nseen++;
            return 0;
        }
    }
    return 1;
}

/* the 4 KB page at va (its words through the TLB now) as a code record, unless that version is
 * saved; kept in memory and written at the end (write_captures) */
typedef struct { uint32_t va, hash; uint8_t* data; unsigned gfx; } Rec;   /* gfx: graphics tasks before it */
static Rec* s_recs;
static unsigned s_nrecs, s_cap_recs;
static void capture(uint32_t va) {
    uint8_t* rec;
    uint32_t vp = va & ~0xFFFu, h = 0x811C9DC5u, k;
    unsigned i;
    rec = (uint8_t*)malloc(4096);
    if (!rec) return;
    for (k = 0; k < 4096; k += 4) {
        int64_t pa = translate_q(vp + k, 0, 1);
        uint32_t w;
        if (pa < 0) { free(rec); return; }
        w = (uint32_t)phys_read((uint32_t)pa, 4);
        wbe32(rec + k, w);
        h = (h ^ w) * 0x01000193u;
    }
    for (i = 0; i < s_npages; i++)
        if (s_pages[i].va == vp && s_pages[i].hash == h) { free(rec); return; }
    if (s_npages < MAX_PAGES) s_pages[s_npages].va = vp, s_pages[s_npages].hash = h, s_npages++;
    if (s_nrecs == s_cap_recs) {
        s_cap_recs = s_cap_recs ? s_cap_recs * 2 : 1024;
        s_recs = (Rec*)realloc(s_recs, s_cap_recs * sizeof *s_recs);
        if (!s_recs) exit(3);
    }
    s_recs[s_nrecs].va = (int32_t)vp >= 0 || vp >= 0xC0000000u ? vp : 0x80000000u | (vp & 0x1FFFFFFFu);
    s_recs[s_nrecs].hash = h, s_recs[s_nrecs].data = rec, s_recs[s_nrecs].gfx = s_gfx_tasks, s_nrecs++;
}

/* A jr through a register that a lw just loaded from a page the CPU made
 * (outside the boot image) reads a jump table there: the page goes out as
 * captured bytes too, for n64rx to read the table from (learn.py
 * _table_sources). Donkey Kong 64 decompresses its code and its rodata;
 * its switch tables at 0x8075xxxx were in no segment, so 120 functions,
 * its audio thread's task builder among them, were interpreted. */
static void table_page(uint32_t ea) {
    int64_t pa = translate_q(ea, 0, 1);
    if (pa < 0 || (uint32_t)pa >= s_ramsize || ((uint32_t)pa >= s_boot_lo && (uint32_t)pa < s_boot_hi)) return;
    if (!page_cpu((uint32_t)pa & ~0xFFFu)) return;
    capture(ea);
}

/* --want: pages the analysis asked for (n64rx's pack/want.txt, "lo hi" in hex), saved as the run ends:
 * a jump table of a function this run never entered, in rodata the boot decompressed. A jr that runs
 * saves its own table's page (table_page), one that never ran did not, and every such function was
 * interpreted: 90 of GoldenEye's 102 hazards from the ROM alone (the console's rounds get these pages
 * from the same list, xrc_learn_want). A page the TLB does not map now is skipped */
static void capture_wants(const char* path) {
    FILE* f = fopen(path, "r");
    unsigned long lo, hi;
    uint32_t p;
    if (!f) { fprintf(stderr, "dryboot: cannot read %s\n", path); return; }
    while (fscanf(f, "%lx %lx", &lo, &hi) == 2)
        for (p = (uint32_t)lo & ~0xFFFu; p < (uint32_t)hi; p += 0x1000) capture(p);
    fclose(f);
}

/* the records out: a page's versions all when an older one came back (an overlay the game loads at
 * that address again and again: Mario Tennis's scenes), else its last one alone when the older ran
 * at boot, before the first graphics task (a loader the code it unpacked replaced for good: one early
 * version made learn.py keep Banjo-Kazooie's whole block twice, as an overlay, whose libultra copy
 * is then never named; 36 names, 107 without). A version superseded once the game drew is kept
 * (Mario Tennis's menu scene, replaced by its match: 2,283 functions in 16 segments, 1,372 in 14
 * without). --versions last|all: the first rule alone, or every version */
static void write_captures(void) {
    unsigned i, j;
    for (i = 0; i < s_nrecs; i++) {
        const Rec* r = &s_recs[i];
        int later = 0, back = 0;
        for (j = i + 1; j < s_nrecs; j++)
            if (s_recs[j].va == r->va) {
                later = 1;
                if (s_recs[j].hash == r->hash) back = 1;
            }
        if (later && !back && s_versions != 1 && (s_versions == 0 || r->gfx == 0)) {   /* superseded: unless a version
                                    * before it comes back after it, or it ran once the game drew (2) */
            unsigned k;
            for (k = 0; k < i && !back; k++)
                if (s_recs[k].va == r->va)
                    for (j = i + 1; j < s_nrecs && !back; j++)
                        if (s_recs[j].va == r->va && s_recs[j].hash == s_recs[k].hash) back = 1;
            if (!back) continue;
        }
        {
            uint8_t head[8];
            wbe32(head, r->va), wbe32(head + 4, 4096);
            fwrite(head, 1, 8, s_bin);
            fwrite(r->data, 1, 4096, s_bin);
            fprintf(s_txt, "code %08x 1000\n", (unsigned)r->va);
            s_ncode++;
        }
    }
}

/* a TLB-mapped page code runs in, saved at its first run and again once its physical page has
 * been written (GoldenEye pages its 0x7F000000 code in from the ROM 8 KB at a time) */
#define MAX_VPAGE 8192
static struct { uint32_t vp, pa, gen; } s_vpage[MAX_VPAGE];
static void vpage_ran(uint32_t va, uint32_t pa) {
    uint32_t vp = va & ~0xFFFu, h = (vp >> 12) * 2654435761u, i, pg = pa >> 12;
    for (i = 0; i < 64; i++) {
        uint32_t k = (h + i) & (MAX_VPAGE - 1);
        if (s_vpage[k].vp == vp && s_vpage[k].pa == (pa & ~0xFFFu) && s_vpage[k].gen == s_pgen[pg]) return;
        if (!s_vpage[k].vp || s_vpage[k].vp == vp) {
            s_vpage[k].vp = vp, s_vpage[k].pa = pa & ~0xFFFu, s_vpage[k].gen = s_pgen[pg];
            capture(vp);
            return;
        }
    }
}

/* a function entered at va from caller: written down as the console's net would */
static void entered(uint32_t va, uint32_t caller) {
    int64_t pa;
    uint32_t p;
    unsigned i;
    if (va & 3 || seen(va) || known(va)) return;
    /* a call made between an exception and its eret is the game's exception code
     * calling its own parts (libultra's send_mesg, from __osException's interrupt
     * cases, which run after it clears EXL): the runtime replaces that code and the
     * console's net never sees it. Kirby 64 and Star Fox 64 learnt send_mesg as a
     * function and then never made a graphics task. --exl-calls 1: written down as before */
    if (s_in_exc && !s_exl_calls) return;
    pa = translate_q(va, 0, 1);
    if (pa < 0 || (uint32_t)pa >= s_ramsize) return;
    p = (uint32_t)pa;
    if ((int32_t)va >= 0 || va >= 0xC0000000u) {   /* through the TLB */
        uint32_t region = va & 0xFFF00000u;
        for (i = 0; i < s_nkuseg && s_kuseg_seen[i] != region; i++) {}
        if (i == s_nkuseg && s_nkuseg < 64) {
            s_kuseg_seen[s_nkuseg++] = region;
            fprintf(s_txt, "kuseg %08x 0\n", (unsigned)region);
        }
    }
    if (p >= 0x400000u && !s_rdram8 && s_rdram_mb == 8) {
        s_rdram8 = 1;
        fprintf(s_txt, "rdram 8\n");
    }
    if ((int32_t)va >= 0) {
        capture(va);   /* through the TLB: its page by its virtual address (learn.c capture_locked's KUSEG path) */
    } else if ((s_cpuw[p >> 7] & (1u << ((p >> 2) & 31))) && page_cpu(p)) {
        capture(va);   /* the CPU put its bytes there */
    } else {
        for (i = s_ndma; i-- > 0;)   /* newest first */
            if (p - s_dma[i].ram < s_dma[i].size) {
                fprintf(s_txt, "dma %08x %08x %x\n", (unsigned)s_dma[i].rom, (unsigned)(s_dma[i].ram | 0x80000000u),
                        (unsigned)s_dma[i].size);
                break;
            }
        if (i == (unsigned)-1 && !(p >= s_boot_lo && p < s_boot_hi) && page_cpu(p)) capture(va);   /* nothing loaded it */
    }
    fprintf(s_txt, "func %08x 4 from %08x\n", (unsigned)va, (unsigned)caller);
    s_nfunc++;
}

/* --------------------------------------------------------- the FPU */
static inline int fr(void) { return (C.cp0[STATUS] & SR_FR) != 0; }
static uint32_t fget32(int n) {
    if (fr()) return (uint32_t)C.f[n];
    return n & 1 ? (uint32_t)(C.f[n & ~1] >> 32) : (uint32_t)C.f[n];
}
static void fset32(int n, uint32_t v) {
    if (fr()) C.f[n] = (C.f[n] & 0xFFFFFFFF00000000ull) | v;
    else if (n & 1) C.f[n & ~1] = (C.f[n & ~1] & 0xFFFFFFFFull) | (uint64_t)v << 32;
    else C.f[n] = (C.f[n] & 0xFFFFFFFF00000000ull) | v;
}
static uint64_t fget64(int n) { return fr() ? C.f[n] : C.f[n & ~1]; }
static void fset64(int n, uint64_t v) {
    if (fr()) C.f[n] = v;
    else C.f[n & ~1] = v;
}
static float fs(int n) { uint32_t u = fget32(n); float f; memcpy(&f, &u, 4); return f; }
static double fd(int n) { uint64_t u = fget64(n); double d; memcpy(&d, &u, 8); return d; }
static void sfs(int n, float f) { uint32_t u; memcpy(&u, &f, 4); fset32(n, u); }
static void sfd(int n, double d) { uint64_t u; memcpy(&u, &d, 8); fset64(n, u); }
static double rnd(double v, int mode) {
    switch (mode) {
    case 0: return rint(v);   /* the host's default: to nearest, ties to even, as the VR4300's */
    case 1: return trunc(v);
    case 2: return ceil(v);
    default: return floor(v);
    }
}
static int32_t to_w(double v) { return v != v || v >= 2147483648.0 || v < -2147483648.0 ? 0x7FFFFFFF : (int32_t)v; }
static int64_t to_l(double v) {
    return v != v || v >= 9223372036854775808.0 || v < -9223372036854775808.0 ? 0x7FFFFFFFFFFFFFFFll : (int64_t)v;
}

static unsigned long long s_fp_fr1, s_fp_fr0, s_fp_odd0;   /* -v: FPU arithmetic with FR 1, FR 0, and FR 0 on an odd register */
static void cop1(uint32_t w) {
    int fmt = (w >> 21) & 31, ft = (w >> 16) & 31, fsr = (w >> 11) & 31, fdr = (w >> 6) & 31, fn = w & 63;
    if (fmt >= 16 && fmt <= 21) {
        if (fr()) s_fp_fr1++;
        else if (s_fp_fr0++, (ft | fsr | fdr) & 1) s_fp_odd0++;
    }
    switch (fmt) {
    case 0: C.r[ft] = SX32(fget32(fsr)); return;                         /* mfc1 */
    case 1: C.r[ft] = fget64(fsr); return;                               /* dmfc1 */
    case 2: C.r[ft] = SX32(fsr == 31 ? C.fcr31 : fsr == 0 ? 0xA00u : 0); return;   /* cfc1 */
    case 4: fset32(fsr, (uint32_t)C.r[ft]); return;                      /* mtc1 */
    case 5: fset64(fsr, C.r[ft]); return;                                /* dmtc1 */
    case 6: if (fsr == 31) C.fcr31 = (uint32_t)C.r[ft] & 0x0183FFFFu; return;   /* ctc1 */
    case 8: {                                                            /* bc1f, bc1t, bc1fl, bc1tl */
        int cond = (C.fcr31 >> 23) & 1, want = ft & 1, likely = ft & 2;
        uint32_t target = C.pc + 4 + ((uint32_t)(int32_t)(int16_t)w << 2);
        if (cond == want) C.taken = 1, C.target = target;
        else if (likely) C.skip = 1;   /* the slot skipped */
        return;
    }
    case 16: case 17: {                                                  /* .s, .d */
        int d = fmt == 17;
        double a = d ? fd(fsr) : fs(fsr), b = d ? fd(ft) : fs(ft), r = 0;
        int rm = C.fcr31 & 3;
        if (fn >= 48) {                                                  /* c.cond */
            int un = a != a || b != b, c = ((fn & 1) && un) || (!un && (((fn & 2) && a == b) || ((fn & 4) && a < b)));
            C.fcr31 = c ? C.fcr31 | 0x800000u : C.fcr31 & ~0x800000u;
            return;
        }
        switch (fn) {
        case 0: r = d ? a + b : (double)((float)a + (float)b); break;
        case 1: r = d ? a - b : (double)((float)a - (float)b); break;
        case 2: r = d ? a * b : (double)((float)a * (float)b); break;
        case 3: r = d ? a / b : (double)((float)a / (float)b); break;
        case 4: r = d ? sqrt(a) : (double)sqrtf((float)a); break;
        case 5: r = fabs(a); break;
        case 6: r = a; break;
        case 7: r = -a; break;
        case 8: case 9: case 10: case 11: fset64(fdr, (uint64_t)to_l(rnd(a, fn - 8 == 0 ? 0 : fn - 8))); return;
        case 12: case 13: case 14: case 15: fset32(fdr, (uint32_t)to_w(rnd(a, fn - 12 == 0 ? 0 : fn - 12))); return;
        case 32: sfs(fdr, (float)a); return;                             /* cvt.s */
        case 33: sfd(fdr, a); return;                                    /* cvt.d */
        case 36: fset32(fdr, (uint32_t)to_w(rnd(a, rm))); return;        /* cvt.w */
        case 37: fset64(fdr, (uint64_t)to_l(rnd(a, rm))); return;        /* cvt.l */
        default: s_unknown = 1; return;
        }
        if (fn == 6 && d) { fset64(fdr, fget64(fsr)); return; }          /* mov.d: the bits */
        if (fn == 6) { fset32(fdr, fget32(fsr)); return; }
        if (d) sfd(fdr, r);
        else sfs(fdr, (float)r);
        return;
    }
    case 20: case 21: {                                                  /* .w, .l: cvt.s, cvt.d */
        double a = fmt == 20 ? (double)(int32_t)fget32(fsr) : (double)(int64_t)fget64(fsr);
        if (fn == 32) sfs(fdr, fmt == 20 ? (float)(int32_t)fget32(fsr) : (float)(int64_t)fget64(fsr));
        else if (fn == 33) sfd(fdr, a);
        else s_unknown = 1;
        return;
    }
    }
    s_unknown = 1;
}

/* --------------------------------------------------------- the CPU's loop */
#define RS ((w >> 21) & 31)
#define RT ((w >> 16) & 31)
#define RD ((w >> 11) & 31)
#define SA ((w >> 6) & 31)
#define IMM ((uint32_t)(int32_t)(int16_t)w)
#define R(n) C.r[n]
#define EA ((uint32_t)R(RS) + IMM)

static void cop0(uint32_t w) {
    int rs = RS, rt = RT, rd = RD;
    if (rs == 0 || rs == 1) {                                            /* mfc0, dmfc0 */
        uint64_t v = C.cp0[rd];
        if (rd == RANDOM) {
            uint32_t wired = (uint32_t)C.cp0[WIRED] & 31;
            v = wired + (uint32_t)(s_cycles % (32 - wired));
        }
        R(rt) = rs == 0 ? SX32(v) : v;
        return;
    }
    if (rs == 4 || rs == 5) {                                            /* mtc0, dmtc0 */
        uint64_t v = rs == 4 ? SX32(R(rt)) : R(rt);
        if (rd == COMPARE) C.cp0[CAUSE] &= ~(uint64_t)0x8000;
        if (rd == CAUSE) { C.cp0[CAUSE] = (C.cp0[CAUSE] & ~(uint64_t)0x300) | (v & 0x300); return; }
        if (rd == COUNT) v &= 0xFFFFFFFFu;
        if (rd == STATUS && ((v ^ C.cp0[STATUS]) & SR_FR) && s_verbose) {   /* -v: who changes the float mode */
            static unsigned s_said;
            if (s_said++ < 24) fprintf(stderr, "FR %d at pc %08x (ra %08x, k0 %08x, frame %u)\n", (int)((v >> 26) & 1), C.pc, (uint32_t)C.r[31],
                                       (uint32_t)C.r[26], s_frames);
        }
        C.cp0[rd] = v;
        return;
    }
    if (rs == 16) {
        switch (w & 63) {
        case 1: {                                                        /* tlbr */
            const Tlb* t = &C.tlb[C.cp0[INDEX] & 31];
            uint32_t g = t->lo0 & t->lo1 & 1;
            C.cp0[PAGEMASK] = t->mask;
            C.cp0[ENTRYHI] = SX32(t->hi);
            C.cp0[ENTRYLO0] = (t->lo0 & ~1u) | g;
            C.cp0[ENTRYLO1] = (t->lo1 & ~1u) | g;
            return;
        }
        case 2: tlb_write((int)(C.cp0[INDEX] & 31)); return;            /* tlbwi */
        case 6: {                                                        /* tlbwr */
            uint32_t wired = (uint32_t)C.cp0[WIRED] & 31;
            tlb_write((int)(wired + (uint32_t)(s_cycles % (32 - wired))));
            return;
        }
        case 8: {                                                        /* tlbp */
            int i;
            uint32_t hi = (uint32_t)C.cp0[ENTRYHI];
            C.cp0[INDEX] = 0x80000000u;
            for (i = 0; i < 32; i++) {
                const Tlb* t = &C.tlb[i];
                uint32_t m = t->mask | 0x1FFFu;
                if ((hi & ~m) == (t->hi & ~m) && (((t->lo0 & t->lo1) & 1) || (t->hi & 0xFF) == (hi & 0xFF))) {
                    C.cp0[INDEX] = (uint64_t)i;
                    break;
                }
            }
            return;
        }
        case 24: {                                                       /* eret */
            uint32_t to;
            if (C.cp0[STATUS] & SR_ERL) to = (uint32_t)C.cp0[ERROREPC], C.cp0[STATUS] &= ~(uint64_t)SR_ERL;
            else to = (uint32_t)C.cp0[EPC], C.cp0[STATUS] &= ~(uint64_t)SR_EXL;
            C.llbit = 0;
            s_in_exc = 0;   /* (before the entry below: a thread's first dispatch is learnt) */
            {   /* a thread's first dispatch enters its entry; a resume point ran before (it is no function's start) */
                int64_t pa = translate_q(to, 0, 1);
                /* nor a call's return point (a thread that yielded in osRecvMesg: __osEnqueueAndYield
                 * saved its ra as the pc), two words after a jal or jalr */
                uint32_t call = pa >= 8 && (uint32_t)pa < s_ramsize ? be32(&s_ram[(uint32_t)pa - 8]) : 0;
                int after_call = (call >> 26) == 3 || ((call >> 26) == 0 && (call & 63) == 9);
                if (pa >= 0 && (uint32_t)pa < s_ramsize && !(s_ran[(uint32_t)pa >> 7] & (1u << (((uint32_t)pa >> 2) & 31))) &&
                    !epc_has(to) && !after_call)
                    entered(to, C.pc);
            }
            C.eret = 1, C.eret_to = to;   /* no delay slot */
            return;
        }
        }
        return;
    }
    s_unknown = 1;
}

#define JUMP(t) (C.taken = 1, C.target = (t))
/* one instruction at C.pc */
static void step(uint32_t w) {
    uint32_t op = w >> 26;
    uint64_t v;
    /* the FPU with CU1 clear: coprocessor unusable (CE 1), before it runs. libultra's handler marks the
     * thread as one that uses the FPU and only then saves its FP registers at a switch: without the
     * trap, another thread clobbers them (Perfect Dark's inflater keeps integers in $f0/$f1) */
    if ((op == 17 || op == 49 || op == 53 || op == 57 || op == 61) && !(C.cp0[STATUS] & SR_CU1)) {
        C.cp0[CAUSE] = (C.cp0[CAUSE] & ~(uint64_t)0x30000000u) | 0x10000000u;
        C.exc = EXC_CPU + 1;
        return;
    }
    switch (op) {
    case 0: {
        uint32_t fn = w & 63;
        switch (fn) {
        case 0: R(RD) = SX32((uint32_t)R(RT) << SA); break;
        case 2: R(RD) = SX32((uint32_t)R(RT) >> SA); break;
        case 3: R(RD) = SX32((int32_t)(uint32_t)R(RT) >> SA); break;
        case 4: R(RD) = SX32((uint32_t)R(RT) << (R(RS) & 31)); break;
        case 6: R(RD) = SX32((uint32_t)R(RT) >> (R(RS) & 31)); break;
        case 7: R(RD) = SX32((int32_t)(uint32_t)R(RT) >> (R(RS) & 31)); break;
        case 8:                                                                          /* jr */
            if (s_tables && RS != 31 && s_lw_ea[RS] && (uint32_t)R(RS) == s_lw_val[RS]) table_page(s_lw_ea[RS]);
            JUMP((uint32_t)R(RS));
            break;
        case 9: { uint32_t t = (uint32_t)R(RS); R(RD) = SX32(C.pc + 8); JUMP(t); entered(t, C.pc); break; }   /* jalr */
        case 12: C.exc = EXC_SYS + 1; break;
        case 13: C.exc = EXC_BP + 1; break;
        case 15: break;                                                                  /* sync */
        case 16: R(RD) = C.hi; break;
        case 17: C.hi = R(RS); break;
        case 18: R(RD) = C.lo; break;
        case 19: C.lo = R(RS); break;
        case 20: R(RD) = R(RT) << (R(RS) & 63); break;
        case 22: R(RD) = R(RT) >> (R(RS) & 63); break;
        case 23: R(RD) = (uint64_t)((int64_t)R(RT) >> (R(RS) & 63)); break;
        case 24: { int64_t p = (int64_t)(int32_t)R(RS) * (int32_t)R(RT); C.lo = SX32(p); C.hi = SX32(p >> 32); break; }
        case 25: { uint64_t p = (uint64_t)(uint32_t)R(RS) * (uint32_t)R(RT); C.lo = SX32(p); C.hi = SX32(p >> 32); break; }
        case 26: {
            int32_t a = (int32_t)R(RS), b = (int32_t)R(RT);
            if (!b) C.lo = a < 0 ? 1 : (uint64_t)-1, C.hi = SX32(a);
            else if (a == INT32_MIN && b == -1) C.lo = SX32(a), C.hi = 0;
            else C.lo = SX32(a / b), C.hi = SX32(a % b);
            break;
        }
        case 27: {
            uint32_t a = (uint32_t)R(RS), b = (uint32_t)R(RT);
            if (!b) C.lo = (uint64_t)-1, C.hi = SX32(a);
            else C.lo = SX32(a / b), C.hi = SX32(a % b);
            break;
        }
        case 28: { __int128 p = (__int128)(int64_t)R(RS) * (int64_t)R(RT); C.lo = (uint64_t)p; C.hi = (uint64_t)(p >> 64); break; }
        case 29: { unsigned __int128 p = (unsigned __int128)R(RS) * R(RT); C.lo = (uint64_t)p; C.hi = (uint64_t)(p >> 64); break; }
        case 30: {
            int64_t a = (int64_t)R(RS), b = (int64_t)R(RT);
            if (!b) C.lo = a < 0 ? 1 : (uint64_t)-1, C.hi = (uint64_t)a;
            else if (a == INT64_MIN && b == -1) C.lo = (uint64_t)a, C.hi = 0;
            else C.lo = (uint64_t)(a / b), C.hi = (uint64_t)(a % b);
            break;
        }
        case 31: {
            uint64_t a = R(RS), b = R(RT);
            if (!b) C.lo = (uint64_t)-1, C.hi = a;
            else C.lo = a / b, C.hi = a % b;
            break;
        }
        case 32: case 33: R(RD) = SX32((uint32_t)R(RS) + (uint32_t)R(RT)); break;   /* add (no overflow trap), addu */
        case 34: case 35: R(RD) = SX32((uint32_t)R(RS) - (uint32_t)R(RT)); break;
        case 36: R(RD) = R(RS) & R(RT); break;
        case 37: R(RD) = R(RS) | R(RT); break;
        case 38: R(RD) = R(RS) ^ R(RT); break;
        case 39: R(RD) = ~(R(RS) | R(RT)); break;
        case 42: R(RD) = (int64_t)R(RS) < (int64_t)R(RT); break;
        case 43: R(RD) = R(RS) < R(RT); break;
        case 44: case 45: R(RD) = R(RS) + R(RT); break;
        case 46: case 47: R(RD) = R(RS) - R(RT); break;
        case 48: if ((int64_t)R(RS) >= (int64_t)R(RT)) C.exc = EXC_TR + 1; break;
        case 49: if (R(RS) >= R(RT)) C.exc = EXC_TR + 1; break;
        case 50: if ((int64_t)R(RS) < (int64_t)R(RT)) C.exc = EXC_TR + 1; break;
        case 51: if (R(RS) < R(RT)) C.exc = EXC_TR + 1; break;
        case 52: if (R(RS) == R(RT)) C.exc = EXC_TR + 1; break;
        case 54: if (R(RS) != R(RT)) C.exc = EXC_TR + 1; break;
        case 56: R(RD) = R(RT) << SA; break;
        case 58: R(RD) = R(RT) >> SA; break;
        case 59: R(RD) = (uint64_t)((int64_t)R(RT) >> SA); break;
        case 60: R(RD) = R(RT) << (SA + 32); break;
        case 62: R(RD) = R(RT) >> (SA + 32); break;
        case 63: R(RD) = (uint64_t)((int64_t)R(RT) >> (SA + 32)); break;
        default: s_unknown = 1; break;
        }
        break;
    }
    case 1: {   /* REGIMM */
        uint32_t rt = RT, target = C.pc + 4 + (IMM << 2);
        int64_t s = (int64_t)R(RS);
        int take = (rt & 1) ? s >= 0 : s < 0;
        if (rt >= 8 && rt <= 14) {   /* the traps */
            int64_t i = (int64_t)(int16_t)w;
            uint64_t u = (uint64_t)i;
            int t = rt == 8 ? s >= i : rt == 9 ? R(RS) >= u : rt == 10 ? s < i : rt == 11 ? R(RS) < u : rt == 12 ? s == i : s != i;
            if (t) C.exc = EXC_TR + 1;
            break;
        }
        if (rt & 0x10) R(31) = SX32(C.pc + 8);   /* bltzal, bgezal (and the likely ones) */
        if (take) {
            JUMP(target);
            if (rt & 0x10) entered(target, C.pc);
        } else if (rt & 2) {
            C.skip = 1;
        }
        break;
    }
    case 2: JUMP((C.pc & 0xF0000000u) | ((w & 0x3FFFFFFu) << 2)); break;
    case 3: {
        uint32_t t = (C.pc & 0xF0000000u) | ((w & 0x3FFFFFFu) << 2);
        R(31) = SX32(C.pc + 8);
        JUMP(t);
        entered(t, C.pc);
        break;
    }
    case 4: case 5: case 6: case 7: case 20: case 21: case 22: case 23: {
        int take, o = op & 3;
        if (o == 0) take = R(RS) == R(RT);
        else if (o == 1) take = R(RS) != R(RT);
        else if (o == 2) take = (int64_t)R(RS) <= 0;
        else take = (int64_t)R(RS) > 0;
        if (take) JUMP(C.pc + 4 + (IMM << 2));
        else if (op >= 20) C.skip = 1;   /* likely: the slot skipped */
        break;
    }
    case 8: case 9: R(RT) = SX32((uint32_t)R(RS) + IMM); break;
    case 10: R(RT) = (int64_t)R(RS) < (int64_t)(int32_t)IMM; break;
    case 11: R(RT) = R(RS) < (uint64_t)(int64_t)(int32_t)IMM; break;
    case 12: R(RT) = R(RS) & (w & 0xFFFF); break;
    case 13: R(RT) = R(RS) | (w & 0xFFFF); break;
    case 14: R(RT) = R(RS) ^ (w & 0xFFFF); break;
    case 15: R(RT) = SX32((w & 0xFFFF) << 16); break;
    case 16: cop0(w); break;
    case 17: cop1(w); break;
    case 24: case 25: R(RT) = R(RS) + (uint64_t)(int64_t)(int32_t)IMM; break;   /* daddi, daddiu */
    case 26: case 27: {   /* ldl, ldr */
        uint32_t a = EA, sh = (a & 7) * 8;
        if (!rd(a & ~7u, 8, &v)) break;
        if (op == 26) R(RT) = sh ? (R(RT) & ((1ull << sh) - 1)) | (v << sh) : v;
        else {
            uint32_t s2 = (7 - (a & 7)) * 8;
            R(RT) = s2 ? (R(RT) & ~(~0ull >> s2)) | (v >> s2) : v;
        }
        break;
    }
    case 32: if (rd(EA, 1, &v)) R(RT) = (uint64_t)(int64_t)(int8_t)v; break;
    case 33: if (rd(EA, 2, &v)) R(RT) = (uint64_t)(int64_t)(int16_t)v; break;
    case 34: case 38: {   /* lwl, lwr */
        uint32_t a = EA, wv, old = (uint32_t)R(RT), nv;
        if (!rd(a & ~3u, 4, &v)) break;
        wv = (uint32_t)v;
        if (op == 34) { uint32_t sh = (a & 3) * 8; nv = sh ? (old & ((1u << sh) - 1)) | (wv << sh) : wv; }
        else { uint32_t sh = (3 - (a & 3)) * 8; nv = sh ? (old & ~(0xFFFFFFFFu >> sh)) | (wv >> sh) : wv; }
        R(RT) = SX32(nv);
        break;
    }
    case 35:
        if (rd(EA, 4, &v)) {
            R(RT) = SX32(v);
            if (RT) s_lw_ea[RT] = EA, s_lw_val[RT] = v;
        }
        break;
    case 36: if (rd(EA, 1, &v)) R(RT) = v & 0xFF; break;
    case 37: if (rd(EA, 2, &v)) R(RT) = v & 0xFFFF; break;
    case 39: if (rd(EA, 4, &v)) R(RT) = v & 0xFFFFFFFFu; break;
    case 40: wr(EA, R(RT) & 0xFF, 1); break;
    case 41: wr(EA, R(RT) & 0xFFFF, 2); break;
    case 42: case 46: {   /* swl, swr */
        uint32_t a = EA, wv, r = (uint32_t)R(RT), nv;
        if (!rd(a & ~3u, 4, &v)) break;
        wv = (uint32_t)v;
        if (op == 42) { uint32_t sh = (a & 3) * 8; nv = (wv & ~(0xFFFFFFFFu >> sh)) | (r >> sh); }
        else { uint32_t sh = (3 - (a & 3)) * 8; nv = (wv & ~(0xFFFFFFFFu << sh)) | (r << sh); }
        wr(a & ~3u, nv, 4);
        break;
    }
    case 43: wr(EA, R(RT) & 0xFFFFFFFFu, 4); break;
    case 44: case 45: {   /* sdl, sdr */
        uint32_t a = EA;
        uint64_t r = R(RT), nv;
        if (!rd(a & ~7u, 8, &v)) break;
        if (op == 44) { uint32_t sh = (a & 7) * 8; nv = sh ? (v & ~(~0ull >> sh)) | (r >> sh) : r; }
        else { uint32_t sh = (7 - (a & 7)) * 8; nv = sh ? (v & ~(~0ull << sh)) | (r << sh) : r; }
        wr(a & ~7u, nv, 8);
        break;
    }
    case 47: break;   /* cache */
    case 48: if (rd(EA, 4, &v)) R(RT) = SX32(v), C.llbit = 1; break;   /* ll */
    case 49: if (rd(EA, 4, &v)) fset32(RT, (uint32_t)v); break;         /* lwc1 */
    case 52: if (rd(EA, 8, &v)) R(RT) = v, C.llbit = 1; break;           /* lld */
    case 53: if (rd(EA, 8, &v)) fset64(RT, v); break;                   /* ldc1 */
    case 55: if (rd(EA, 8, &v)) R(RT) = v; break;                       /* ld */
    case 56: if (wr(EA, R(RT) & 0xFFFFFFFFu, 4)) R(RT) = 1; break;      /* sc: one CPU */
    case 57: wr(EA, fget32(RT), 4); break;                              /* swc1 */
    case 60: if (wr(EA, R(RT), 8)) R(RT) = 1; break;                    /* scd */
    case 61: wr(EA, fget64(RT), 8); break;                              /* sdc1 */
    case 63: wr(EA, R(RT), 8); break;                                   /* sd */
    default: s_unknown = 1; break;
    }
    R(0) = 0;
}

/* -------------------------------------------------------------- time */
static void events(void) {
    if (s_cycles >= s_next_vi) {
        s_next_vi += s_frame;
        s_frames++;
        if (s_vi_reg[6] || s_vi_reg[3]) raise_mi(MI_VI);   /* (a VI the game set up) */
        if (s_ai_n) raise_mi(MI_AI);                        /* the audio thread's beat, too */
    }
    if (s_sp_done_at && s_cycles >= s_sp_done_at) s_sp_done_at = 0, raise_mi(MI_SP);
    if (s_dp_done_at && s_cycles >= s_dp_done_at) s_dp_done_at = 0, raise_mi(MI_DP);
    if (s_ai_n && s_cycles >= s_ai_end) {
        s_ai_len[0] = s_ai_len[1];
        s_ai_n--;
        if (s_ai_n) s_ai_end = s_cycles + s_frame / 2;
        raise_mi(MI_AI);
    }
    s_next_event = s_next_vi;
    if (s_sp_done_at && s_sp_done_at < s_next_event) s_next_event = s_sp_done_at;
    if (s_dp_done_at && s_dp_done_at < s_next_event) s_next_event = s_dp_done_at;
    if (s_ai_n && s_ai_end < s_next_event) s_next_event = s_ai_end;
}

static int fetch(uint32_t va, uint32_t* w) {
    uint64_t v;
    if ((va & 0xC0000000u) == 0x80000000u) {
        uint32_t pa = va & 0x1FFFFFFFu;
        if (pa + 4 <= s_ramsize) { *w = be32(&s_ram[pa]); return 1; }
        if (pa >= 0x04000000u && pa < 0x04002000u) { *w = be32(&s_sp[pa & 0x1FFC]); return 1; }
    }
    if (!rd(va, 4, &v)) return 0;
    *w = (uint32_t)v;
    return 1;
}

static void run(uint64_t frames) {
    s_next_vi = s_frame;
    s_next_event = s_next_vi;
    while (s_frames < frames && !s_unknown) {
        uint32_t w, cur;
        int in_delay;
        if (s_cycles >= s_next_event) events();
        /* the interrupt: IE on, EXL and ERL off, a line pending and unmasked (not in a delay slot) */
        if (!C.delay) {
            uint64_t sr = C.cp0[STATUS], cause = C.cp0[CAUSE];
            if ((sr & 7) == SR_IE && (cause & sr & 0xFF00)) {
                exception(EXC_INT, 0);
                continue;
            }
        }
        cur = C.pc;
        in_delay = C.delay;
        if (s_verbose && !(++s_exec & 0xFF)) sample(cur);
        if (!fetch(cur, &w)) {
            int code = C.exc - 1, refill = (C.cp0[CAUSE] & 0x100000000ull) != 0;
            C.cp0[CAUSE] &= 0xFFFFFFFFull;
            C.exc = 0;
            exception(code, refill);
            continue;
        }
        {   /* code running in a page the CPU wrote since it was saved: saved now, as it is */
            uint32_t pa = (cur & 0xC0000000u) == 0x80000000u ? cur & 0x1FFFFFFFu : 0xFFFFFFFFu;
            if (pa == 0xFFFFFFFFu) {
                static uint32_t s_lva = 1, s_lpa;
                if ((cur & ~0xFFFu) != s_lva) {
                    int64_t t = translate_q(cur & ~0xFFFu, 0, 1);
                    s_lva = cur & ~0xFFFu, s_lpa = t < 0 ? 0xFFFFFFFFu : (uint32_t)t;
                }
                pa = s_lpa == 0xFFFFFFFFu ? s_lpa : s_lpa | (cur & 0xFFFu);
            }
            if (pa < s_ramsize) s_ran[pa >> 7] |= 1u << ((pa >> 2) & 31);
            if ((int32_t)cur >= 0 && pa < s_ramsize) vpage_ran(cur, pa);
            if (pa < s_ramsize && (!s_pcap[pa >> 12] || s_pdirty[pa >> 12]) && (s_cpuw[pa >> 7] & (1u << ((pa >> 2) & 31))) &&
                page_cpu(pa)) {
                s_pdirty[pa >> 12] = 0, s_pcap[pa >> 12] = 1;
                if ((cur & 0xC0000000u) == 0x80000000u) s_pexec[pa >> 12] = 1;
                capture(cur);
            }
        }
        C.taken = C.skip = C.eret = 0;
        step(w);
        if (s_unknown) {
            s_bad_w = w, s_bad_pc = cur;
            break;
        }
        if (C.exc) {
            int code = C.exc - 1, refill = (C.cp0[CAUSE] & 0x100000000ull) != 0;
            C.cp0[CAUSE] &= 0xFFFFFFFFull;
            C.exc = 0;
            exception(code, refill);   /* (C.pc and C.delay still this instruction's: its EPC) */
        } else if (C.eret) {
            C.pc = C.eret_to, C.delay = 0;
        } else if (in_delay) {   /* a slot done: to its branch's target */
            C.pc = C.target, C.delay = 0;
        } else if (C.taken) {    /* its slot next */
            C.pc = cur + 4, C.delay = 1;
        } else {
            C.pc = cur + (C.skip ? 8 : 4);
        }
        /* the idle loop (a branch to itself over a nop): on to the next event */
        if (C.taken && C.target == cur && !in_delay) {
            uint32_t slot;
            if (fetch(cur + 4, &slot) && slot == 0 && s_next_event > s_cycles + 2) {
                uint64_t skip = s_next_event - s_cycles - 2;
                uint32_t cnt = (uint32_t)C.cp0[COUNT], cmp = (uint32_t)C.cp0[COMPARE], left = cmp - cnt;
                if (left > 1 && left - 1 < skip) skip = left - 1;
                s_cycles += skip;
                C.cp0[COUNT] = (uint32_t)(cnt + (uint32_t)skip);
            }
        }
        s_cycles++;
        {
            uint32_t cnt = (uint32_t)C.cp0[COUNT] + 1;
            C.cp0[COUNT] = cnt;
            if (cnt == (uint32_t)C.cp0[COMPARE]) C.cp0[CAUSE] |= 0x8000;
        }
    }
}

/* ------------------------------------------------------------- the boot */
/* boot.c's ipl3_left: the IPL3's last part, copied from DMEM to RDRAM 0 */
static void ipl3_left(uint32_t entry) {
    uint32_t i, j, x, y, d;
    for (i = 0x40; i < 0x1000 && i + 4 <= s_romsize; i += 4) {
        if (be32(s_rom + i) != 0x3C09A000u) continue;
        x = y = 0;
        for (j = i >= 0x68 ? i - 0x28 : 0x40; j < i; j += 4) {
            uint32_t w = be32(s_rom + j);
            if (w >> 16 == 0x2508u) x = w & 0xFFFFu;
            if (w >> 16 == 0x256Bu) y = w & 0xFFFFu;
        }
        d = 0;
        for (j = i + 4; j < i + 0x30 && j < 0x1000 && be32(s_rom + j) != 0x8D0D0000u; j += 4)
            if (be32(s_rom + j) >> 16 == 0x2529u) d += (uint32_t)(int32_t)(int16_t)(be32(s_rom + j) & 0xFFFFu);
        if (x < 0x40 || y <= x || y > 0x1000 || d > 0x100u || y - x + d > 0x400u || y - x + d > (entry & 0x1FFFFFFFu))
            continue;
        memcpy(s_ram + d, s_rom + x, y - x);
        for (j = i + 4; j < i + 0x40 && j < 0x1000; j += 4)
            if (be32(s_rom + j) == 0x01AC6826u) {   /* the 6106's is scrambled (boot.c) */
                uint32_t k = 0x3C421AAAu, n;
                for (n = 0; n < y - x; n += 4) {
                    wbe32(s_ram + d + n, be32(s_rom + x + n) ^ k);
                    k *= 0x0260BCD5u;
                }
                break;
            }
        say("[BOOT] IPL3's last part in RDRAM %03x..%03x", d, d + y - x);
        return;
    }
}

static uint32_t* load_known(const char* path, size_t* n) {
    FILE* f = fopen(path, "r");
    uint32_t* v = NULL;
    size_t cap = 0;
    char line[64];
    *n = 0;
    if (!f) return NULL;
    while (fgets(line, sizeof line, f)) {
        char* end;
        unsigned long x = strtoul(line, &end, 16);
        if (end == line) continue;
        if (*n == cap) {
            cap = cap ? cap * 2 : 4096;
            v = (uint32_t*)realloc(v, cap * sizeof *v);
            if (!v) exit(3);
        }
        v[(*n)++] = (uint32_t)x;
    }
    fclose(f);
    {   /* sorted, for the binary search */
        size_t i, j;
        for (i = 1; i < *n; i++) {
            uint32_t t = v[i];
            for (j = i; j > 0 && v[j - 1] > t; j--) v[j] = v[j - 1];
            v[j] = t;
        }
    }
    return v;
}

static int usage(void) {
    fprintf(stderr, "usage: dryboot <rom.z64> <out dir> --entry X --boot-size N [--cic N] [--tv 0|1|2] [--rdram 4|8]\n"
                    "               [--known file] [--frames N] [--press frame|0] [--script autopad.txt] [--eep 4|16]\n"
                    "               [--dump addr bytes file] [--snap gfx-task file] [--watch addr bytes]\n"
                    "               [--versions boot|all|last] [--window keep] [--want file] [-v]\n");
    return 1;
}

int main(int argc, char** argv) {
    const char *rom_path, *out, *known_path = NULL, *dump_path = NULL, *want_path = NULL;
    uint32_t dump_at = 0, dump_n = 0;
    uint32_t entry = 0, boot_size = 0x100000, cic = 6102, tv = 1, mb = 4, frames = 600, i;
    int keep8 = 0;
    char path[1024];
    FILE* f;
    long n;
    if (argc < 3) return usage();
    rom_path = argv[1], out = argv[2];
    for (i = 3; i < (uint32_t)argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < (uint32_t)argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "-v")) { s_verbose = 1; continue; }
        if (!v) return usage();
        if (!strcmp(a, "--entry")) entry = (uint32_t)strtoul(v, NULL, 16);
        else if (!strcmp(a, "--boot-size")) boot_size = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--cic")) cic = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--tv")) tv = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--rdram")) mb = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--known")) known_path = v;
        else if (!strcmp(a, "--frames")) frames = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--press")) s_press_from = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--press-mode")) s_press_mode = (int)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--script")) script_load(v);
        else if (!strcmp(a, "--eep")) s_eep16 = strtoul(v, NULL, 10) == 16;
        else if (!strcmp(a, "--exl-calls")) s_exl_calls = (int)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--jump-tables")) s_tables = (int)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--want")) want_path = v;        else if (!strcmp(a, "--window")) keep8 = !strcmp(v, "keep");
        else if (!strcmp(a, "--versions")) s_versions = !strcmp(v, "all") ? 1 : !strcmp(v, "last") ? 0 : 2;
        else if (!strcmp(a, "--watch") && i + 2 < (uint32_t)argc) {   /* --watch <addr> <bytes>: who stores there */
            s_watch_lo = (uint32_t)strtoul(v, NULL, 16) & 0x1FFFFFFFu;
            s_watch_hi = s_watch_lo + (uint32_t)strtoul(argv[i + 2], NULL, 0);
            i += 1;
        }
        else if (!strcmp(a, "--snap") && i + 2 < (uint32_t)argc) {   /* --snap <n> <file>: RDRAM as graphics task n starts */
            s_snap_at = (unsigned)strtoul(v, NULL, 0), s_snap_path = argv[i + 2];
            i += 1;
        }
        else if (!strcmp(a, "--dump") && i + 3 < (uint32_t)argc) {   /* --dump <addr> <bytes> <file>: RDRAM after the run */
            dump_at = (uint32_t)strtoul(v, NULL, 16) & 0x1FFFFFFFu;
            dump_n = (uint32_t)strtoul(argv[i + 2], NULL, 0);
            dump_path = argv[i + 3];
            i += 2;
        }
        else return usage();
        i++;
    }
    if (!entry || (mb != 4 && mb != 8)) return usage();
    s_rdram_mb = mb;
    if (!(f = fopen(rom_path, "rb"))) { fprintf(stderr, "dryboot: cannot read %s\n", rom_path); return 1; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0x101000 || n > 0xFC00000) {   /* (the cartridge window: a ROM hack can pass 64 MB) */ fprintf(stderr, "dryboot: %s is not a ROM\n", rom_path); fclose(f); return 1; }
    s_romsize = (uint32_t)n;
    s_rom = (uint8_t*)malloc(s_romsize);
    if (!s_rom || fread(s_rom, 1, s_romsize, f) != s_romsize) { fclose(f); return 1; }
    fclose(f);
    /* 8 MB kept whatever osMemSize says: a 4 MB game's stack top writes past 4 MB (All-Star Baseball
     * 2000's entry: sp 0x803FFFF0, the 5th argument at 0x14(sp)) and reads it back, as the console
     * runtime's window lets it (an access past the end went nowhere: the TLB mapped nothing) */
    s_ramsize = 8u << 20;
    s_k1ram = mb == 8 || keep8 ? s_ramsize : 4u << 20;
    s_ram = (uint8_t*)calloc(1, s_ramsize);
    s_cpuw = (uint32_t*)calloc(s_ramsize / 128, sizeof *s_cpuw);
    s_ran = (uint32_t*)calloc(s_ramsize / 128, sizeof *s_ran);
    if (!s_ram || !s_cpuw || !s_ran) return 3;
    if (known_path) s_known = load_known(known_path, &s_nknown);
    memset(s_eep, 0xFF, sizeof s_eep);
    if (tv == 0) s_frame = 937500;

    /* what the IPL3 and the boot ROM leave (boot.c) */
    ipl3_left(entry);
    wbe32(s_ram + 0x300, tv);
    wbe32(s_ram + 0x304, 0);
    wbe32(s_ram + 0x308, 0xB0000000u);
    wbe32(s_ram + 0x30C, 0);
    wbe32(s_ram + 0x310, cic == 6106 ? 0x17D8u : cic);   /* (boot.c: what the 6106's tail leaves) */
    wbe32(s_ram + 0x314, 0);
    wbe32(s_ram + 0x318, mb << 20);   /* osMemSize: the build's */
    if (boot_size > s_romsize - 0x1000) boot_size = s_romsize - 0x1000;
    s_boot_lo = entry & 0x1FFFFFFFu;
    s_boot_hi = s_boot_lo + boot_size;
    if (s_boot_hi > s_ramsize) s_boot_hi = s_ramsize, boot_size = s_boot_hi - s_boot_lo;
    memcpy(s_ram + s_boot_lo, s_rom + 0x1000, boot_size);
    if (cic == 6105 && 0x2FE1C4u <= s_ramsize) {   /* as the runtime's boot.c: the IPL3's two words */
        memcpy(s_ram + 0x2FE1C0u, s_rom + 0x750, 4);
        memcpy(s_ram + 0x2FB1F4u, s_rom + 0x73C, 4);
    }
    memcpy(s_sp, s_rom, 0x1000);   /* DMEM held the ROM's first 4 KB */
    if (cic == 6106) wbe32(s_sp, 0xFFFFFFFFu), wbe32(s_sp + 0x1000, 0x17D8u);   /* as the runtime's mmio.c, boot.c */

    memset(&C, 0, sizeof C);
    C.pc = entry;
    C.r[29] = SX32(0xA4001FF0u);
    /* an entry that returns (the test ROM's) idles in IMEM: `b .; nop` (a game never returns) */
    wbe32(s_sp + 0x1000, 0x1000FFFFu), wbe32(s_sp + 0x1004, 0);
    C.r[31] = SX32(0xA4001000u);
    C.r[19] = 0, C.r[20] = tv, C.r[21] = 0, C.r[23] = 0;
    C.cp0[STATUS] = 0x34000000u;   /* CU1, CU0, FR */
    C.cp0[CONFIG] = 0x0006E463u;
    C.cp0[PRID] = 0x0B22u;
    C.cp0[RANDOM] = 31;
    for (i = 0; i < 32; i++) C.tlb[i].hi = 0x80000000u + i * 0x2000u;   /* none of them maps KUSEG */

    snprintf(path, sizeof path, "%s/learn.txt", out);
    s_txt = fopen(path, "w");
    snprintf(path, sizeof path, "%s/learn_code.bin", out);
    s_bin = fopen(path, "wb");
    if (!s_txt || !s_bin) { fprintf(stderr, "dryboot: cannot write into %s\n", out); return 1; }
    fprintf(s_txt, "# n64rx dry boot (tools/dryboot): code this build would not have recompiled, as the console's learn.txt\n");
    run(frames);
    {   /* the CPU-written block around code that ran (a decompressed segment is one): each of its pages */
        uint32_t np = s_ramsize >> 12, pg, k;
        static uint8_t s_take[2048];
        for (pg = 0; pg < np; pg++) {
            uint32_t lo, hi;
            if (!s_pexec[pg]) continue;
            for (lo = pg; lo > 0 && page_cpu((lo - 1) << 12) && page_code((lo - 1) << 12); lo--) {}
            for (hi = pg + 1; hi < np && page_cpu(hi << 12) && page_code(hi << 12); hi++) {}
            for (k = lo; k < hi; k++) s_take[k] = 1;
        }
        if (s_verbose)
            for (pg = 0; pg < np; pg++) {
                unsigned bits = 0;
                for (k = 0; k < 32; k++) bits += (unsigned)__builtin_popcount(s_cpuw[pg * 32 + k]);
                if (bits || s_pexec[pg]) fprintf(stderr, "page %08x: %4u cpu words%s%s\n", 0x80000000u | pg << 12, bits,
                                                 s_pexec[pg] ? ", ran" : "", s_take[pg] ? ", taken" : "");
            }
        for (pg = 0; pg < np; pg++)
            if (s_take[pg] && !s_pcap[pg]) capture(0x80000000u | pg << 12);   /* (in the boot copy too: rewritten, it is not the ROM's) */
    }
    if (want_path) capture_wants(want_path);
    write_captures();
    fprintf(s_txt, "# %u frames, %llu instructions, %u RSP tasks (%u graphics), %u PI DMAs\n", s_frames,
            (unsigned long long)s_cycles, s_tasks, s_gfx_tasks, s_ndma);
    fclose(s_txt);
    fclose(s_bin);
    if (dump_path && dump_at < s_ramsize) {
        FILE* d = fopen(dump_path, "wb");
        if (d) {
            fwrite(s_ram + dump_at, 1, dump_n < s_ramsize - dump_at ? dump_n : s_ramsize - dump_at, d);
            fclose(d);
        }
    }
    if (s_verbose)
        fprintf(stderr, "fpu: %llu operations with FR 1, %llu with FR 0 (%llu on an odd register)\n", s_fp_fr1, s_fp_fr0, s_fp_odd0);
    fprintf(stderr, "dryboot: %u frames, %llu instructions, %u tasks (%u graphics), %u DMAs; %u functions, %u code pages learnt\n",
            s_frames, (unsigned long long)s_cycles, s_tasks, s_gfx_tasks, s_ndma, s_nfunc, s_ncode);
    if (s_verbose) {
        fprintf(stderr, "end: pc %08x sr %08llx cause %08llx mi intr %02x mask %02x, vi status %08x origin %08x intr %x vsync %x\n",
                C.pc, (unsigned long long)C.cp0[STATUS], (unsigned long long)C.cp0[CAUSE], s_mi_intr, s_mi_mask,
                s_vi_reg[0], s_vi_reg[1], s_vi_reg[3], s_vi_reg[6]);
        for (i = 0; i < 32; i++)
            if (s_excs[i]) fprintf(stderr, "  exception %u: %u\n", i, s_excs[i]);
        fprintf(stderr, "  %llu instructions run (the rest idle)\n", (unsigned long long)s_exec);
        fprintf(stderr, "  pif runs %u, pad reads %u (%u with buttons)\n", s_pif_runs, s_pad_reads, s_pad_pressed);
        for (i = 0; i < 256; i++)
            if (s_pif_cmds[i]) fprintf(stderr, "  pif channel %u command %x: %u\n", i >> 4, i & 15, s_pif_cmds[i]);
        for (i = 0; i < 16; i++) {   /* where the time went */
            uint32_t j, best = 0;
            for (j = 1; j < NHIST; j++)
                if (s_hist_n[j] > s_hist_n[best]) best = j;
            if (!s_hist_n[best]) break;
            fprintf(stderr, "  pc %08x: %u samples\n", s_hist_pc[best], s_hist_n[best]);
            s_hist_n[best] = 0;
        }
    }
    if (s_unknown) {
        fprintf(stderr, "dryboot: stopped at an instruction it does not know: %08x at %08x\n", s_bad_w, s_bad_pc);
        return 2;
    }
    return 0;
}
