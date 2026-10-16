/* fbfx.c - framebuffer effects' bookkeeping (fbfx.h). */
#include "fbfx.h"

#include <string.h>

static FbfxSlot s_slot[FBFX_SLOTS];

void fbfx_reset(void) { memset(s_slot, 0, sizeof s_slot); }

const FbfxSlot* fbfx_slot(int i) { return i >= 0 && i < FBFX_SLOTS ? &s_slot[i] : 0; }

void fbfx_mark(int i, int stale) {
    if (i >= 0 && i < FBFX_SLOTS) s_slot[i].stale = stale;
}

int fbfx_stale_count(void) {
    int i, n = 0;
    for (i = 0; i < FBFX_SLOTS; i++) n += s_slot[i].addr && s_slot[i].stale;
    return n;
}

static int overlaps(const FbfxSlot* s, uint32_t addr, uint32_t bytes) {
    return s->addr && addr < s->addr + fbfx_bytes(s) && s->addr < addr + bytes;
}

int fbfx_frame_end(uint32_t addr, uint32_t w, uint32_t h, uint32_t siz, uint32_t frame, uint32_t ram_size,
                   FbfxSlot drop[FBFX_SLOTS], int* ndrop) {
    FbfxSlot n;
    int i, at = -1;
    *ndrop = 0;
    n.addr = addr & 0x1FFFFFFFu;
    n.w = w, n.h = h, n.siz = siz, n.frame = frame, n.stale = 0, n.hot = 0;
    /* the RDP's colour images: 16- or 32-bit, a sensible size, 8-byte aligned, inside RDRAM */
    if (!n.addr || (n.addr & 7u) || (siz != 2 && siz != 3) || w < 8 || w > 1024 || h < 8 || h > 1024 ||
        n.addr + fbfx_bytes(&n) > ram_size)
        return -1;
    for (i = 0; i < FBFX_SLOTS; i++)   /* the same image again */
        if (s_slot[i].addr == n.addr && s_slot[i].w == w && s_slot[i].h == h && s_slot[i].siz == siz) at = i;
    for (i = 0; i < FBFX_SLOTS; i++)   /* anything else on its bytes is gone */
        if (i != at && overlaps(&s_slot[i], n.addr, fbfx_bytes(&n))) {
            drop[(*ndrop)++] = s_slot[i];
            s_slot[i].addr = 0;
        }
    if (at < 0) {
        for (i = 0; i < FBFX_SLOTS && at < 0; i++)
            if (!s_slot[i].addr) at = i;
        if (at < 0) {   /* the one drawn into longest ago */
            at = 0;
            for (i = 1; i < FBFX_SLOTS; i++)
                if (frame - s_slot[i].frame > frame - s_slot[at].frame) at = i;   /* wrap-safe */
            drop[(*ndrop)++] = s_slot[at];
        }
    }
    if (s_slot[at].addr == n.addr) n.stale = s_slot[at].stale, n.hot = s_slot[at].hot;
    s_slot[at] = n;
    return at;
}

int fbfx_frame_start(int i, int touched) {
    FbfxSlot* s;
    if (i < 0 || i >= FBFX_SLOTS || !s_slot[i].addr) return 0;
    s = &s_slot[i];
    if (touched) s->hot = FBFX_HOT;
    else if (s->hot > 0) s->hot--;
    return s->hot > 0 && s->stale;
}

int fbfx_find(uint32_t addr, uint32_t bytes) {
    int i;
    addr &= 0x1FFFFFFFu;
    for (i = 0; i < FBFX_SLOTS; i++)
        if (s_slot[i].stale && overlaps(&s_slot[i], addr, bytes)) return i;
    return -1;
}

void fbfx_rect(int i, int32_t stage_w, int32_t stage_h, int32_t r[4]) {
    const FbfxSlot* s = &s_slot[i];
    int32_t qw = stage_w / 2, qh = stage_h / 2, k = 1;
    while ((int32_t)s->w > qw * k || (int32_t)s->h > qh * k) k++;
    r[0] = (i & 1) * qw;
    r[1] = (i >> 1) * qh;
    r[2] = ((int32_t)s->w + k - 1) / k;
    r[3] = ((int32_t)s->h + k - 1) / k;
}

void fbfx_convert(const FbfxSlot* s, const int32_t r[4], const uint8_t* stage, int32_t pitch, int32_t bpp, uint8_t* ram,
                  uint32_t ram_size) {
    static uint16_t xs[1024];   /* the stage column of each image column: no division per pixel */
    uint32_t x, y, bpp_n = s->siz == 3 ? 4u : 2u;
    if (!s->addr || r[2] <= 0 || r[3] <= 0 || s->w > 1024 || s->addr + fbfx_bytes(s) > ram_size) return;
    for (x = 0; x < s->w; x++) xs[x] = (uint16_t)((uint32_t)r[0] + x * (uint32_t)r[2] / s->w);
    for (y = 0; y < s->h; y++) {
        const uint8_t* row = stage + (size_t)(r[1] + (int32_t)(y * (uint32_t)r[3] / s->h)) * (size_t)pitch;
        uint8_t* out = ram + s->addr + y * s->w * bpp_n;
        if (bpp == 16 && s->siz == 2) {   /* R5G6B5 -> RGBA5551: green's low bit goes */
            const uint16_t* in = (const uint16_t*)row;
            for (x = 0; x < s->w; x++) {
                uint32_t c = in[xs[x]];
                *(uint16_t*)(out + ((x * 2u) ^ 2u)) = (uint16_t)((c & 0xFFC0u) | (c & 0x1Fu) << 1 | 1u);
            }
        } else if (bpp == 16) {
            const uint16_t* in = (const uint16_t*)row;
            for (x = 0; x < s->w; x++) {
                uint32_t c = in[xs[x]];
                ((uint32_t*)out)[x] = (c >> 11) << 27 | ((c >> 5) & 63) << 18 | (c & 31) << 11 | 0xFFu;
            }
        } else if (s->siz == 2) {   /* X8R8G8B8 -> RGBA5551 */
            const uint32_t* in = (const uint32_t*)row;
            for (x = 0; x < s->w; x++) {
                uint32_t c = in[xs[x]];
                *(uint16_t*)(out + ((x * 2u) ^ 2u)) =
                    (uint16_t)(((c >> 8) & 0xF800u) | ((c >> 5) & 0x07C0u) | ((c >> 2) & 0x003Eu) | 1u);
            }
        } else {
            const uint32_t* in = (const uint32_t*)row;
            for (x = 0; x < s->w; x++) ((uint32_t*)out)[x] = in[xs[x]] << 8 | 0xFFu;
        }
    }
}
