/* WWF No Mercy (NW4E, USA, v1.0): the runtime's per-ROM settings (MIT). Its own functions all run as recompiled.
 *
 * Its graphics thread (0x80003138) sends each frame as about six tasks and waits for each one's done before the next.
 * The runtime holds a graphics task's done 2 ms (ultra/sp.c, XRC_RCP_SENDER_US: Hydro Thunder and Mortal Kombat 4 set
 * flags after the send), so the hold came six times a frame and a 4-wrestler match ran at 20 to 29 fps with the CPU half
 * idle. Nothing here waits on the RCP's time.
 * DIVERGES: the done comes as soon as the task is over (the N64's RDP takes its time). */
int xrc_game_rcp_sender_us(void) { return 0; }

/* It culls what it draws to its own 4:3 view, so hor+ drew only its ropes and turnbuckle pads into the bars, over black
 * (f3d_interp.c, xrc_game_horplus). 16:9 shows the 4:3 picture between bars. */
int xrc_game_horplus(void) { return 0; }
