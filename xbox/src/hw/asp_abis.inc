/* asp_abis.inc - the other audio microcode ABIs and the registry (PLAN.md 3.8).
 *
 * Part of asp_hle.c (#included at its end: the DMEM model, the DMA helpers
 * and the shared commands are its statics). Not a translation unit of its
 * own, hence .inc: xbox/CMakeLists.txt globs the .c files of src/hw.
 *
 * --- Registry --------------------------------------------------------------
 * Every audio microcode in the libultra lineage keeps its command jump table
 * in its data segment, one 16-bit IMEM address per opcode, directly before
 * the four ADPCM nibble masks f000 0f00 00f0 000f. Its place and length name
 * the family; the entries that point at opcode 0's handler are the opcodes
 * that microcode lacks:
 *   - table at data+0x10, 16 entries: aspMain (ABI1, libultra's audio);
 *   - table at data+0x10, 32 or 24 entries: the Nintendo EAD family ("nead",
 *     the synthesizer of SM64 EU/Shindou, Mario Kart 64, Star Fox 64,
 *     Zelda): opcodes 16..22 and others added;
 *   - table at data+0x00, 16 entries: n_aspMain (libultra's n_audio, fixed
 *     DMEM buffers).
 * Within nead the variants differ in a few opcodes, told apart by what the
 * table holds: no ADDMIXER (4) = the first nead (ASP_NEAD_MK: no wet ramp in
 * ENVSETUP1, ENVMIXER without the wet negation and the swap); a 32-entry
 * table with opcode 14 present keeps the old INTERLEAVE (SETBUFF's out and
 * count) and POLEF at 14 (ASP_NEAD_OLD_INTERL); a 24-entry table has FILTER
 * at 7, DUPLICATE at 9 and HILOGAIN at 14 (ASP_NEAD_SHORT). Those rules were
 * checked against the tables of the maintainer's ROMs (Pilotwings 64, SM64
 * US, Pokemon Snap: ABI1; Mario Kart 64: nead MK; Ocarina of Time: nead,
 * 24 entries; Paper Mario, Smash Bros.: n_aspMain) and agree with how emulators sort the known variants; nothing
 * from any microcode is stored. Mupen64Plus-RSP-HLE (GPL) was read for the
 * behaviour of each command, nothing copied; the encodings are the decomps'
 * abi.h / n_abi.h (mk64, papermario).
 *
 * Kill switches: -DASP_HLE_NEAD=0 and -DASP_HLE_NAUDIO=0 make that family
 * unknown (silent).
 *
 * --- nead ------------------------------------------------------------------
 * No segments (opcode 7 in the 32-entry table is a no-op for the lists:
 * mk64 sends aSegment(0, 0)); addresses are the low 24 bits. LOADBUFF and
 * SAVEBUFF carry their own DMEM offset and count, MIXER and ADDMIXER their
 * count; ADPCM, RESAMPLE, POLEF and the old INTERLEAVE use SETBUFF's. The
 * envelope is three unsigned Q16 levels (dry left, dry right, wet) stepped
 * once per 8 samples: dry = (in x level) >> 16, wet = (dry x wet level) >> 16,
 * each optionally bit-inverted (the "negate" flags), added with saturation.
 * DIVERGES: the opcodes no game here uses yet (S8DEC, UNK_25; 3 and 23 of
 * the Zelda microcode) are counted as unknown and skipped.
 *
 * --- n_aspMain ---------------------------------------------------------------
 * Fixed buffers: offsets in the commands are relative to N_MAIN (0x4F0);
 * every RESAMPLE, ENVMIXER and MIXER works on N_COUNT bytes (184 samples).
 * ENVMIXER is linear: per sample the left and right volumes step by rate / 8
 * (Q16.16) until they reach their targets, the gains are volume x dry|wet
 * (Q15, rounded) and the four sends mix as A_MIXER does. Its state (80 bytes
 * the CPU allocates and never reads) is private: native s32 and s16 here.
 * DIVERGES: opcode 14's filters (Donkey Kong 64's and Conker's variants) are
 * not implemented; plain n_aspMain only stores its rate half-word there. */

#ifndef ASP_HLE_NEAD
#define ASP_HLE_NEAD 1      /* 0: the nead family reads as unknown (silent) */
#endif
#ifndef ASP_HLE_NAUDIO
#define ASP_HLE_NAUDIO 1    /* 0: n_aspMain reads as unknown (silent) */
#endif

/* ---- registry ------------------------------------------------------------- */

static asp_ucode_t s_uc = {ASP_ABI_AUDIO, 0, 0xFFFFu, 16};

static uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }

uint32_t asp_hle_identify(const uint8_t* d, uint32_t n, asp_ucode_t* u)
{
    static const uint8_t masks[8] = {0xF0, 0x00, 0x0F, 0x00, 0x00, 0xF0, 0x00, 0x0F};
    uint32_t m, base, i, noop;
    memset(u, 0, sizeof *u);
    for (m = 0x20; m + 8 <= n && m <= 0x60; m += 0x10)
        if (!memcmp(d + m, masks, 8)) break;
    if (m + 8 > n || m > 0x60) return u->abi = ASP_ABI_UNKNOWN;
    /* the 16-byte constants block (0001 0002 ffff ...) or the table first */
    base = (be16(d) == 0 && be16(d + 2) == 1) ? 0x10 : 0;
    u->entries = (m - base) / 2;
    noop = be16(d + base);
    for (i = 0; i < u->entries && i < 32; i++)
        if (be16(d + base + 2 * i) != noop || i == 0) u->present |= 1u << i;
    if (base == 0x10 && u->entries == 16) {
        u->abi = ASP_ABI_AUDIO;
    } else if (base == 0x10 && (u->entries == 32 || u->entries == 24)) {
        u->abi = ASP_HLE_NEAD ? ASP_ABI_NEAD : ASP_ABI_UNKNOWN;
        if (!(u->present & (1u << 4))) u->flags |= ASP_NEAD_MK;
        if (u->entries == 24) u->flags |= ASP_NEAD_SHORT;
        else if (u->present & (1u << 14)) u->flags |= ASP_NEAD_OLD_INTERL;
    } else if (base == 0 && u->entries == 16) {
        u->abi = ASP_HLE_NAUDIO ? ASP_ABI_NAUDIO : ASP_ABI_UNKNOWN;
    } else {
        u->abi = ASP_ABI_UNKNOWN;
    }
    return u->abi;
}

void asp_hle_set_ucode(const asp_ucode_t* u)
{
    if (!s_inited) asp_hle_reset();
    s_uc = *u;
}

const char* asp_hle_abi_name(uint32_t abi)
{
    switch (abi) {
    case ASP_ABI_AUDIO: return "aspMain (ABI1)";
    case ASP_ABI_NEAD: return "nead";
    case ASP_ABI_NAUDIO: return "n_aspMain";
    default: return "unknown";
    }
}

/* ---- shared by nead and n_aspMain ------------------------------------------ */

/* alist DMA: DMEM & ~3, RDRAM & ~7, the length rounded up to 8 */
static void dma_load(uint32_t dmem, uint32_t a, uint32_t count)
{
    uint32_t n = cap_dmem(align_up(count, 8));
    uint8_t* p;
    if (!n) return;
    p = rdram_at(a & 0xFFFFFFu);
    if (p) dmem_from_rd(dmem & ~3u, p, n);
    else dmem_zero(dmem & ~3u, n);
}

static void dma_save(uint32_t dmem, uint32_t a, uint32_t count)
{
    uint32_t n = cap_dmem(align_up(count, 8));
    uint8_t* p;
    if (!n) return;
    p = rdram_at(a & 0xFFFFFFu);
    if (p) dmem_to_rd(p, dmem & ~3u, n);
}

static void dmem_move(uint32_t dst, uint32_t src, uint32_t n)
{
    static uint8_t tmp[DMEM_SIZE];
    n = cap_dmem(n);
    dmem_to(tmp, src, n);       /* DIVERGES: memmove semantics, see cmd_interleave */
    dmem_from(dst, tmp, n);
}

/* ---- nead -------------------------------------------------------------------- */

static struct {
    uint16_t env[3], step[3];   /* ENVSETUP1/2: dry left, dry right, wet; Q16 */
    uint32_t filter_count, filter_lut;
} N;

/* ENVMIXER: n samples (rounded up to 8) from in to the four sends */
static void nead_envmix(uint32_t in, uint32_t n, int swap, const int16_t x[4], uint32_t dl, uint32_t dr, uint32_t wl,
                        uint32_t wr)
{
    uint32_t i, k, s = in >> 1, o[4];
    o[0] = dl >> 1, o[1] = dr >> 1, o[2] = (swap ? wr : wl) >> 1, o[3] = (swap ? wl : wr) >> 1;
    n = align_up(n, 8);
    for (i = 0; i < n; i += 8) {
        for (k = 0; k < 8; k++) {
            int32_t v = DS(s + i + k);
            int16_t l = (int16_t)(((v * (int32_t)N.env[0]) >> 16) ^ x[0]);
            int16_t r = (int16_t)(((v * (int32_t)N.env[1]) >> 16) ^ x[1]);
            int16_t l2 = (int16_t)(((l * (int32_t)N.env[2]) >> 16) ^ x[2]);
            int16_t r2 = (int16_t)(((r * (int32_t)N.env[2]) >> 16) ^ x[3]);
            DS(o[0] + i + k) = sat16(DS(o[0] + i + k) + l);
            DS(o[1] + i + k) = sat16(DS(o[1] + i + k) + r);
            DS(o[2] + i + k) = sat16(DS(o[2] + i + k) + l2);
            DS(o[3] + i + k) = sat16(DS(o[3] + i + k) + r2);
        }
        for (k = 0; k < 3; k++) N.env[k] = (uint16_t)(N.env[k] + N.step[k]);
    }
}

/* FILTER (24-entry table at 7, 32-entry at 27): flags > 1 set the byte
 * count and the table's address; else the buffer at dmem runs through an
 * 8-tap FIR, y[n] = sum_j h[j] x[n - j] (Q15, rounded, saturated), its
 * history the 8 samples saved in the state, whose next 16 bytes hold the
 * previous call's table: the taps used are the average of the two, saved
 * there for the next call. DIVERGES: the game's own table is not written
 * (an emulator writes the average to both); the restart flag is ignored, as
 * there (the game clears the state). */
static void nead_filter(uint32_t w0, uint32_t a)
{
    static int16_t x[8 + 0x800];
    int16_t h[8], prev[8], st[16];
    uint32_t n, d = (w0 & 0xFFFF) >> 1, i;
    int j;
    if (((w0 >> 16) & 0xFF) > 1) {
        N.filter_count = w0 & 0xFFFF, N.filter_lut = a;
        return;
    }
    n = align_up(N.filter_count, 16) >> 1;
    if (n > 0x800) n = 0x800;
    rd_load(N.filter_lut, h, sizeof h);
    rd_load(a, st, sizeof st);
    for (j = 0; j < 8; j++) prev[j] = st[8 + j], h[j] = (int16_t)((h[j] + prev[j]) >> 1);
    memcpy(x, st, 16);
    for (i = 0; i < n; i++) x[8 + i] = DS(d + i);
    for (i = 0; i < n; i++) {
        int64_t acc = 0x4000;
        for (j = 0; j < 8; j++) acc += (int32_t)x[8 + i - (uint32_t)j] * h[j];
        DS(d + i) = sat16_64(acc >> 15);
    }
    if (n) memcpy(st, x + n, 16);
    memcpy(st + 8, h, 16);
    rd_store(a, st, sizeof st);
}

/* HILOGAIN: x * gain (signed Q4.4) */
static void nead_hilogain(uint32_t w0, uint32_t w1)
{
    int32_t g = (int8_t)(w0 >> 16);
    uint32_t d = (w1 >> 16) >> 1, n = (w0 & 0xFFF) >> 1, i;
    for (i = 0; i < n; i++) DS(d + i) = sat16((DS(d + i) * g) >> 4);
}

static void exec_nead(uint32_t w0, uint32_t w1)
{
    uint32_t op = w0 >> 24, flags = (w0 >> 16) & 0xFF, a = w1 & 0xFFFFFFu;
    s_cur_op = op;
    int shortt = (s_uc.flags & ASP_NEAD_SHORT) != 0;
    if (op >= 32 || !(s_uc.present & (1u << op))) {
        s_stats.cmds[op ? 32 : 0]++;
        return;   /* opcode 0, or one this microcode lacks: its handler is SPNOOP's */
    }
    s_stats.cmds[op]++;
    switch (op) {
    case 0:   /* SPNOOP */
        break;
    case 1:   /* ADPCM */
        cmd_adpcm(flags, T.in, T.out, T.count, a, (flags & 4) != 0);
        break;
    case 2:   /* CLEARBUFF */
        dmem_zero(w0 & 0xFFFF, w1 & 0xFFF);
        break;
    case 4: { /* ADDMIXER */
        uint32_t n = ((w0 >> 12) & 0xFF0) >> 1, s = (w1 >> 16) >> 1, d = (w1 & 0xFFFF) >> 1, i;
        for (i = 0; i < n; i++) DS(d + i) = sat16(DS(d + i) + DS(s + i));
        break;
    }
    case 5:   /* RESAMPLE */
        cmd_resample(flags, w0 & 0xFFFF, T.in, T.out, T.count, a);
        break;
    case 6: { /* RESAMPLE_ZOH: nearest sample, pitch Q1.15, the start fraction in w1 */
        uint32_t ip = T.in >> 1, op2 = T.out >> 1, n = T.count >> 1, step = (w0 & 0xFFFF) << 1, fr = w1 & 0xFFFF, i;
        for (i = 0; i < n; i++) {
            DS(op2 + i) = DS(ip);
            fr += step;
            ip += fr >> 16;
            fr &= 0xFFFF;
        }
        break;
    }
    case 7:
        if (shortt) nead_filter(w0, a);   /* else SEGMENT: the lists' addresses are physical */
        break;
    case 8:   /* SETBUFF */
        T.in = w0 & 0xFFFF, T.out = w1 >> 16, T.count = w1 & 0xFFFF;
        break;
    case 9:   /* DUPLICATE (24-entry table) */
    case 26: { /* DUPLICATE (32-entry table): 128 bytes, `count` copies */
        static uint8_t blk[128];
        uint32_t c = flags, d = w1 >> 16;
        dmem_to(blk, w0 & 0xFFFF, 128);
        for (; c; c--, d += 128) dmem_from(d, blk, 128);
        break;
    }
    case 10:  /* DMEMMOVE */
        if (w1 & 0xFFFF) dmem_move(w1 >> 16, w0 & 0xFFFF, align_up(w1 & 0xFFFF, 4));
        break;
    case 11:  /* LOADADPCM */
        cmd_loadadpcm(w0 & 0xFFFF, a);
        break;
    case 12:  /* MIXER: count in w0, gain Q15 */
        cmd_mixer((int16_t)w0, w1 >> 16, w1 & 0xFFFF, (w0 >> 12) & 0xFF0);
        break;
    case 13:  /* INTERLEAVE */
        if (s_uc.flags & ASP_NEAD_OLD_INTERL) {
            if (T.count) cmd_interleave(T.out, w1 >> 16, w1 & 0xFFFF, T.count);
        } else {
            cmd_interleave(w0 & 0xFFFF, w1 >> 16, w1 & 0xFFFF, (w0 >> 12) & 0xFF0);
        }
        break;
    case 14:
        if (shortt) nead_hilogain(w0, w1);   /* HILOGAIN in the 24-entry table */
        else if (T.count) cmd_polef(flags, (int16_t)w0, T.in, T.out, T.count, a);   /* POLEF */
        break;
    case 24:
        nead_hilogain(w0, w1);
        break;
    case 15:  /* SETLOOP */
        T.loop = a;
        break;
    case 16: { /* DMEMMOVE2 / copy blocks: `count` blocks of the size in w1, 32 bytes a step, source following */
        int32_t c = (int32_t)flags;
        uint32_t si = w0 & 0xFFFF, so = w1 >> 16, bs = w1 & 0xFFFF;
        do {
            int32_t left = (int32_t)bs;
            do {
                dmem_move(so, si, 32);
                si += 32, so += 32, left -= 32;
            } while (left > 0);
        } while (--c > 0);
        break;
    }
    case 17: { /* DOWNSAMPLE_HALF: every other sample */
        uint32_t n = w0 & 0xFFFF, s = (w1 >> 16) >> 1, d = (w1 & 0xFFFF) >> 1, i;
        for (i = 0; i < n; i++) DS(d + i) = DS(s + 2 * i);
        break;
    }
    case 18:  /* ENVSETUP1 */
        N.env[2] = (uint16_t)((w0 >> 8) & 0xFF00);
        N.step[2] = (s_uc.flags & ASP_NEAD_MK) ? 0 : (uint16_t)w0;
        N.step[0] = (uint16_t)(w1 >> 16), N.step[1] = (uint16_t)w1;
        break;
    case 19: { /* ENVMIXER */
        int16_t x[4];
        int mk = (s_uc.flags & ASP_NEAD_MK) != 0;
        x[0] = (int16_t)-(int16_t)((w0 >> 1) & 1);
        x[1] = (int16_t)-(int16_t)(w0 & 1);
        x[2] = mk ? 0 : (int16_t)-(int16_t)((w0 >> 3) & 1);
        x[3] = mk ? 0 : (int16_t)-(int16_t)((w0 >> 2) & 1);
        nead_envmix((w0 >> 12) & 0xFF0, (w0 >> 8) & 0xFF, mk ? 0 : (int)((w0 >> 4) & 1), x, (w1 >> 20) & 0xFF0,
                    (w1 >> 12) & 0xFF0, (w1 >> 4) & 0xFF0, (w1 << 4) & 0xFF0);
        break;
    }
    case 20:  /* LOADBUFF */
        dma_load(w0 & 0xFFF, a, (w0 >> 12) & 0xFFF);
        break;
    case 21:  /* SAVEBUFF */
        dma_save(w0 & 0xFFF, a, (w0 >> 12) & 0xFFF);
        break;
    case 22:  /* ENVSETUP2 */
        N.env[0] = (uint16_t)(w1 >> 16), N.env[1] = (uint16_t)w1;
        break;
    case 27:  /* FILTER (32-entry table) */
        nead_filter(w0, a);
        break;
    default:  /* S8DEC, UNK_25, opcodes 3 and 23: DIVERGES: not implemented */
        s_stats.cmds[32]++;
        break;
    }
}

/* ---- n_aspMain ----------------------------------------------------------------- */

#define N_MAIN 0x4F0u
#define N_MAIN2 0x660u
#define N_DRY_L 0x9D0u
#define N_DRY_R 0xB40u
#define N_WET_L 0xCB0u
#define N_WET_R 0xE20u
#define N_COUNT 0x170u      /* bytes: 184 samples */

typedef struct {
    int32_t value[2], step[2], target[2];
    int16_t dry, wet;
} NEnvState;   /* 28 bytes, native; the 80-byte ENVMIX_STATE is private to the microcode */

static void naudio_envmix(uint32_t flags, uint32_t state)
{
    NEnvState e;
    uint32_t i, c, in = N_MAIN >> 1, out[4] = {N_DRY_L >> 1, N_DRY_R >> 1, N_WET_L >> 1, N_WET_R >> 1};
    if (flags & A_INIT) {
        for (c = 0; c < 2; c++) {
            e.step[c] = T.rate[c] / 8;
            e.value[c] = (int32_t)((uint32_t)(uint16_t)T.vol[c] << 16);
            e.target[c] = (int32_t)((uint32_t)(uint16_t)T.target[c] << 16);
        }
        e.dry = T.dry, e.wet = T.wet;
    } else {
        rd_load(state, &e, sizeof e);
    }
    for (i = 0; i < N_COUNT / 2; i++) {
        int16_t vol[2], g[4];
        int32_t x = DS(in + i);
        for (c = 0; c < 2; c++) {
            int64_t v = (int64_t)e.value[c] + e.step[c];
            if (e.step[c] <= 0 ? v <= e.target[c] : v >= e.target[c]) v = e.target[c], e.step[c] = 0;
            e.value[c] = (int32_t)v;
            vol[c] = (int16_t)(e.value[c] >> 16);
        }
        g[0] = sat16((vol[0] * e.dry + 0x4000) >> 15);
        g[1] = sat16((vol[1] * e.dry + 0x4000) >> 15);
        g[2] = sat16((vol[0] * e.wet + 0x4000) >> 15);
        g[3] = sat16((vol[1] * e.wet + 0x4000) >> 15);
        for (c = 0; c < 4; c++) DS(out[c] + i) = sat16(DS(out[c] + i) + mulq15(x, g[c]));
    }
    rd_store(state, &e, sizeof e);
}

static void exec_naudio(uint32_t w0, uint32_t w1)
{
    uint32_t op = w0 >> 24, flags = (w0 >> 16) & 0xFF;
    s_cur_op = op;
    if (op >= 16 || op == 7 || op == 8) {
        s_stats.cmds[32]++;
        return;
    }
    s_stats.cmds[op]++;
    switch (op) {
    case 0:   /* SPNOOP */
        break;
    case 1:   /* ADPCM: state in w0; w1 = flags 4 | count 12 | in 4 | out 12 */
        cmd_adpcm(w1 >> 28, N_MAIN + ((w1 >> 12) & 0xF), N_MAIN + (w1 & 0xFFF), (w1 >> 16) & 0xFFF, w0 & 0xFFFFFFu, 0);
        break;
    case 2:   /* CLEARBUFF */
        dmem_zero(N_MAIN + (w0 & 0xFFFF), cap_dmem(w1 & 0xFFF));
        break;
    case 3:   /* ENVMIXER: w0's low half is the right volume */
        T.vol[1] = (int16_t)w0;
        naudio_envmix(flags, w1 & 0xFFFFFFu);
        break;
    case 4:   /* LOADBUFF */
        dma_load(N_MAIN + (w0 & 0xFFF), w1, (w0 >> 12) & 0xFFF);
        break;
    case 5:   /* RESAMPLE: state in w0; w1 = flags 2 | pitch 16 | in 12 | out select 2 */
        cmd_resample(w1 >> 30, (w1 >> 14) & 0xFFFF, N_MAIN + ((w1 >> 2) & 0xFFF), (w1 & 3) ? N_MAIN2 : N_MAIN, N_COUNT,
                     w0 & 0xFFFFFFu);
        break;
    case 6:   /* SAVEBUFF */
        dma_save(N_MAIN + (w0 & 0xFFF), w1, (w0 >> 12) & 0xFFF);
        break;
    case 9:   /* SETVOL */
        if (flags & A_VOL) {
            if (flags & A_LEFT) T.vol[0] = (int16_t)w0, T.dry = (int16_t)(w1 >> 16), T.wet = (int16_t)w1;
            else T.target[1] = (int16_t)w0, T.rate[1] = (int32_t)w1;
        } else {
            T.target[0] = (int16_t)w0, T.rate[0] = (int32_t)w1;
        }
        break;
    case 10:  /* DMEMMOVE */
        dmem_move(N_MAIN + (w1 >> 16), N_MAIN + (w0 & 0xFFFF), align_up(w1 & 0xFFFF, 4));
        break;
    case 11:  /* LOADADPCM */
        cmd_loadadpcm(w0 & 0xFFFF, w1 & 0xFFFFFFu);
        break;
    case 12:  /* MIXER */
        cmd_mixer((int16_t)w0, N_MAIN + (w1 >> 16), N_MAIN + (w1 & 0xFFFF), N_COUNT);
        break;
    case 13:  /* INTERLEAVE: the dry pair into N_MAIN */
        cmd_interleave(N_MAIN, N_DRY_L, N_DRY_R, N_COUNT);
        break;
    case 14:  /* the low half of the right rate (n_aspMain jumps into SETVOL here) */
        T.rate[1] = (int32_t)(((uint32_t)T.rate[1] & 0xFFFF0000u) | (w1 & 0xFFFF));
        break;
    case 15:  /* SETLOOP */
        T.loop = w1 & 0xFFFFFFu;
        break;
    }
}
