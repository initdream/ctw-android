#ifndef __MOVIE_PLAYER_H__
#define __MOVIE_PLAYER_H__


int movie_play(const char *path, int skippable);
void movie_stop(void);
void movie_skip(void);
int movie_is_playing(void);
void movie_set_text(const char *text);
void movie_set_text_scale(int scale);
unsigned int eglSwapBuffersHook(void *display, void *surface);
void movie_main_loop_tick(void);

#endif
