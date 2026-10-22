/* bench_dl.c - one graphics task replayed on the host (tools/xbox/bench_dl.py).
 *
 * A test build made with -DXRC_DLDUMP=<s> dumps the first graphics task
 * after <s> seconds (the task's words and the whole RDRAM image, as the
 * runtime keeps it: words native, halfwords at ^2, bytes at ^3). This runs
 * the real interpreter (f3d_interp.c, texconv.c, ucode.c, fbfx.c) over that
 * image again and again with a back end that only counts, times it, and on
 * Windows samples the running thread's instruction pointer every
 * millisecond, so bench_dl.py can say which functions the time goes to. The
 * back end's own cost (the NV2A's push buffer) is not in it.
 *
 * usage: bench_dl <dir> <tasks> <runs> [samples.txt [picture.ppm [draws]]]
 * (<dir>/task<k>.bin and ram<k>.bin for k = 0 .. tasks - 1) */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "f3d_interp.h"
#include "f3dex_gbi.h"
#include "texconv.h"
#include "ucode.h"
#include "xgfx.h"
#include "xhw.h"

#define RAM_MAX (8u << 20)
enum { TK_UCODE_DATA_W = 6, TK_UCODE_DATA_SIZE_W = 7, TK_DATA_PTR_W = 12 };   /* OSTask words (ultra.h) */
static uint8_t s_ram[RAM_MAX] __attribute__((aligned(16)));
uint8_t* xrc_rdram_host = s_ram;
uint8_t* xrc_rdram = s_ram;
uint32_t xrc_rdram_size = RAM_MAX;
uint32_t xrc_tlb_to_phys(uint32_t vaddr) { (void)vaddr; return ~0u; }

void xu_from_rdram(uint8_t* dst, uint32_t src, uint32_t n) {
    uint32_t i, m = xrc_rdram_size - 1;
    for (i = 0; i < n; i++) dst[i] = s_ram[((src + i) ^ 3) & m];
}

/* ---- the counting back end, and a software picture of the first run ----
 * The picture: the colour combiner (one or two cycles, every input but
 * noise), point-sampled textures with their wrap modes, perspective-correct
 * through w, depth, alpha test, alpha blending, fog and the scissor; enough
 * to see what a list draws, not the RDP's exact pixels. */
static unsigned long long s_tris, s_draws, s_texs, s_clears;
static XgfxOutput s_o = { 640, 480, 32, 16777215.0f, 1 };
void xgfx_output(XgfxOutput* out) { *out = s_o; }
void xgfx_frame_begin(void) {}
static volatile uint32_t s_sink;
static int s_render;
#define FBW 640
#define FBH 480
static uint32_t s_fb[FBW * FBH];
static float s_zb[FBW * FBH];
typedef struct { uint32_t w, h; uint32_t* px; } Tex;
#define MAXTEX 65536
static Tex s_tex[MAXTEX];

static void sample(const XgfxState* st, int u, float s, float t, float out[4]) {
    const Tex* tx;
    int x, y, w, h;
    uint32_t p;
    if (!st->tex[u] || st->tex[u] >= MAXTEX || !(tx = &s_tex[st->tex[u]])->px) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        return;
    }
    w = (int)tx->w, h = (int)tx->h;
    x = (int)floorf(s * (float)w), y = (int)floorf(t * (float)h);
    for (int k = 0; k < 2; k++) {
        int* v = k ? &y : &x;
        int n = k ? h : w;
        uint32_t mode = k ? st->wrap_t[u] : st->wrap_s[u];
        if (mode == XGFX_WRAP_CLAMP) *v = *v < 0 ? 0 : *v >= n ? n - 1 : *v;
        else if (mode == XGFX_WRAP_MIRROR) {
            int m = ((*v % (2 * n)) + 2 * n) % (2 * n);
            *v = m < n ? m : 2 * n - 1 - m;
        } else *v = ((*v % n) + n) % n;
    }
    p = tx->px[y * w + x];
    out[0] = (float)(p >> 16 & 255) / 255.0f, out[1] = (float)(p >> 8 & 255) / 255.0f;
    out[2] = (float)(p & 255) / 255.0f, out[3] = (float)(p >> 24) / 255.0f;
}
static float c8(uint8_t v) { return (float)v / 255.0f; }
/* one cycle of the combiner: (A - B) * C + D for colour and for alpha */
static void cycle(int cyc, const XgfxState* st, const float t0[4], const float t1[4], const float sh[4], const float comb[4],
                  float out[4]) {
    uint32_t w0 = st->cc0, w1 = st->cc1;
    int a = cyc ? (w0 >> 5 & 15) : (w0 >> 20 & 15), c = cyc ? (w0 & 31) : (w0 >> 15 & 31);
    int b = cyc ? (w1 >> 24 & 15) : (w1 >> 28 & 15), d = cyc ? (w1 >> 6 & 7) : (w1 >> 15 & 7);
    int aa = cyc ? (w1 >> 21 & 7) : (w0 >> 12 & 7), ac = cyc ? (w1 >> 18 & 7) : (w0 >> 9 & 7);
    int ab = cyc ? (w1 >> 3 & 7) : (w1 >> 12 & 7), ad = cyc ? (w1 & 7) : (w1 >> 9 & 7);
    float pr[4] = { c8(st->prim[0]), c8(st->prim[1]), c8(st->prim[2]), c8(st->prim[3]) };
    float en[4] = { c8(st->env[0]), c8(st->env[1]), c8(st->env[2]), c8(st->env[3]) };
    const float* srcs[6] = { comb, t0, t1, pr, sh, en };
    for (int i = 0; i < 3; i++) {
        float A, B, C, D;
        A = a < 6 ? srcs[a][i] : a == 6 ? 1.0f : 0.0f;
        B = b < 6 ? srcs[b][i] : 0.0f;
        if (c < 6) C = srcs[c][i];
        else if (c >= 7 && c <= 12) C = srcs[c - 7][3];
        else if (c == 14) C = c8(st->prim_lod_frac);
        else if (c == 13) C = c8(st->lod_frac);
        else C = 0.0f;
        D = d < 6 ? srcs[d][i] : d == 6 ? 1.0f : 0.0f;
        out[i] = (A - B) * C + D;
    }
    {
        float A = aa < 6 ? srcs[aa][3] : aa == 6 ? 1.0f : 0.0f, B = ab < 6 ? srcs[ab][3] : ab == 6 ? 1.0f : 0.0f;
        float D = ad < 6 ? srcs[ad][3] : ad == 6 ? 1.0f : 0.0f, C;
        C = ac == 0 ? c8(st->lod_frac) : ac < 6 ? srcs[ac][3] : ac == 6 ? c8(st->prim_lod_frac) : 0.0f;
        out[3] = (A - B) * C + D;
    }
    for (int i = 0; i < 4; i++) out[i] = out[i] < 0.0f ? 0.0f : out[i] > 1.0f ? 1.0f : out[i];
}
static void raster(const XgfxState* st, const XgfxVtx* v0, const XgfxVtx* v1, const XgfxVtx* v2) {
    float minx = fminf(v0->x, fminf(v1->x, v2->x)), maxx = fmaxf(v0->x, fmaxf(v1->x, v2->x));
    float miny = fminf(v0->y, fminf(v1->y, v2->y)), maxy = fmaxf(v0->y, fmaxf(v1->y, v2->y));
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    float w0 = v0->w > 0 ? 1.0f / v0->w : 1.0f, w1 = v1->w > 0 ? 1.0f / v1->w : 1.0f, w2 = v2->w > 0 ? 1.0f / v2->w : 1.0f;
    int x0, x1, y0, y1;
    if (fabsf(area) < 1e-6f) return;
    x0 = (int)floorf(minx), x1 = (int)ceilf(maxx), y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (x0 < st->scissor[0]) x0 = st->scissor[0];
    if (y0 < st->scissor[1]) y0 = st->scissor[1];
    if (x1 > st->scissor[2]) x1 = st->scissor[2];
    if (y1 > st->scissor[3]) y1 = st->scissor[3];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > FBW) x1 = FBW;
    if (y1 > FBH) y1 = FBH;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float b0 = ((v1->x - px) * (v2->y - py) - (v2->x - px) * (v1->y - py)) / area;
            float b1 = ((v2->x - px) * (v0->y - py) - (v0->x - px) * (v2->y - py)) / area;
            float b2 = 1.0f - b0 - b1, iw, z, t0[4], t1[4], sh[4], comb[4] = { 0, 0, 0, 0 }, o[4];
            int i = y * FBW + x;
            if (b0 < 0 || b1 < 0 || b2 < 0) continue;
            z = b0 * v0->z + b1 * v1->z + b2 * v2->z;
            if (st->depth_func == XGFX_DEPTH_LEQUAL && z > s_zb[i]) continue;
            if (st->depth_func == XGFX_DEPTH_LESS && z >= s_zb[i]) continue;
            iw = b0 * w0 + b1 * w1 + b2 * w2;
#define PERSP(f) ((b0 * v0->f * w0 + b1 * v1->f * w1 + b2 * v2->f * w2) / iw)
            sample(st, 0, PERSP(s0), PERSP(t0), t0);
            sample(st, 1, PERSP(s1), PERSP(t1), t1);
            for (int k = 0; k < 4; k++) sh[k] = (b0 * v0->rgba[k] + b1 * v1->rgba[k] + b2 * v2->rgba[k]) / 255.0f;
            cycle(0, st, t0, t1, sh, comb, o);
            if (st->two_cycle) {
                memcpy(comb, o, sizeof comb);
                cycle(1, st, t0, t1, sh, comb, o);
            }
            if (st->alpha_ref && o[3] * 255.0f < (float)st->alpha_ref) continue;
            if (st->stencil == XGFX_STENCIL_MARK || !st->color_write) {
                if (st->depth_write) s_zb[i] = z;
                continue;
            }
            if (st->fog) {
                float f = PERSP(fog);
                for (int k = 0; k < 3; k++) o[k] = o[k] * (1.0f - f) + c8(st->fog_color[k]) * f;
            }
#undef PERSP
            if (st->blend) {
                uint32_t d = s_fb[i];
                float dc[3] = { (float)(d >> 16 & 255) / 255.0f, (float)(d >> 8 & 255) / 255.0f, (float)(d & 255) / 255.0f };
                for (int k = 0; k < 3; k++) {
                    o[k] = st->blend == XGFX_BLEND_ADD ? o[k] * o[3] + dc[k] : o[k] * o[3] + dc[k] * (1.0f - o[3]);
                    if (o[k] > 1.0f) o[k] = 1.0f;
                }
            }
            s_fb[i] = 0xFF000000u | (uint32_t)(o[0] * 255.0f + 0.5f) << 16 | (uint32_t)(o[1] * 255.0f + 0.5f) << 8 |
                      (uint32_t)(o[2] * 255.0f + 0.5f);
            if (st->depth_write) s_zb[i] = z;
        }
}
static int s_draw_log;
void xgfx_draw(const XgfxState* st, const XgfxVtx* v, uint32_t ntris) {
    s_draws++;
    s_tris += ntris;
    s_sink += (uint32_t)(int)v[0].x + st->wrap_s[0];
    if (!s_render) return;
    if (s_draw_log) {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (uint32_t i = 0; i < ntris * 3; i++) {
            x0 = fminf(x0, v[i].x), x1 = fmaxf(x1, v[i].x), y0 = fminf(y0, v[i].y), y1 = fmaxf(y1, v[i].y);
        }
        printf("[BDRAW] %u: %u tris, x %.0f..%.0f y %.0f..%.0f, cc %06x:%08x%s, prim %02x%02x%02x%02x env %02x%02x%02x%02x, "
               "shade %02x%02x%02x%02x, tex %u %u (%ux%u, st %.2f %.2f, wrap %u %u), blend %u, aref %u, depth %u/%u, cw %u, "
               "sc %d %d %d %d\n",
               (unsigned)s_draws, (unsigned)ntris, x0, x1, y0, y1, (unsigned)st->cc0, (unsigned)st->cc1,
               st->two_cycle ? " 2cyc" : "", st->prim[0], st->prim[1], st->prim[2], st->prim[3], st->env[0], st->env[1],
               st->env[2], st->env[3], v[0].rgba[0], v[0].rgba[1], v[0].rgba[2], v[0].rgba[3], (unsigned)st->tex[0],
               (unsigned)st->tex[1], st->tex[0] < MAXTEX ? (unsigned)s_tex[st->tex[0]].w : 0,
               st->tex[0] < MAXTEX ? (unsigned)s_tex[st->tex[0]].h : 0, v[0].s0, v[0].t0, (unsigned)st->wrap_s[0],
               (unsigned)st->wrap_t[0], (unsigned)st->blend, (unsigned)st->alpha_ref, (unsigned)st->depth_func,
               (unsigned)st->depth_write, (unsigned)st->color_write, (int)st->scissor[0], (int)st->scissor[1],
               (int)st->scissor[2], (int)st->scissor[3]);
    }
    for (uint32_t i = 0; i < ntris; i++) raster(st, &v[i * 3], &v[i * 3 + 1], &v[i * 3 + 2]);
}
void xgfx_clear(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t argb, int color, int depth, float z01) {
    s_clears++;
    if (!s_render) return;
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < FBH; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < FBW; x++) {
            if (color) s_fb[y * FBW + x] = argb;
            if (depth) s_zb[y * FBW + x] = z01 * s_o.zmax;
        }
}
void xgfx_present(void) {}
uint32_t xgfx_tex_create(uint32_t w, uint32_t h, const uint32_t* argb) {
    uint32_t id;
    s_texs++;
    s_sink += argb[0] + w + h;
    id = (uint32_t)(s_texs % (MAXTEX - 1)) + 1;
    if (s_render) {
        free(s_tex[id].px);
        s_tex[id].w = w, s_tex[id].h = h;
        s_tex[id].px = malloc((size_t)w * h * 4);
        memcpy(s_tex[id].px, argb, (size_t)w * h * 4);
    }
#ifdef BENCH_TEX_DUMP   /* -D BENCH_TEX_DUMP=<id>: that texture's texels as made (every 4th of rows 0, 8, 16, 24) */
    if (BENCH_TEX_DUMP < 0 ? w == 32 && h == 32 : id == BENCH_TEX_DUMP)
        for (uint32_t y = 0; y < h; y += 8) {
            printf("[BTEX] %ux%u row %u:", w, h, y);
            for (uint32_t x = 0; x < w; x += 4) printf(" %08x", argb[y * w + x]);
            printf("\n");
        }
#endif
    return id;
}
void xgfx_tex_destroy(uint32_t tex) { (void)tex; }
uint32_t xgfx_capture_last(float* u, float* v) { (void)u; (void)v; return 0; }
uint32_t xgfx_tex_linear_last(float* w, float* h) { (void)w; (void)h; return 0; }
int xgfx_target_hold(int on) { (void)on; return 0; }
uint32_t xgfx_tex_linear_hold(float* w, float* h) { (void)w; (void)h; return 0; }
void xgfx_debug_dump(void) {}
uint32_t xgfx_tex_linear_back(float* w, float* h) { *w = 640.0f; *h = 480.0f; return 0; }
int xgfx_target_stage(int on, int32_t* w, int32_t* h) { (void)on; (void)w; (void)h; return 0; }
const void* xgfx_stage_read(int32_t* pitch, int32_t* bpp) { (void)pitch; (void)bpp; return NULL; }
int xgfx_stage_cpu(const int32_t r[4], float x0, float y0, float x1, float y1) { (void)r; (void)x0; (void)y0; (void)x1; (void)y1; return 0; }
unsigned xu_retrace(void) { return 0; }   /* test-build lines name the retrace (-D XHW_TEST_BUILD=1 traces) */
int xhw_watch_pages(uint32_t va, uint32_t bytes, int arm) { (void)va; (void)bytes; (void)arm; return 0; }
static int s_quiet;
void xhw_log(const char* s) { if (!s_quiet) printf("%s\n", s); }
void xhw_logf(const char* fmt, ...) {
    va_list ap;
    if (s_quiet) return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
uint64_t xhw_time_us(void) { return 0; }
void xhw_perf_add(int what, uint64_t us) { (void)what; (void)us; }
void xhw_perf_phase(int dl) { (void)dl; }
uint64_t xhw_perf_log_us(void) { return 0; }
int xhw_perf_detail(void) { return 0; }
void xhw_panel_render(uint32_t* img, int pitch_px, const xhw_panel* p) { (void)img; (void)pitch_px; (void)p; }
int wx_game_state(void) { return -1; }
void wx_frame_overlay(void) {}

static size_t load(const char* path, void* dst, size_t max) {
    FILE* f = fopen(path, "rb");
    size_t n;
    if (!f) {
        fprintf(stderr, "bench_dl: cannot open %s\n", path);
        exit(2);
    }
    n = fread(dst, 1, max, f);
    fclose(f);
    return n;
}

/* ---- the sampler (Windows): the main thread's pc every millisecond ---- */
#ifdef _WIN32
#define MAXS 2000000
static uint64_t* s_samples;
static volatile LONG s_nsamp, s_stop;
static HANDLE s_main;
static DWORD WINAPI sampler(LPVOID arg) {
    (void)arg;
    timeBeginPeriod(1);
    while (!s_stop) {
        CONTEXT c;
        Sleep(1);
        if (SuspendThread(s_main) == (DWORD)-1) continue;
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(s_main, &c) && s_nsamp < MAXS) s_samples[s_nsamp++] = (uint64_t)c.Rip;
        ResumeThread(s_main);
    }
    return 0;
}
#endif

static double now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    return 0.0;
#endif
}

#define MAXT 16
static uint32_t s_task[MAXT][17];
static uint8_t* s_rams[MAXT];

/* every task in order; the first run each over its own RDRAM image, the timed ones over the last */
static void run_all(unsigned nt, int first) {
    for (unsigned k = 0; k < nt; k++) {
        uint32_t dl = s_task[k][TK_DATA_PTR_W];
        if (first) memcpy(s_ram, s_rams[k], xrc_rdram_size);
        f3d_set_ucode(gfx_ucode_for_task(s_task[k][TK_UCODE_DATA_W], s_task[k][TK_UCODE_DATA_SIZE_W], dl));
#ifdef BENCH_PRESENT_EACH   /* bench_dl.py --trace: each task its own frame, so XGFX_TRACE_FRAME can pick one */
        f3d_run(dl, 1);
#else
        f3d_run(dl, k + 1 == nt);
#endif
    }
}

int main(int argc, char** argv) {
    uint32_t i, runs;
    unsigned nt, k;
    size_t n;
    double t0, t1;
    char path[1024];
    if (argc < 4) {
        fprintf(stderr, "usage: bench_dl <dir> <tasks> <runs> [samples.txt [picture.ppm [draws]]]\n");
        return 2;
    }
    nt = (unsigned)atoi(argv[2]);
    if (nt < 1 || nt > MAXT) return 2;
    for (k = 0; k < nt; k++) {
        snprintf(path, sizeof path, "%s/task%u.bin", argv[1], k);
        if (load(path, s_task[k], sizeof s_task[k]) != sizeof s_task[k]) {
            fprintf(stderr, "bench_dl: %s is not 68 bytes\n", path);
            return 2;
        }
        s_rams[k] = malloc(RAM_MAX);
        snprintf(path, sizeof path, "%s/ram%u.bin", argv[1], k);
        n = load(path, s_rams[k], RAM_MAX);
        if (n != s_task[k][16] || (n & (n - 1))) {
            fprintf(stderr, "bench_dl: RDRAM %u bytes, the task says %u\n", (unsigned)n, (unsigned)s_task[k][16]);
            return 2;
        }
        xrc_rdram_size = (uint32_t)n;
        printf("[BENCH] task %u: list %08x\n", k, (unsigned)s_task[k][TK_DATA_PTR_W]);
    }
    runs = (uint32_t)atoi(argv[3]);
    /* the first run logs what the interpreter says, fills the texture cache
     * and draws the picture */
    s_render = 1;
    s_draw_log = argc > 6;
    run_all(nt, 1);
    printf("[BENCH] first run: %llu draws, %llu triangles, %llu textures, %llu clears\n", s_draws, s_tris, s_texs, s_clears);
    if (argc > 5) {
        FILE* f = fopen(argv[5], "wb");
        if (f) {
            fprintf(f, "P6 %d %d 255\n", FBW, FBH);
            for (i = 0; i < FBW * FBH; i++) {
                unsigned char rgb[3] = { (unsigned char)(s_fb[i] >> 16), (unsigned char)(s_fb[i] >> 8), (unsigned char)s_fb[i] };
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
    }
    s_render = 0;
    s_quiet = 1;
    s_draws = s_tris = s_texs = s_clears = 0;
#ifdef _WIN32
    if (argc > 4) {
        s_samples = malloc(sizeof(uint64_t) * MAXS);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_main, 0, FALSE, DUPLICATE_SAME_ACCESS);
        CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    }
#endif
    t0 = now_ms();
    for (i = 0; i < runs; i++) run_all(nt, 0);
    t1 = now_ms();
#ifdef _WIN32
    if (argc > 4) {
        FILE* f;
        s_stop = 1;
        Sleep(20);
        f = fopen(argv[4], "w");
        if (f) {
            fprintf(f, "base %llx\n", (unsigned long long)(uintptr_t)GetModuleHandle(NULL));
            for (LONG k = 0; k < s_nsamp; k++) fprintf(f, "%llx\n", (unsigned long long)s_samples[k]);
            fclose(f);
        }
    }
#endif
    printf("[BENCH] %u runs of %u tasks: %.3f ms a run; per run %llu draws, %llu triangles, %llu textures made\n", runs,
           nt, (t1 - t0) / runs, s_draws / runs, s_tris / runs, s_texs / runs);
    return 0;
}
