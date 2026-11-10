/* Virtual Pro-Wrestling 2 (NA2J, Japan; an English translation's image): the runtime's per-ROM settings (MIT). Its own functions all run as recompiled.
 *
 * An AKI wrestling game, as WWF No Mercy (NW4E.3566d9b57232.c says more):
 * - its graphics thread sends each frame as several tasks and waits for each one's done: the runtime's hold of a graphics
 *   task's done (ultra/sp.c, XRC_RCP_SENDER_US) came each time. DIVERGES: the done comes as soon as the task is over;
 * - it culls what it draws to its own 4:3 view, so hor+ drew only its ropes and turnbuckle pads into the bars
 *   (f3d_interp.c, xrc_game_horplus). DIVERGES: none; 16:9 shows the 4:3 picture between bars. */
int xrc_game_rcp_sender_us(void) { return 0; }
int xrc_game_horplus(void) { return 0; }
