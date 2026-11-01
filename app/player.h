/*
 * Video playback with the bundled mpv (and yt-dlp or ytp for YouTube pages):
 *   <app>/mpv/mpv (+ lib/, mpv.conf, input.conf, ca-certificates.crt), <app>/bin/yt-dlp, <app>/bin/ytp.
 * Same mpv build and configuration as the youtube-tsp client; ytp is its RustyPipe backend.
 */
#pragma once

#include <glib.h>

typedef struct {
    void (*release_display)(void *user_data);   /* free the SDL window before mpv starts */
    void (*restore_display)(void *user_data);   /* recreate it after mpv exits */
    void (*status_changed)(void *user_data);    /* redraw the status strip */
    void (*finished)(gboolean played, void *user_data); /* playback ended (or failed/cancelled) */
    void (*ytdlp_missing)(void *user_data);     /* a YouTube video needs yt-dlp, which isn't installed */
    void *user_data;
} PlayerCallbacks;

/* app_dir: the browser's directory (<app>/mpv, <app>/bin/yt-dlp, <app>/bin/ytp) */
void player_init(const char *app_dir, const PlayerCallbacks *callbacks);
/* PLAY_YOUTUBE_IN_MPV=2: YouTube is resolved by ytp (RustyPipe) instead of yt-dlp, logged in with the
 * youtube.com cookies of the browser's cookies.sqlite (read only, fresh on every run).
 * cache_dir: ytp's cache (player JS, visitor data) */
void player_use_ytp(const char *cookies, const char *cache_dir);
gboolean player_available(void);                    /* mpv is installed */
char *player_ytdlp_path(void);                      /* <app>/bin/yt-dlp, newly allocated */

/* Canonical watch URL for YouTube video pages (watch?v=, youtu.be/, /shorts/), else NULL. */
char *player_youtube_watch_url(const char *uri);

void player_play_youtube(const char *watch_url);   /* resolve with yt-dlp (or ytp), then mpv */
void player_play_url(const char *url);             /* direct media URL or file: mpv only */

gboolean player_busy(void);                         /* resolving or playing */
gboolean player_playing(void);                      /* mpv running (display released) */
void player_cancel(void);                           /* cancel while resolving */

/* Status strip text ("Loading video… B: cancel", errors), newly allocated, or NULL. */
char *player_status(void);
gboolean player_failed(void);                       /* the last video failed (its error is still shown) */
