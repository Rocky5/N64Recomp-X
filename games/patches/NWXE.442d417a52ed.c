/* WWF WrestleMania 2000 (NWXE, USA): the runtime's per-ROM settings (MIT). Its own functions all run as recompiled.
 *
 * An AKI wrestling game, as WWF No Mercy (NW4E.3566d9b57232.c says more):
 * - its graphics thread sends each frame as several tasks and waits for each one's done: the runtime's hold of a graphics
 *   task's done (ultra/sp.c, XRC_RCP_SENDER_US) came each time. DIVERGES: the done comes as soon as the task is over;
 * - it culls what it draws to its own 4:3 view, so hor+ drew only its ropes and turnbuckle pads into the bars
 *   (f3d_interp.c, xrc_game_horplus). DIVERGES: none; 16:9 shows the 4:3 picture between bars. */
int xrc_game_rcp_sender_us(void) { return 0; }
int xrc_game_horplus(void) { return 0; }

/* n64rx: ovl_jr
 * Its match overlay (ROM 0xD2720 at 0x800E1B90, 893 functions) has a few `jr` with no table: the analyser left it out, and the
 * game crashed entering a match. That marker keeps it and interprets those functions (analyze/segments.py, N64RX_OVL_JR). */
