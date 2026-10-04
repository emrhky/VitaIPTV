#ifndef PLAYER_H
#define PLAYER_H
#include <vita2d.h>

/* Starts playing a URL or a file path (ux0:...). 0 = ok, otherwise a negative error code. */
int  player_start(const char *url);
void player_stop(void);

/* Call once per frame. Returns the texture with the latest video frame,
 * or NULL if no frame has arrived yet. */
vita2d_texture *player_poll(void);

void player_shutdown(void);

/* Empties ux0:data/VitaIPTV/log.txt (call once at startup). */
void player_log_reset(void);
#endif
