/* video.c - the runtime's half of the video-mode probation (PLAN.md 5.5,
 * Wings64-X's pad.c): a mode not yet confirmed on this console shows
 * "New video mode 720p. Press A to keep it." (xhw_video.c draws it); A on
 * any port keeps it (settings.ini's video_confirmed), and that press does
 * not reach the game until A is let go. With no press in time the XBE
 * restarts and the next boot runs 480i (xhw_video.c's video_pending.txt).
 *
 * A is looked for at every controller read the game makes (ultra/si.c) and
 * from the VI thread every 8 retraces, so a game that reads nothing while
 * the prompt is up (the intro, a level load) or is stalled can't keep a
 * mode nobody can see; the timeout is checked on the VI thread too. */
#include <stdio.h>

#include "settings.h"
#include "ultra/ultra.h"

static volatile int s_confirm_hold;   /* ports whose A kept the mode, until let go */
static volatile long s_reverting;

static void confirm(int port) {
    if (!xhw_video_probation()) return;
    xhw_video_confirm();
    snprintf(g_xrc_settings.video_confirmed, sizeof g_xrc_settings.video_confirmed, "%s", xhw_video_mode_name());
    xhw_logf("[VIDEO] %s kept (A on port %d)", xhw_video_mode_name(), port + 1);
    xrc_settings_save();
}

/* a raw pad state (the caller holds the pad lock): 1 when A must not reach the game */
int xrc_video_pad(int port, const xhw_pad* p) {
    int bit = 1 << port;
    if ((p->buttons & XHW_BTN_A) && xhw_video_probation()) {
        confirm(port);
        s_confirm_hold |= bit;
    }
    if (!(p->buttons & XHW_BTN_A)) s_confirm_hold &= ~bit;
    return (s_confirm_hold & bit) != 0;
}

/* the VI thread, every retrace */
void xrc_video_vi(unsigned retrace) {
    int i;
    if (xhw_video_probation_expired() && __sync_bool_compare_and_swap(&s_reverting, 0, 1)) {
        xhw_logf("[VIDEO] %s not confirmed in time: restarting in 480i", xhw_video_mode_name());
        xhw_exit_reason("video mode not confirmed: restarted in 480i");
        xhw_pad_lock();
        xhw_reboot_self();
    }
    if (!xhw_video_probation() || (retrace & 7) || !xhw_pad_trylock()) return;
    xhw_pad_poll();
    for (i = 0; i < 4; i++) {
        xhw_pad p;
        if (xhw_pad_get(i, &p) && p.connected) xrc_video_pad(i, &p);
    }
    xhw_pad_unlock();
}
