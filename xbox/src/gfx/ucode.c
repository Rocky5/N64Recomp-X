/* ucode.c - the graphics microcode registry (ucode.h, docs/renderer.md). */
#include <string.h>

#include "ultra/ultra.h"
#include "ucode.h"

static const uint8_t* find(const uint8_t* p, size_t n, const char* s) {
    size_t k = strlen(s), i;
    for (i = 0; i + k <= n; i++)
        if (p[i] == (uint8_t)s[0] && !memcmp(p + i, s, k)) return p + i;
    return NULL;
}

static int is_digit(uint8_t c) { return c >= '0' && c <= '9'; }

int gfx_ucode_parse(const uint8_t* data, size_t n, GfxUcode* out) {
    const uint8_t *p, *end = data + n, *name, *q;
    size_t nl, i;
    int major = -1;
    memset(out, 0, sizeof *out);
    out->family = GFX_UC_F3D;
    out->vtx_max = 16;
    if ((p = find(data, n, "RSP Gfx ucode ")) != NULL) {
        p += 14;
        while (p < end && *p == ' ') p++;
        name = p;
        while (p < end && *p > ' ' && *p < 0x7F) p++;
        nl = (size_t)(p - name);
        /* the version: the first d.dd after the name ("fifo 1.23", "xbus 2.08J", "0.95") */
        for (q = p; q + 2 < end && *q && q < p + 24; q++)
            if (is_digit(q[0]) && q[1] == '.' && is_digit(q[2]) && !is_digit(q[-1])) { major = q[0] - '0'; break; }
        if (nl >= 5 && !memcmp(name, "S2DEX", 5)) {
            out->family = major >= 2 ? GFX_UC_S2DEX2 : GFX_UC_S2DEX;
            out->vtx_max = 32;
        } else if (find(name, nl, "3DEX") || find(name, nl, "3DLX") || find(name, nl, "3DLP") || find(name, nl, "3DZEX")) {
            out->family = major >= 2 ? GFX_UC_F3DEX2 : GFX_UC_F3DEX;
            out->vtx_max = find(name, nl, "3DLP") ? 80 : find(name, nl, ".Rej") ? 64 : 32;
            if (major == 0) out->flags |= GFX_UC_F_OLD;
            if (name[0] == 'L') out->flags |= GFX_UC_F_LINE;
        } else {
            return 0;   /* an unknown graphics microcode: Fast3D's GBI is the best guess */
        }
        out->found = 1;
        p = name - 14;
    } else if ((p = find(data, n, "RSP SW Version: ")) != NULL) {
        out->found = 1;
    } else {
        return 0;
    }
    for (i = 0; i + 1 < sizeof out->name && p + i < end && p[i] >= ' ' && p[i] < 0x7F; i++) out->name[i] = (char)p[i];
    out->name[i] = 0;
    return 1;
}

#define UC_SLOTS 8
static struct {
    uint32_t addr;
    GfxUcode uc;
} s_uc[UC_SLOTS];
static unsigned s_nuc;

static const char* const s_family[GFX_UC_FAMILIES] = { "Fast3D", "F3DEX", "F3DEX2", "S2DEX", "S2DEX2" };

const GfxUcode* gfx_ucode_at(uint32_t data_addr, uint32_t data_size) {
    uint8_t buf[0x1000];
    GfxUcode uc;
    unsigned i, n;
    data_addr &= 0x1FFFFFFFu;
    for (i = 0; i < s_nuc; i++)
        if (s_uc[i].addr == data_addr) return &s_uc[i].uc;
    n = data_size && data_size < sizeof buf ? data_size : sizeof buf;
    for (i = 0; i < n; i++) buf[i] = rd8(data_addr + i);
    gfx_ucode_parse(buf, n, &uc);
    /* the kill switches: that family read as Fast3D, as before the registry */
    if ((!XGFX_UCODE_F3DEX && uc.family == GFX_UC_F3DEX) || (!XGFX_UCODE_F3DEX2 && uc.family == GFX_UC_F3DEX2) ||
        (!XGFX_UCODE_S2DEX && (uc.family == GFX_UC_S2DEX || uc.family == GFX_UC_S2DEX2))) {
        uc.family = GFX_UC_F3D;
        uc.vtx_max = 16;
        uc.flags = 0;
    }
    xhw_logf("[GFX] ucode %s (data %08x): %s, %u vertices%s%s", uc.found ? uc.name : "(no version text)",
             (unsigned)data_addr, s_family[uc.family], uc.vtx_max, uc.flags & GFX_UC_F_OLD ? ", old" : "",
             uc.flags & GFX_UC_F_LINE ? ", lines" : "");
    i = s_nuc < UC_SLOTS ? s_nuc++ : UC_SLOTS - 1;
    s_uc[i].addr = data_addr;
    s_uc[i].uc = uc;
    return &s_uc[i].uc;
}
