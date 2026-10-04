#ifndef PLAYER_H
#define PLAYER_H
#include <vita2d.h>

/* Starts playing a URL (http/https/rtmp-less: whatever sceAvPlayer accepts). 0 = ok. */
int  player_start(const char *url);
void player_stop(void);

/* Call once per frame. Returns the texture with the latest video frame,
 * or NULL if no frame has arrived yet. */
vita2d_texture *player_poll(void);

void player_shutdown(void);
#endif
