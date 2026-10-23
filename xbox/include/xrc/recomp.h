/* recomp.h - the contract between N64Recomp's generated C and this runtime,
 * for the Pentium III (PLAN.md 3.2).
 *
 * N64Recomp's include/recomp.h (MIT, (c) 2024 Wiseguy) is the
 * specification: the same names, the same recomp_context layout, the same
 * word-swapped RDRAM image. The memory helpers below (LD/SD, lwl/lwr/swl/
 * swr, ldl/ldr/sdl/sdr) follow it closely. What differs here, and why:
 *
 *   gpr       uint64_t by default; uint32_t with -DXRC_GPR32=1, which the
 *             game pack sets when tools/xbox/recomp_scan64.py finds no
 *             64-bit-ISA instruction in the game's code (O32 code from IDO
 *             -mips2 keeps every register sign-extended, so 32 bits hold
 *             the same value), halving the register pressure on a CPU with
 *             seven usable GPRs
 *   MEM_*     rdram is the RDRAM window's physical base P: the buffer is
 *             physically contiguous and the kernel maps it at 0x80000000 + P,
 *             so a KSEG0 address 0x80000000 + x is host address rdram + addr.
 *             One addressing mode, no subtraction, no mask (the ^2/^3 of
 *             halfword and byte accesses stay: the image is word-swapped,
 *             32-bit accesses are native). -DXRC_MEM_MASK=1 masks every
 *             access with 0x1FFFFFFF (KSEG1 and physical addresses land in
 *             the window too; slower: PLAN.md 3.2.1's kill switch)
 *   DMULT     four 32x32 multiplies: no 128-bit integer on i686
 *   rounding  set_cop1_cs writes MXCSR and the x87 control word together;
 *             CVT_W_S is cvtss2si (MXCSR rounding), CVT_W_D and the 64-bit
 *             conversions fistp under the control word; nxdk's lrintf is a
 *             stub that asserts and pdclib has no working fenv
 *   lround*   our own: not in nxdk's libc
 *   checks    NAN_CHECK and CHECK_FR are empty, or a logged fatal with
 *             -DXRC_CHECKS=1 (test builds): no assert in the game loop
 *   RECOMP_FUNC  upstream's weak + noinline by default (a patch library
 *             linked first can replace a function); -DXRC_FUNC_PLAIN=1 makes
 *             it a plain external function so ThinLTO may inline (M3
 *             measures both)
 *
 * The generated code includes nothing else; the runtime functions declared
 * at the bottom are xbox/src/rt's. tests/xbox/test_recomp_h.c holds this
 * header against upstream's on the host (M1). */
#ifndef XRC_RECOMP_H
#define XRC_RECOMP_H

#include <stddef.h>
#include <stdint.h>

#ifndef XRC_GPR32
#define XRC_GPR32 0
#endif
#ifndef XRC_MEM_MASK
#define XRC_MEM_MASK 0
#endif
#ifndef XRC_HIGH_TLB
#define XRC_HIGH_TLB 0
#endif
#ifndef XRC_CHECKS
#define XRC_CHECKS 0
#endif
#ifndef XRC_FUNC_PLAIN
#define XRC_FUNC_PLAIN 0
#endif

#if XRC_FUNC_PLAIN
#define RECOMP_FUNC
#else
/* upstream's clang definition: inline allows a definition per file, extern
 * emits it, weak keeps clang from IPO across an interposable symbol */
#define RECOMP_FUNC extern inline __attribute__((weak, noinline))
#endif
#define SET_FENV_ACCESS()
/* before a loop's backward goto when n64rx writes `restrict` (N64RX_RESTRICT,
 * pipeline.py write_restrict): RDRAM is read again each time round, so a spin
 * on a word another thread sets ends */
#define XRC_LOOP_BARRIER() __asm__ volatile("" ::: "memory")

#ifndef __cplusplus
#ifndef nullptr
#define nullptr ((void*)0)
#endif
#endif

/* ---- 64-bit multiply and divide ---- */
static inline void DMULTU(uint64_t a, uint64_t b, uint64_t* lo64, uint64_t* hi64) {
    uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    *lo64 = (mid << 32) | (uint32_t)p00;
    *hi64 = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

static inline void DMULT(int64_t a, int64_t b, int64_t* lo64, int64_t* hi64) {
    uint64_t lo, hi;
    DMULTU((uint64_t)a, (uint64_t)b, &lo, &hi);
    /* signed high word from the unsigned one */
    if (a < 0) hi -= (uint64_t)b;
    if (b < 0) hi -= (uint64_t)a;
    *lo64 = (int64_t)lo;
    *hi64 = (int64_t)hi;
}

static inline void DDIV(int64_t a, int64_t b, int64_t* quot, int64_t* rem) {
    int overflow = ((uint64_t)a == 0x8000000000000000ull) && (b == -1ll);
    if (b == 0) {   /* the VR4300's, no trap (DIV32 below; interp.c's twin) */
        *quot = a >= 0 ? -1 : 1;
        *rem = a;
        return;
    }
    *quot = overflow ? a : (a / b);
    *rem = overflow ? 0 : (a % b);
}

static inline void DDIVU(uint64_t a, uint64_t b, uint64_t* quot, uint64_t* rem) {
    if (b == 0) {
        *quot = ~0ull;
        *rem = a;
        return;
    }
    *quot = a / b;
    *rem = a % b;
}

/* ---- registers ---- */
#if XRC_GPR32
typedef uint32_t gpr;
#define SIGNED(val) ((int32_t)(val))
#else
typedef uint64_t gpr;
#define SIGNED(val) ((int64_t)(val))
#endif

#define ADD32(a, b) ((gpr)(int32_t)((a) + (b)))
#define SUB32(a, b) ((gpr)(int32_t)((a) - (b)))

/* ---- memory: the RDRAM window (PLAN.md 3.2.1) ---- */
#if XRC_MEM_MASK
#define XRC_ADDR(a) (((uint32_t)(a) & 0x1FFFFFFFu) | 0x80000000u)
#elif XRC_HIGH_TLB
/* a game that maps KSEG2/3 through its TLB (n64rx learns it: Paper Mario's
 * effects at 0xE0200000): rdram + vaddr is the Xbox kernel's system space
 * there, so such an address goes through the game's TLB to its KSEG0 alias
 * (rt/tlb.c). One compare a access, taken rarely. */
uint32_t xrc_high_addr(uint32_t a);
#define XRC_ADDR(a) (__builtin_expect((uint32_t)(a) >= 0xC0000000u, 0) ? xrc_high_addr((uint32_t)(a)) : (uint32_t)(a))
#else
#define XRC_ADDR(a) ((uint32_t)(a))
#endif

#define MEM_W(offset, reg) (*(int32_t*)(rdram + XRC_ADDR((reg) + (offset))))
#define MEM_H(offset, reg) (*(int16_t*)(rdram + (XRC_ADDR((reg) + (offset)) ^ 2)))
#define MEM_B(offset, reg) (*(int8_t*)(rdram + (XRC_ADDR((reg) + (offset)) ^ 3)))
#define MEM_HU(offset, reg) (*(uint16_t*)(rdram + (XRC_ADDR((reg) + (offset)) ^ 2)))
#define MEM_BU(offset, reg) (*(uint8_t*)(rdram + (XRC_ADDR((reg) + (offset)) ^ 3)))
/* lwu (64-bit code): the word zero-extended (upstream's generator emits it; the
 * library sweep found it in Bomberman 64: The Second Attack!) */
#define MEM_WU(offset, reg) (*(uint32_t*)(rdram + XRC_ADDR((reg) + (offset))))

/* A function that reaches the RCP's registers by a constant KSEG1 address
 * (n64rx N64RX_MMIO_RECOMP, pipeline.py write_mmio) stays recompiled with its
 * loads and stores through these: KSEG1 past RDRAM goes to the register model
 * (rt/mmio.c), as the interpreter's do; anything else is the plain access.
 * Its stores are rewritten to xrc_mst_*, and MEM_* is redefined around it to
 * xrc_mld_*. The bytes of a register word are big-endian lanes, as interp.c. */
extern uint32_t xrc_rdram_size;
uint32_t xrc_mmio_read(uint32_t vaddr);
void xrc_mmio_write(uint32_t vaddr, uint32_t v);
#define XRC_IS_MMIO(a) __builtin_expect((uint32_t)((uint32_t)(a) - 0xA0000000u) < 0x20000000u && \
                                        ((uint32_t)(a) & 0x1FFFFFFFu) >= xrc_rdram_size, 0)
static inline int32_t xrc_mld_w(uint8_t* rdram, uint32_t a) {
    return XRC_IS_MMIO(a) ? (int32_t)xrc_mmio_read(a) : *(int32_t*)(rdram + XRC_ADDR(a));
}
static inline uint32_t xrc_mld_hu(uint8_t* rdram, uint32_t a) {
    return XRC_IS_MMIO(a) ? (xrc_mmio_read(a & ~3u) >> ((2u - (a & 2u)) * 8u)) & 0xFFFFu : *(uint16_t*)(rdram + (XRC_ADDR(a) ^ 2));
}
static inline uint32_t xrc_mld_bu(uint8_t* rdram, uint32_t a) {
    return XRC_IS_MMIO(a) ? (xrc_mmio_read(a & ~3u) >> ((3u - (a & 3u)) * 8u)) & 0xFFu : *(uint8_t*)(rdram + (XRC_ADDR(a) ^ 3));
}
static inline void xrc_mst_w(uint8_t* rdram, uint32_t a, uint32_t v) {
    if (XRC_IS_MMIO(a)) xrc_mmio_write(a, v);
    else *(uint32_t*)(rdram + XRC_ADDR(a)) = v;
}
static inline void xrc_mst_h(uint8_t* rdram, uint32_t a, uint32_t v) {
    if (XRC_IS_MMIO(a)) {
        uint32_t sh = (2u - (a & 2u)) * 8u;
        xrc_mmio_write(a & ~3u, (xrc_mmio_read(a & ~3u) & ~(0xFFFFu << sh)) | ((v & 0xFFFFu) << sh));
    } else {
        *(uint16_t*)(rdram + (XRC_ADDR(a) ^ 2)) = (uint16_t)v;
    }
}
static inline void xrc_mst_b(uint8_t* rdram, uint32_t a, uint32_t v) {
    if (XRC_IS_MMIO(a)) {
        uint32_t sh = (3u - (a & 3u)) * 8u;
        xrc_mmio_write(a & ~3u, (xrc_mmio_read(a & ~3u) & ~(0xFFu << sh)) | ((v & 0xFFu) << sh));
    } else {
        *(uint8_t*)(rdram + (XRC_ADDR(a) ^ 3)) = (uint8_t)v;
    }
}

/* the high word at the lower address, as upstream */
#define SD(val, offset, reg) { \
    *(uint32_t*)(rdram + XRC_ADDR((reg) + (offset) + 4)) = (uint32_t)((uint64_t)(val) >> 0); \
    *(uint32_t*)(rdram + XRC_ADDR((reg) + (offset) + 0)) = (uint32_t)((uint64_t)(val) >> 32); \
}

static inline uint64_t load_doubleword(uint8_t* rdram, gpr reg, gpr offset) {
    uint64_t lo = (uint64_t)(uint32_t)MEM_W(reg, offset + 4);
    uint64_t hi = (uint64_t)(uint32_t)MEM_W(reg, offset + 0);
    return lo | (hi << 32);
}

#define LD(offset, reg) load_doubleword(rdram, offset, reg)

static inline gpr do_lwl(uint8_t* rdram, gpr initial_value, gpr offset, gpr reg) {
    gpr address = (offset + reg);
    gpr word_address = address & ~(gpr)0x3;
    uint32_t loaded_value = MEM_W(0, word_address);
    uint32_t misalignment = (uint32_t)address & 0x3;
    gpr masked_value = initial_value & (gpr)(uint32_t)~(0xFFFFFFFFu << (misalignment * 8));
    loaded_value <<= (misalignment * 8);
    return (gpr)(int32_t)((uint32_t)masked_value | loaded_value);
}

static inline gpr do_lwr(uint8_t* rdram, gpr initial_value, gpr offset, gpr reg) {
    gpr address = (offset + reg);
    gpr word_address = address & ~(gpr)0x3;
    uint32_t loaded_value = MEM_W(0, word_address);
    uint32_t misalignment = (uint32_t)address & 0x3;
    gpr masked_value = initial_value & (gpr)(uint32_t)~(0xFFFFFFFFu >> (24 - misalignment * 8));
    loaded_value >>= (24 - misalignment * 8);
    return (gpr)(int32_t)((uint32_t)masked_value | loaded_value);
}

static inline void do_swl(uint8_t* rdram, gpr offset, gpr reg, gpr val) {
    gpr address = (offset + reg);
    gpr word_address = address & ~(gpr)0x3;
    uint32_t initial_value = MEM_W(0, word_address);
    uint32_t misalignment = (uint32_t)address & 0x3;
    uint32_t masked_initial_value = initial_value & ~(0xFFFFFFFFu >> (misalignment * 8));
    uint32_t shifted_input_value = ((uint32_t)val) >> (misalignment * 8);
    MEM_W(0, word_address) = masked_initial_value | shifted_input_value;
}

static inline void do_swr(uint8_t* rdram, gpr offset, gpr reg, gpr val) {
    gpr address = (offset + reg);
    gpr word_address = address & ~(gpr)0x3;
    uint32_t initial_value = MEM_W(0, word_address);
    uint32_t misalignment = (uint32_t)address & 0x3;
    uint32_t masked_initial_value = initial_value & ~(0xFFFFFFFFu << (24 - misalignment * 8));
    uint32_t shifted_input_value = ((uint32_t)val) << (24 - misalignment * 8);
    MEM_W(0, word_address) = masked_initial_value | shifted_input_value;
}

static inline gpr do_ldl(uint8_t* rdram, gpr initial_value, gpr offset, gpr reg) {
    gpr address = (offset + reg);
    gpr dword_address = address & ~(gpr)0x7;
    uint64_t loaded_value = load_doubleword(rdram, 0, dword_address);
    uint32_t misalignment = (uint32_t)address & 0x7;
    uint64_t masked_value = (uint64_t)initial_value & ~(0xFFFFFFFFFFFFFFFFull << (misalignment * 8));
    loaded_value <<= (misalignment * 8);
    return (gpr)(masked_value | loaded_value);
}

static inline gpr do_ldr(uint8_t* rdram, gpr initial_value, gpr offset, gpr reg) {
    gpr address = (offset + reg);
    gpr dword_address = address & ~(gpr)0x7;
    uint64_t loaded_value = load_doubleword(rdram, 0, dword_address);
    uint32_t misalignment = (uint32_t)address & 0x7;
    uint64_t masked_value = (uint64_t)initial_value & ~(0xFFFFFFFFFFFFFFFFull >> (56 - misalignment * 8));
    loaded_value >>= (56 - misalignment * 8);
    return (gpr)(masked_value | loaded_value);
}

static inline void do_sdl(uint8_t* rdram, gpr offset, gpr reg, gpr val) {
    gpr address = (offset + reg);
    gpr dword_address = address & ~(gpr)0x7;
    uint64_t initial_value = load_doubleword(rdram, 0, dword_address);
    uint32_t misalignment = (uint32_t)address & 0x7;
    uint64_t masked_initial_value = initial_value & ~(0xFFFFFFFFFFFFFFFFull >> (misalignment * 8));
    uint64_t shifted_input_value = (uint64_t)val >> (misalignment * 8);
    uint64_t ret = masked_initial_value | shifted_input_value;
    MEM_W(0, dword_address + 4) = (int32_t)(uint32_t)ret;
    MEM_W(0, dword_address + 0) = (int32_t)(uint32_t)(ret >> 32);
}

static inline void do_sdr(uint8_t* rdram, gpr offset, gpr reg, gpr val) {
    gpr address = (offset + reg);
    gpr dword_address = address & ~(gpr)0x7;
    uint64_t initial_value = load_doubleword(rdram, 0, dword_address);
    uint32_t misalignment = (uint32_t)address & 0x7;
    uint64_t masked_initial_value = initial_value & ~(0xFFFFFFFFFFFFFFFFull << (56 - misalignment * 8));
    uint64_t shifted_input_value = (uint64_t)val << (56 - misalignment * 8);
    uint64_t ret = masked_initial_value | shifted_input_value;
    MEM_W(0, dword_address + 4) = (int32_t)(uint32_t)ret;
    MEM_W(0, dword_address + 0) = (int32_t)(uint32_t)(ret >> 32);
}

/* ---- the FPU's rounding mode: MXCSR (floats) and the x87 control word
 * (doubles) together. MIPS RM: 0 nearest, 1 toward zero, 2 up, 3 down;
 * x86 RC: 0 nearest, 1 down, 2 up, 3 toward zero. ---- */
static const uint8_t xrc_rm_to_rc[4] = { 0, 3, 2, 1 };
static const uint8_t xrc_rc_to_rm[4] = { 0, 3, 2, 1 };

/* FCSR.FS (bit 24, flush denormal results to zero, which libultra's
 * osCreateThread gives every thread) as MXCSR.FZ (bit 15): the single-
 * precision SSE results the N64 flushed are flushed here too instead of
 * taking the Pentium III's microcode assist (DAZ, the inputs, is not on a
 * Pentium III and stays off; doubles are x87's, which has no flush). Kill
 * switch: -DXRC_FTZ=0 (review 5a.15) */
#ifndef XRC_FTZ
#define XRC_FTZ 1
#endif
static inline uint32_t get_cop1_cs(void) {
    uint32_t mxcsr;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    return xrc_rc_to_rm[(mxcsr >> 13) & 3] | (XRC_FTZ && (mxcsr & 0x8000u) ? 0x01000000u : 0u);
}

static inline void set_cop1_cs(uint32_t val) {
    uint32_t rc = xrc_rm_to_rc[val & 3], mxcsr;
    uint16_t cw;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    mxcsr = (mxcsr & ~(3u << 13)) | (rc << 13);
    if (XRC_FTZ) mxcsr = (val & 0x01000000u) ? mxcsr | 0x8000u : mxcsr & ~0x8000u;
    __asm__ volatile("ldmxcsr %0" ::"m"(mxcsr));
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    cw = (uint16_t)((cw & ~(3u << 10)) | (rc << 10));
    __asm__ volatile("fldcw %0" ::"m"(cw));
}

#define S32(val) ((int32_t)(val))
#define U32(val) ((uint32_t)(val))
#define S64(val) ((int64_t)(val))
#define U64(val) ((uint64_t)(val))

/* DIV / DIVU (patches/recomp/0003): the VR4300 never traps on a zero
 * divisor: lo = -1 (or 1 for a negative dividend; DIVU all ones), hi = the
 * dividend; 0x80000000 / -1 gives 0x80000000 rem 0. Code built without
 * zero-division checks relies on it (Vigilante 8, World Driver
 * Championship crashed on the host's #DE in the sweep); 32-bit idiv, where
 * N64Recomp divided in 64 bits (a libcall on the P3). -DXRC_DIV_VR4300=0:
 * the plain division, as before (a zero divisor traps). */
#ifndef XRC_DIV_VR4300
#define XRC_DIV_VR4300 1
#endif
#if XRC_DIV_VR4300
#define DIV32(a, b, lo_, hi_)                                                                   \
    do {                                                                                         \
        int32_t xa_ = (int32_t)(a), xb_ = (int32_t)(b);                                          \
        if (xb_ == 0) lo_ = (int64_t)(xa_ >= 0 ? -1 : 1), hi_ = (int64_t)xa_;          \
        else if (xa_ == INT32_MIN && xb_ == -1) lo_ = (int64_t)xa_, hi_ = 0;                \
        else lo_ = (int64_t)(xa_ / xb_), hi_ = (int64_t)(xa_ % xb_);                   \
    } while (0)
#define DIVU32(a, b, lo_, hi_)                                                                  \
    do {                                                                                         \
        uint32_t xa_ = (uint32_t)(a), xb_ = (uint32_t)(b);                                       \
        if (xb_ == 0) lo_ = (int64_t)-1, hi_ = (int64_t)(int32_t)xa_;                  \
        else lo_ = (int64_t)(int32_t)(xa_ / xb_), hi_ = (int64_t)(int32_t)(xa_ % xb_); \
    } while (0)
#else
#define DIV32(a, b, lo_, hi_) \
    (lo_ = S32(S64(S32(a)) / S64(S32(b))), hi_ = S32(S64(S32(a)) % S64(S32(b))))
#define DIVU32(a, b, lo_, hi_) (lo_ = S32(U32(a) / U32(b)), hi_ = S32(U32(a) % U32(b)))
#endif

#define MUL_S(val1, val2) ((val1) * (val2))
#define MUL_D(val1, val2) ((val1) * (val2))
#define DIV_S(val1, val2) ((val1) / (val2))
#define DIV_D(val1, val2) ((val1) / (val2))

#define CVT_S_W(val) ((float)((int32_t)(val)))
#define CVT_D_W(val) ((double)((int32_t)(val)))
#define CVT_D_L(val) ((double)((int64_t)(val)))
#define CVT_S_L(val) ((float)((int64_t)(val)))
#define CVT_D_S(val) ((double)(val))
#define CVT_S_D(val) ((float)(val))

#define TRUNC_W_S(val) ((int32_t)(val))
#define TRUNC_W_D(val) ((int32_t)(val))
#define TRUNC_L_S(val) ((int64_t)(val))
#define TRUNC_L_D(val) ((int64_t)(val))

#define DEFAULT_ROUNDING_MODE 0

/* rounding-mode-aware conversions: the current MXCSR or control word */
static inline int32_t do_cvt_w_s(float val) {
    int32_t r;
    __asm__ volatile("cvtss2si %1, %0" : "=r"(r) : "x"(val));
    return r;
}
static inline int64_t do_cvt_l_s(float val) {
    int64_t r;
    __asm__ volatile("fistpll %0" : "=m"(r) : "t"((double)val) : "st");
    return r;
}
static inline int32_t do_cvt_w_d(double val) {
    int32_t r;
    __asm__ volatile("fistpl %0" : "=m"(r) : "t"(val) : "st");
    return r;
}
static inline int64_t do_cvt_l_d(double val) {
    int64_t r;
    __asm__ volatile("fistpll %0" : "=m"(r) : "t"(val) : "st");
    return r;
}
#define CVT_W_S(val) do_cvt_w_s(val)
#define CVT_L_S(val) do_cvt_l_s(val);
#define CVT_W_D(val) do_cvt_w_d(val)
#define CVT_L_D(val) do_cvt_l_d(val)

/* round.w.* and round.l.*: to the nearest, a half to the even one, as the
 * VR4300 rounds (0.5 gives 0, 1.5 and 2.5 give 2), whatever the rounding
 * mode; the fraction x - trunc(x) is exact, so the half is seen exactly.
 * -DXRC_ROUND_EVEN=0: a half away from zero, as upstream (review 5a.15) */
#ifndef XRC_ROUND_EVEN
#define XRC_ROUND_EVEN 1
#endif
#define XRC_ROUND_HALF(t, d, half) \
    do { if ((d) > (half) || ((d) == (half) && (!XRC_ROUND_EVEN || ((t) & 1)))) (t)++; \
         else if ((d) < -(half) || ((d) == -(half) && (!XRC_ROUND_EVEN || ((t) & 1)))) (t)--; } while (0)
static inline long xrc_lroundf(float x) {
    int32_t t = (int32_t)x;
    float d = x - (float)t;
    XRC_ROUND_HALF(t, d, 0.5f);
    return t;
}
static inline long xrc_lround(double x) {
    int32_t t = (int32_t)x;
    double d = x - (double)t;
    XRC_ROUND_HALF(t, d, 0.5);
    return t;
}
static inline long long xrc_llroundf(float x) {
    int64_t t = (int64_t)x;
    float d = x - (float)t;
    XRC_ROUND_HALF(t, d, 0.5f);
    return t;
}
static inline long long xrc_llround(double x) {
    int64_t t = (int64_t)x;
    double d = x - (double)t;
    XRC_ROUND_HALF(t, d, 0.5);
    return t;
}
#define lroundf(x) xrc_lroundf(x)
#define lround(x) xrc_lround(x)
#define llroundf(x) xrc_llroundf(x)
#define llround(x) xrc_llround(x)

/* sqrt.s in SSE (single, MXCSR rounding), sqrt.d on the x87 (53-bit
 * control word: IEEE double); no libm, no errno */
static inline float xrc_sqrtf(float x) {
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}
static inline double xrc_sqrt(double x) {
    double r;
    __asm__("fsqrt" : "=t"(r) : "0"(x));
    return r;
}
#define sqrtf(x) xrc_sqrtf(x)
#define sqrt(x) xrc_sqrt(x)
#define fabsf(x) __builtin_fabsf(x)
#define fabs(x) __builtin_fabs(x)
/* ceil.* and floor.* (upstream's generator names libm's; the sweep found
 * games that use them): pdclib's, as the interpreter's (interp.c) */
#define floorf(x) __builtin_floorf(x)
#define floor(x) __builtin_floor(x)
#define ceilf(x) __builtin_ceilf(x)
#define ceil(x) __builtin_ceil(x)

#if XRC_CHECKS
void xrc_check_failed(const char* what);
#define NAN_CHECK(val) do { if ((val) != (val)) xrc_check_failed("NaN"); } while (0)
#else
#define NAN_CHECK(val) ((void)0)
#endif

typedef union {
    double d;
    struct {
        float fl;
        float fh;
    };
    struct {
        uint32_t u32l;
        uint32_t u32h;
    };
    uint64_t u64;
} fpr;

/* f_odd: in the MIPS II float mode an odd FPR's word is the even one's high
 * word, which the generator spells ctx->f_odd[(i - 1) * 2]. Upstream keeps
 * a pointer, set to &f0.u32h; here it is an array over the same words, in
 * the struct, so every context access in the generated code is an offset
 * from ctx and none goes through a pointer loaded from it: what `restrict`
 * on ctx needs (docs/roadmap.md B6). A copied context (strand.c) also stops
 * pointing at the old one's registers. -DXRC_FODD_ARRAY=0: the pointer */
#ifndef XRC_FODD_ARRAY
#define XRC_FODD_ARRAY 1
#endif
typedef struct {
    gpr r0,  r1,  r2,  r3,  r4,  r5,  r6,  r7,
        r8,  r9,  r10, r11, r12, r13, r14, r15,
        r16, r17, r18, r19, r20, r21, r22, r23,
        r24, r25, r26, r27, r28, r29, r30, r31;
#if XRC_FODD_ARRAY
    union {
        struct {
            fpr f0,  f1,  f2,  f3,  f4,  f5,  f6,  f7,
                f8,  f9,  f10, f11, f12, f13, f14, f15,
                f16, f17, f18, f19, f20, f21, f22, f23,
                f24, f25, f26, f27, f28, f29, f30, f31;
        };
        struct {
            uint32_t f_odd_base_;   /* f0.u32l */
            uint32_t f_odd[63];     /* f_odd[2k] = f(2k).u32h */
        };
    };
#else
    fpr f0,  f1,  f2,  f3,  f4,  f5,  f6,  f7,
        f8,  f9,  f10, f11, f12, f13, f14, f15,
        f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31;
#endif
    uint64_t hi, lo;
#if !XRC_FODD_ARRAY
    uint32_t* f_odd;
#endif
    uint32_t status_reg;
    uint8_t mips3_float_mode;
} recomp_context;

/* XRC_FODD(ctx): the word mtc1, mfc1, lwc1, swc1 and the conversions reach
 * for an odd FPR, which n64rx spells instead of the generator's
 * ctx->f_odd[(i - 1) * 2] in a game whose threads run with Status.FR set
 * (pipeline.py write_fodd): word (i - 1) * 2 from f0's high word is f(i-1)'s
 * high word with FR = 0 (the even register's other half, as O32 code wants)
 * and, one word on, f(i)'s own low word with FR = 1, the register the
 * generator's .s arithmetic (ctx->fN.fl) uses. mips3_float_mode is the
 * context's FR: set where a thread starts (its game's osCreateThread rule)
 * and by cop0_status_write. Without it a Rare game's hand-written matrix code
 * (mov.s $f3 ... swc1 $f17) stored the even registers' high words: Jet Force
 * Gemini's view matrices came out NaN (docs/renderer.md). Still an offset
 * from ctx, so restrict holds. XRC_FR: 0 (FR = 0 everywhere, as before), 1
 * (osCreateThread gives every thread FR), 2 (Rare's: FR unless the id is
 * negative); n64rx sets it from the ROM (saves.py thread_fr; N64RX_FR=0
 * leaves it 0) */
#ifndef XRC_FR
#define XRC_FR 0
#endif
#if XRC_FR
#define XRC_FODD(c) ((uint32_t*)&(c)->f0 + 1 + (c)->mips3_float_mode)
#else
#define XRC_FODD(c) ((uint32_t*)&(c)->f0 + 1)
#endif

#if XRC_CHECKS
#define CHECK_FR(ctx, idx) do { if (((idx) & 1) != 0 && !(ctx)->mips3_float_mode) xrc_check_failed("odd FPR"); } while (0)
#else
#define CHECK_FR(ctx, idx) ((void)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the runtime (xbox/src/rt) ---- */
void cop0_status_write(recomp_context* ctx, gpr value);
gpr cop0_status_read(recomp_context* ctx);
void switch_error(const char* func, uint32_t vram, uint32_t jtbl);
void do_break(uint32_t vram);

typedef void(recomp_func_t)(uint8_t* rdram, recomp_context* ctx);
typedef void(recomp_func_ext_t)(uint8_t* rdram, recomp_context* ctx, uintptr_t arg);

recomp_func_t* get_function(int32_t vram);
void xrc_ovl_relocate(uint32_t ram, uint32_t hdr, uint32_t vram);   /* ovl.c: an overlay the game relocates (n64rx's hook) */
#define LOOKUP_FUNC(val) get_function((int32_t)(val))

extern int32_t* section_addresses;

#define LO16(x) ((x) & 0xFFFF)
#define HI16(x) (((x) >> 16) + (((x) >> 15) & 1))
#define RELOC_HI16(section_index, offset) HI16(section_addresses[section_index] + (offset))
#define RELOC_LO16(section_index, offset) LO16(section_addresses[section_index] + (offset))

void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t instruction_vram);
void pause_self(uint8_t* rdram);

/* strand.c: a call to the game's own setjmp (n64rx: n64_setjmp) is a host
 * setjmp here in the caller, kept per N64 buffer, so a longjmp back to it
 * from another context lands in this live frame (pipeline.py
 * write_setjmp_sites). Kill switch: N64RX_STRANDS=0. */
void** xrc_strand_jb(uint32_t buf);
void xrc_strand_resumed(uint8_t* rdram, recomp_context* ctx, uint32_t buf);
/* the empty asm after it keeps the call from being a tail call when the
 * site ends its function: jb must hold this frame, not one already gone
 * (review 5a.10: clang made it `jmp n64_setjmp`) */
#define XRC_SETJMP(rdram, ctx) do { \
        uint32_t xrc_b_ = (uint32_t)(ctx)->r4; \
        if (__builtin_setjmp(xrc_strand_jb(xrc_b_))) xrc_strand_resumed(rdram, ctx, xrc_b_); \
        else n64_setjmp(rdram, ctx); \
        __asm__ volatile("" ::: "memory"); \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif
