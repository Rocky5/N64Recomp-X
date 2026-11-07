/* test_gfx_swap.c - when a swap's present runs (xbox/src/rt/gfx.c, XRC_SWAP_AT_BLOCK), on the host (tools/xbox/test_gfx_swap.py).
 *
 * The real gfx.c over a fake interpreter that records the order of its calls. A present can sleep up to a vblank; on the N64 a
 * swap only writes a register, so the thread that swaps must not sleep there (Snowboard Kids). Each case says what runs when. */
#include <stdio.h>
#include <string.h>

#include "f3d_interp.h"
#include "settings.h"
#include "ultra/ultra.h"
#include "xgfx.h"

static char s_log[256];
static unsigned s_retrace;
static int s_open;
static int s_fail;

#define CHECK(c, ...) do { if (!(c)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void note(const char* w) { strncat(s_log, w, sizeof s_log - strlen(s_log) - 1); }

/* the interpreter */
int f3d_swap(uint32_t fb) {
    char b[32];
    snprintf(b, sizeof b, "swap:%x ", (unsigned)fb);
    note(b);
    s_open = 0;
    return 1;
}
void f3d_run(uint32_t dl, int present) { (void)dl; (void)present; note("task "); s_open = 1; }
void f3d_run_rdp(uint32_t list, uint32_t bytes, int present) { (void)list; (void)bytes; (void)present; note("rdp "); s_open = 1; }
void f3d_present_frame(void) { note("stale "); s_open = 0; }
void f3d_fb_inval(uint32_t addr, uint32_t len) { (void)addr; (void)len; note("inval "); }
int f3d_frame_open(void) { return s_open; }
void f3d_split_frames(int on) { (void)on; }
unsigned f3d_presents(void) { return 0; }
int f3d_last_fullsync(void) { return 0; }
unsigned f3d_task_no(void) { return 0; }
void f3d_set_ucode(const GfxUcode* uc) { (void)uc; }
void f3d_set_widescreen(int on) { (void)on; }
void f3d_probe_restore(unsigned task) { (void)task; }

/* the runtime and the platform */
unsigned xu_retrace(void) { return s_retrace; }
int xu_lock_drop(void) { return 0; }
void xu_lock_take(int n) { (void)n; }
int xu_event_set(int e) { (void)e; return 0; }
int xrc_menu_open(void) { return 0; }
void xhw_logf(const char* f, ...) { (void)f; }
void xhw_sleep_ms(unsigned ms) { (void)ms; }
uint64_t xhw_time_us(void) { return 0; }
xhw_mutex* xhw_mutex_create(void) { return NULL; }
void xhw_mutex_lock(xhw_mutex* m) { (void)m; }
void xhw_mutex_unlock(xhw_mutex* m) { (void)m; }
int xgfx_init(void) { return 0; }
void xgfx_set_fps_overlay(int on) { (void)on; }
void xgfx_set_tex_filter(int f) { (void)f; }
const xhw_video_mode* xhw_video(void) { static xhw_video_mode v; return &v; }
int xhw_video_widescreen_set(void) { return 0; }
XrcSettings g_xrc_settings;
uint32_t xrc_rdram_size = 0x10000;
static uint8_t s_ram[0x10000];
uint8_t* xrc_rdram_host = s_ram;
uint32_t xrc_tlb_to_phys(uint32_t v) { return v; }
const GfxUcode* gfx_ucode_for_task(uint32_t data, uint32_t size, uint32_t dl) { (void)data; (void)size; (void)dl; return NULL; }

void xrc_gfx_swap(uint32_t fb);
void xrc_gfx_swap_block(void);
void xrc_gfx_task(uint32_t task);
void xrc_gfx_vi(unsigned retrace);
void xrc_gfx_inval(uint32_t addr, uint32_t len);
void xrc_gfx_dp_list(uint32_t list, uint32_t bytes);
void xrc_gfx_boot(void);

#define EXPECT(want) do { CHECK(!strcmp(s_log, want), "got \"%s\", want \"%s\"", s_log, want); s_log[0] = 0; } while (0)

int main(void) {
    xrc_gfx_boot();
#if XRC_SWAP_AT_BLOCK
    /* the swap itself does not present; a thread about to block does */
    xrc_gfx_swap(0x100);
    EXPECT("");
    xrc_gfx_swap_block();
    EXPECT("swap:100 ");
    xrc_gfx_swap_block();
    EXPECT("");
    /* the next task presents first, on its own thread */
    xrc_gfx_swap(0x200);
    xrc_gfx_task(0);
    EXPECT("swap:200 task ");
    /* a retrace: not the one it was swapped in */
    s_open = 0;
    xrc_gfx_swap(0x300);
    xrc_gfx_vi(s_retrace);
    EXPECT("");
    s_retrace++;
    xrc_gfx_vi(s_retrace);
    EXPECT("swap:300 ");
    /* two swaps with nothing between: both, in order */
    xrc_gfx_swap(0x400);
    xrc_gfx_swap(0x500);
    EXPECT("swap:400 swap:500 ");
    /* a read of a frame by the CPU sees the swap done */
    xrc_gfx_swap(0x600);
    xrc_gfx_inval(0, 4);
    EXPECT("swap:600 inval ");
    /* an RDP list is a task like any other */
    xrc_gfx_swap(0x700);
    xrc_gfx_dp_list(0, 8);
    EXPECT("swap:700 rdp ");
#else
    xrc_gfx_swap(0x100);
    EXPECT("swap:100 ");
    xrc_gfx_swap_block();
    EXPECT("");
#endif
    printf("test_gfx_swap (XRC_SWAP_AT_BLOCK %d): %d failed\n", XRC_SWAP_AT_BLOCK, s_fail);
    return s_fail != 0;
}
