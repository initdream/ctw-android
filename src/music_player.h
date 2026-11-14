#ifndef __CTW_MUSIC_PLAYER_H__
#define __CTW_MUSIC_PLAYER_H__

#ifdef __cplusplus
extern "C" {
#endif

int music_playlist_open(const char *name);

int music_playlist_count(void);

int music_playlist_is_playing(void);

void music_playlist_play(void);
void music_playlist_pause(void);
void music_playlist_stop(void);

void music_tick(void);

void music_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
