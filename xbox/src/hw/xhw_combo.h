/* xhw_combo.h - the exit-combo detector's pure core (xhw_combo.c), the part
 * tests/xbox/test_pad_combo.c drives on the host with its own clock. Plain
 * C, no kernel headers. */
#ifndef XHW_COMBO_H
#define XHW_COMBO_H
#include <stdint.h>

#include "xhw.h"

/* one pad state at time now_ms: returns 1 when the pad shows a full combo or
 * a combo is pending (the reader then gives the game neutral input). Fires
 * at most once: the first full combo is the one carried out. */
int xhw_combo_feed(int port, const xhw_pad* p, uint32_t now_ms);
/* XHW_COMBO_NONE, _DASHBOARD or _RESTART, and the port that asked */
int xhw_combo_request(int* port);
/* a pending combo may be carried out now: its buttons are up on every port,
 * or it has waited 3 s for that */
int xhw_combo_due(uint32_t now_ms);
/* the VI thread should look this retrace: the game has not read for
 * XHW_COMBO_VI_RETRACES and the retrace is on the look's 8-retrace grid */
int xhw_combo_vi_due(unsigned retrace);
/* the watchdog should look: nothing has fed the detector for
 * XHW_COMBO_WATCHDOG_MS */
int xhw_combo_watchdog_due(uint32_t now_ms);
/* the time of the latest look, from any detection point */
void xhw_combo_note_look(uint32_t now_ms);
/* host tests: everything back to boot state */
void xhw_combo_reset(void);

#endif
