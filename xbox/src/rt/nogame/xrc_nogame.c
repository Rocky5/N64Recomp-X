/* xrc_nogame.c - the game descriptor of a build without a game (the platform
 * and the runtime alone, PLAN.md 6 M0): the loading screen ends on the "No
 * game built in" card. n64rx's builds compile their own xrc_game.c instead
 * (tools/xbox/compile_game.py picks one of the two). */
#include "xrc_game.h"

const XrcGame xrc_game = { 0 };
