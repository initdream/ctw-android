#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <mpg123.h>

#include "music_player.h"
#include "util.h"

#define MAX_TRACKS       256
#define NBUF             8      /* OpenAL buffers in the ring */
#define DECODE_TARGET    65536  /* bytes of PCM per buffer (~0.37s @ 44.1k stereo) */
#define PCM_CAP          262144 /* scratch buffer for one buffer's worth */
#define SEARCH_MAX_DEPTH 4      /* how deep to scan the GTA folder for mp3s */

static char g_tracks[MAX_TRACKS][1024];
static int  g_track_count = 0;
static int  g_opened = 0;
static int  g_playing = 0;
static int  g_current = -1;     /* index of the track being decoded */
static int  g_track_open = 0;   /* 1 if an MP3 file stream is actively open on g_mh */

static mpg123_handle *g_mh = NULL;
static int  g_mpg_ok = 0;
static unsigned char *g_pcm = NULL;

static ALCdevice  *g_dev = NULL;
static ALCcontext *g_ctx = NULL;
static ALCcontext *g_saved = NULL;
static ALuint g_src = 0;
static ALuint g_bufs[NBUF];
static ALuint g_fmt = 0;
static long   g_rate = 0;

static int openal_setup(void) {
  g_dev = alcOpenDevice(NULL);
  fprintf(stderr, "[music-dbg] alcOpenDevice -> %p\n", (void *)g_dev);
  if (!g_dev) {
    debugPrintf("[music] alcOpenDevice failed\n");
    return -1;
  }
  g_ctx = alcCreateContext(g_dev, NULL);
  fprintf(stderr, "[music-dbg] alcCreateContext -> %p\n", (void *)g_ctx);
  if (!g_ctx) {
    debugPrintf("[music] alcCreateContext failed\n");
    alcCloseDevice(g_dev);
    g_dev = NULL;
    return -1;
  }
  return 0;
}

static void al_begin(void) {
  g_saved = alcGetCurrentContext();
  alcMakeContextCurrent(g_ctx);
}

static void al_end(void) {
  alcMakeContextCurrent(g_saved);
}

static int has_mp3_ext(const char *name) {
  size_t n = strlen(name);
  if (n < 5)
    return 0;
  return strcasecmp(name + n - 4, ".mp3") == 0;
}

static int track_cmp(const void *a, const void *b) {
  return strcasecmp((const char *)a, (const char *)b);
}

static void scan_mp3(const char *dir, int depth) {
  if (g_track_count >= MAX_TRACKS || depth > SEARCH_MAX_DEPTH)
    return;
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  struct stat st;
  char full[1280];
  while (g_track_count < MAX_TRACKS && (e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
    if (stat(full, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode)) {
      scan_mp3(full, depth + 1);
      continue;
    }
    if (!S_ISREG(st.st_mode) || !has_mp3_ext(e->d_name))
      continue;

    snprintf(g_tracks[g_track_count], sizeof(g_tracks[g_track_count]), "%s", full);
    g_track_count++;
  }
  closedir(d);
}

static int discover_tracks(void) {
  g_track_count = 0;

  const char *custom = getenv("CTW_MUSIC_DIR");
  if (custom && custom[0])
    scan_mp3(custom, 0);

  scan_mp3("./GTA", 0);
  scan_mp3("GTA", 0);

  for (int i = 0; i < g_track_count; i++) {
    for (int j = i + 1; j < g_track_count; j++) {
      if (!strcmp(g_tracks[i], g_tracks[j])) {
        memmove(&g_tracks[j], &g_tracks[j + 1],
                (size_t)(g_track_count - j - 1) * sizeof(g_tracks[0]));
        g_track_count--;
        j--;
      }
    }
  }

  qsort(g_tracks, g_track_count, sizeof(g_tracks[0]), track_cmp);
  return g_track_count;
}

static int open_track(int idx) {
  if (idx < 0 || idx >= g_track_count || !g_mh)
    return -1;

  mpg123_close(g_mh);
  g_track_open = 0;

  int err = mpg123_open(g_mh, g_tracks[idx]);
  fprintf(stderr, "[music-dbg] mpg123_open('%s') -> err=%d (%s)\n",
          g_tracks[idx], err, mpg123_plain_strerror(err));
  if (err != MPG123_OK)
    return -1;

  long r; int ch, enc;
  if (mpg123_getformat(g_mh, &r, &ch, &enc) != MPG123_OK) {
    fprintf(stderr, "[music-dbg] mpg123_getformat failed\n");
    mpg123_close(g_mh);
    return -1;
  }
  mpg123_format_none(g_mh);
  mpg123_format(g_mh, r, ch, MPG123_ENC_SIGNED_16);
  g_rate = r;
  g_fmt  = (ALenum)((ch >= 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16);
  g_track_open = 1;

  fprintf(stderr, "[music-dbg] track fmt: rate=%ld ch=%d fmt=0x%x\n", r, ch, g_fmt);
  return 0;
}

static int open_next_track(void) {
  int tries = 0;
  while (tries < g_track_count) {
    g_current = (g_current + 1) % g_track_count;
    tries++;
    if (open_track(g_current) == 0)
      return 0;
  }
  return -1;
}

static int refill_buffer(ALuint albuf) {
  size_t total = 0;
  int tries = 0;

  while (total < DECODE_TARGET) {
    if (!g_track_open) {
      if (open_next_track() != 0) {
        if (total > 0)
          break;
        fprintf(stderr, "[music-dbg] refill_buffer: open_next_track failed\n");
        return 0;
      }
    }
    size_t got = 0;
    int err = mpg123_read(g_mh, g_pcm + total, (size_t)(PCM_CAP - total), &got);
    if (err == MPG123_OK && got > 0) {
      total += got;
    } else if (err == MPG123_NEW_FORMAT) {
      long r; int ch, enc;
      if (mpg123_getformat(g_mh, &r, &ch, &enc) == MPG123_OK) {
        g_rate = r;
        g_fmt  = (ALenum)((ch >= 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16);
      }
    } else {
      fprintf(stderr, "[music-dbg] mpg123_read end/error err=%d got=%zu\n", err, got);
      mpg123_close(g_mh);
      g_track_open = 0; // Mark file stream closed, KEEP g_mh handle valid!
      if (++tries > g_track_count)
        break;
    }
  }

  if (total == 0) {
    fprintf(stderr, "[music-dbg] refill_buffer: total PCM bytes decoded is 0!\n");
    return 0;
  }

  alBufferData(albuf, g_fmt, g_pcm, (ALsizei)total, (ALsizei)g_rate);
  ALenum alerr = alGetError();
  if (alerr != AL_NO_ERROR)
    fprintf(stderr, "[music-dbg] alBufferData error: 0x%x\n", alerr);

  return 1;
}

int music_playlist_open(const char *name) {
  (void)name;
  music_shutdown();

  int count = discover_tracks();
  fprintf(stderr, "[music-dbg] discover_tracks -> %d\n", count);
  if (count <= 0) {
    debugPrintf("[music] playlistOpen: no .mp3 files found in ./GTA/\n");
    return 0;
  }

  if (!g_mpg_ok) {
    int merr = mpg123_init();
    fprintf(stderr, "[music-dbg] mpg123_init -> %d\n", merr);
    if (merr != MPG123_OK)
      return 0;
    g_mpg_ok = 1;
  }
  if (!g_mh) {
    int e = 0;
    g_mh = mpg123_new(NULL, &e);
    fprintf(stderr, "[music-dbg] mpg123_new -> %p (e=%d)\n", (void *)g_mh, e);
    if (!g_mh)
      return 0;
  }

  if (!g_pcm)
    g_pcm = (unsigned char *)malloc(PCM_CAP);

  if (!g_src) {
    if (openal_setup() < 0)
      return 0;
    al_begin();
    alGenSources(1, &g_src);
    alGenBuffers(NBUF, g_bufs);
    ALenum alerr = alGetError();
    al_end();
    fprintf(stderr, "[music-dbg] alGenSources src=%u err=0x%x\n", g_src, alerr);
  }

  g_opened = 1;
  g_playing = 0;
  g_current = -1;
  g_track_open = 0;

  for (int i = 0; i < g_track_count && i < 5; i++)
    fprintf(stderr, "[music-dbg] GTA track[%d] = %s\n", i, g_tracks[i]);

  return g_track_count;
}

int music_playlist_count(void) {
  return g_track_count;
}

int music_playlist_is_playing(void) {
  return g_playing;
}

void music_playlist_play(void) {
  fprintf(stderr, "[music-dbg] playlistPlay called (g_opened=%d, g_src=%u)\n", g_opened, g_src);
  if (!g_opened || !g_src)
    return;

  al_begin();
  ALint queued = 0;
  alGetSourcei(g_src, AL_BUFFERS_QUEUED, &queued);
  fprintf(stderr, "[music-dbg] playlistPlay initial queued=%d\n", queued);

  if (queued == 0) {
    int filled = 0;
    for (int i = 0; i < NBUF; i++) {
      if (refill_buffer(g_bufs[i])) {
        alSourceQueueBuffers(g_src, 1, &g_bufs[i]);
        filled++;
      }
    }
    fprintf(stderr, "[music-dbg] playlistPlay refilled and queued %d buffers\n", filled);
  }

  alSourcePlay(g_src);

  ALint state = 0;
  ALfloat gain = 0.0f;
  alGetSourcei(g_src, AL_SOURCE_STATE, &state);
  alGetSourcef(g_src, AL_GAIN, &gain);
  ALenum alerr = alGetError();
  fprintf(stderr, "[music-dbg] alSourcePlay -> src=%u state=%d (4114/1027=PLAYING) gain=%.2f err=0x%x\n",
          g_src, state, gain, alerr);

  g_playing = 1;
  al_end();
}

void music_playlist_pause(void) {
  fprintf(stderr, "[music-dbg] playlistPause called\n");
  if (!g_src || !g_playing)
    return;
  al_begin();
  alSourcePause(g_src);
  g_playing = 0;
  al_end();
}

void music_playlist_stop(void) {
  fprintf(stderr, "[music-dbg] playlistStop called\n");
  if (!g_src)
    return;
  al_begin();
  alSourceStop(g_src);
  ALint queued = 0;
  alGetSourcei(g_src, AL_BUFFERS_QUEUED, &queued);
  if (queued > 0) {
    ALuint done[16];
    while (queued > 0) {
      int n = queued > 16 ? 16 : queued;
      alSourceUnqueueBuffers(g_src, (ALsizei)n, done);
      queued -= n;
    }
  }
  g_playing = 0;
  al_end();
}

void music_tick(void) {
  if (!g_opened || !g_playing || !g_src)
    return;

  al_begin();

  ALint processed = 0;
  alGetSourcei(g_src, AL_BUFFERS_PROCESSED, &processed);

  static unsigned tick_countdown = 0;
  if (--tick_countdown <= 0) {
    tick_countdown = 180; /* ~every 3 seconds at 60fps */
    ALint state = 0;
    ALint queued = 0;
    alGetSourcei(g_src, AL_SOURCE_STATE, &state);
    alGetSourcei(g_src, AL_BUFFERS_QUEUED, &queued);
    fprintf(stderr, "[music-dbg] music_tick: track %d/%d, state=%d queued=%d processed=%d\n",
            g_current + 1, g_track_count, state, queued, processed);
  }

  while (processed-- > 0) {
    ALuint b = 0;
    alSourceUnqueueBuffers(g_src, 1, &b);
    if (refill_buffer(b))
      alSourceQueueBuffers(g_src, 1, &b);
    else {
      fprintf(stderr, "[music-dbg] music_tick: refill failed, stopping source\n");
      alSourceStop(g_src);
      break;
    }
  }

  ALint state = 0;
  alGetSourcei(g_src, AL_SOURCE_STATE, &state);
  if (state != AL_PLAYING) {
    ALint queued = 0;
    alGetSourcei(g_src, AL_BUFFERS_QUEUED, &queued);
    if (queued > 0) {
      fprintf(stderr, "[music-dbg] music_tick: restarting stopped source! queued=%d\n", queued);
      alSourcePlay(g_src);
    }
  }

  al_end();
}

void music_shutdown(void) {
  if (g_src) {
    al_begin();
    alSourceStop(g_src);
    alDeleteSources(1, &g_src);
    alDeleteBuffers(NBUF, g_bufs);
    g_src = 0;
    al_end();
  }
  if (g_ctx) {
    alcDestroyContext(g_ctx);
    g_ctx = NULL;
  }
  if (g_dev) {
    alcCloseDevice(g_dev);
    g_dev = NULL;
  }
  if (g_mh) {
    mpg123_close(g_mh);
    mpg123_delete(g_mh);
    g_mh = NULL;
  }
  if (g_mpg_ok) {
    mpg123_exit();
    g_mpg_ok = 0;
  }
  free(g_pcm);
  g_pcm = NULL;
  g_opened = 0;
  g_playing = 0;
  g_track_open = 0;
  g_track_count = 0;
}
