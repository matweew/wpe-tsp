/*
 * Video playback through the bundled mpv/yt-dlp, see player.h.
 * Mirrors youtube-tsp's src/player.c: same yt-dlp invocation (format, --print resolution -g,
 * HLS retry, portrait rotation) and same mpv command line and environment.
 */
#include "player.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define RESOLVE_OUT "/tmp/wpe-tsp-yurl"
#define RESOLVE_ERR "/tmp/wpe-tsp-yurl-err"
/* youtube-tsp's format: H.264 + AAC up to 720p (what the device decodes in software in real time) */
#define YT_DLP_FORMAT "bestvideo[vcodec^=avc][width<=1280][height<=1280]+bestaudio[acodec^=mp4a]" \
                      "/best[ext=mp4][width<=1280][height<=1280]/22/18"
#define HLS_FORMAT "best[height<=720][protocol^=m3u8]/best[protocol^=m3u8]/best"
#define ERROR_SECONDS 8   /* long enough to read the longer messages */
/* Resolved stream URLs are reused (watching again, coming back to a video) until shortly before
 * YouTube's links expire (expire= in the URL, ~6 hours). */
#define CACHE_MAX 16
#define CACHE_MARGIN_S 600
/* mpv failing this soon on cached URLs (e.g. 403: links tied to a changed IP) means resolve again */
#define CACHE_FAIL_S 10

typedef enum { P_IDLE, P_RESOLVING, P_PLAYING } PlayerState;

typedef struct {
    char *watch_url;            /* canonical, see player_youtube_watch_url() */
    char *video, *audio;        /* audio NULL: muxed stream */
    int rotate;
    gint64 expires;             /* unix time to stop using the URLs */
} Resolved;

static struct {
    char *dir;                  /* <app>/mpv: mpv, lib/, mpv.conf, input.conf, ca-certificates.crt */
    char *fonts_dir;            /* <app>/share/fonts (subtitles) */
    char *ytdlp;                /* <app>/bin/yt-dlp */
    PlayerCallbacks cb;
    PlayerState state;
    GPid pid;
    char *watch_url;
    gboolean hls_retry_done;
    GQueue cache;               /* of Resolved*, most recently used first */
    gboolean from_cache;        /* mpv is playing cached URLs */
    gint64 started;             /* monotonic time mpv was started */
    char *error;                /* last failure, shown for a few seconds */
    guint error_timer;
} pl;

static char *mpv_path(void)
{
    return g_build_filename(pl.dir, "mpv", NULL);
}

static char *ca_path(void)
{
    return g_build_filename(pl.dir, "ca-certificates.crt", NULL);
}

/* ---- state / status ------------------------------------------------------- */

static void status_changed(void)
{
    if (pl.cb.status_changed)
        pl.cb.status_changed(pl.cb.user_data);
}

static gboolean error_expired(gpointer user_data)
{
    (void)user_data;
    pl.error_timer = 0;
    g_clear_pointer(&pl.error, g_free);
    status_changed();
    return G_SOURCE_REMOVE;
}

static void fail(const char *message)
{
    fprintf(stderr, "[wpe-tsp] video: %s\n", message);
    g_free(pl.error);
    pl.error = g_strdup(message);
    if (pl.error_timer)
        g_source_remove(pl.error_timer);
    pl.error_timer = g_timeout_add_seconds(ERROR_SECONDS, error_expired, NULL);
    pl.state = P_IDLE;
    status_changed();
    if (pl.cb.finished)
        pl.cb.finished(FALSE, pl.cb.user_data);
}

void player_init(const char *app_dir, const PlayerCallbacks *callbacks)
{
    pl.dir = g_build_filename(app_dir, "mpv", NULL);
    pl.fonts_dir = g_build_filename(app_dir, "share", "fonts", NULL);
    pl.ytdlp = g_build_filename(app_dir, "bin", "yt-dlp", NULL);
    pl.cb = *callbacks;
}

gboolean player_available(void)
{
    char *mpv = mpv_path();
    gboolean ok = g_file_test(mpv, G_FILE_TEST_IS_EXECUTABLE);
    g_free(mpv);
    return ok;
}

char *player_ytdlp_path(void)
{
    return g_strdup(pl.ytdlp);
}

gboolean player_busy(void)
{
    return pl.state != P_IDLE;
}

gboolean player_playing(void)
{
    return pl.state == P_PLAYING;
}

char *player_status(void)
{
    if (pl.state == P_RESOLVING)
        return g_strdup("Loading video\xe2\x80\xa6   B: cancel");
    if (pl.error)
        return g_strdup_printf("Can't play video: %s", pl.error);
    return NULL;
}

/* ---- YouTube URLs --------------------------------------------------------- */

static gboolean valid_video_id(const char *id)
{
    if (!id || strlen(id) != 11)
        return FALSE;
    for (const char *c = id; *c; c++)
        if (!g_ascii_isalnum(*c) && *c != '-' && *c != '_')
            return FALSE;
    return TRUE;
}

char *player_youtube_watch_url(const char *uri)
{
    GUri *u = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;
    if (!u)
        return NULL;
    const char *host = g_uri_get_host(u) ? g_uri_get_host(u) : "";
    const char *path = g_uri_get_path(u);
    char *id = NULL;
    if (!strcmp(host, "youtu.be")) {
        id = g_strdup(path + (*path == '/'));
    } else if (!strcmp(host, "youtube.com") || g_str_has_suffix(host, ".youtube.com")) {
        if (!strcmp(path, "/watch") && g_uri_get_query(u)) {
            GHashTable *params = g_uri_parse_params(g_uri_get_query(u), -1, "&", G_URI_PARAMS_NONE, NULL);
            if (params) {
                id = g_strdup(g_hash_table_lookup(params, "v"));
                g_hash_table_unref(params);
            }
        } else if (g_str_has_prefix(path, "/shorts/")) {
            id = g_strdup(path + strlen("/shorts/"));
        }
    }
    g_uri_unref(u);
    char *watch = valid_video_id(id) ? g_strdup_printf("https://www.youtube.com/watch?v=%s", id) : NULL;
    g_free(id);
    return watch;
}

/* ---- resolved URL cache ---------------------------------------------------- */

static void resolved_free(Resolved *r)
{
    g_free(r->watch_url);
    g_free(r->video);
    g_free(r->audio);
    g_free(r);
}

/* googlevideo.com URLs carry their expiry as a query parameter (expire=T) or, for HLS manifests,
 * as a path segment (/expire/T/). 0 if there is none. */
static gint64 url_expiry(const char *url)
{
    const char *e = url ? strstr(url, "expire=") : NULL;
    if (e)
        return g_ascii_strtoll(e + strlen("expire="), NULL, 10);
    e = url ? strstr(url, "/expire/") : NULL;
    return e ? g_ascii_strtoll(e + strlen("/expire/"), NULL, 10) : 0;
}

static GList *cache_find(const char *watch_url)
{
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    for (GList *l = pl.cache.head; l; ) {
        GList *next = l->next;
        Resolved *r = l->data;
        if (r->expires <= now) { /* drop expired entries on the way */
            resolved_free(r);
            g_queue_delete_link(&pl.cache, l);
        } else if (!strcmp(r->watch_url, watch_url))
            return l;
        l = next;
    }
    return NULL;
}

static void cache_drop(const char *watch_url)
{
    GList *l = cache_find(watch_url);
    if (l) {
        resolved_free(l->data);
        g_queue_delete_link(&pl.cache, l);
    }
}

static void cache_add(const char *watch_url, const char *video, const char *audio, int rotate)
{
    gint64 expires = url_expiry(video);
    gint64 audio_expires = audio ? url_expiry(audio) : expires;
    if (!expires || !audio_expires) /* unknown lifetime: don't guess */
        return;
    cache_drop(watch_url);
    Resolved *r = g_new0(Resolved, 1);
    r->watch_url = g_strdup(watch_url);
    r->video = g_strdup(video);
    r->audio = g_strdup(audio);
    r->rotate = rotate;
    r->expires = MIN(expires, audio_expires) - CACHE_MARGIN_S;
    g_queue_push_head(&pl.cache, r);
    while (g_queue_get_length(&pl.cache) > CACHE_MAX)
        resolved_free(g_queue_pop_tail(&pl.cache));
}

/* ---- mpv ------------------------------------------------------------------ */

static void detach_session(gpointer user_data)
{
    (void)user_data;
    setsid();
}

/* GLib's child watch reports the exit but, on this kernel (4.9, no pidfd), can leave the
 * child unreaped: reap it ourselves (harmless if it was already). */
static void reap(GPid pid)
{
    waitpid(pid, NULL, WNOHANG);
    g_spawn_close_pid(pid);
}

static void start_resolve(void);

static void on_mpv_exited(GPid pid, gint status, gpointer user_data)
{
    (void)user_data;
    reap(pid);
    pl.state = P_IDLE;
    if (pl.cb.restore_display)
        pl.cb.restore_display(pl.cb.user_data);
    fprintf(stderr, "[wpe-tsp] video: player exited\n");
    if (pl.from_cache && !g_spawn_check_wait_status(status, NULL)
        && g_get_monotonic_time() - pl.started < CACHE_FAIL_S * G_USEC_PER_SEC) {
        fprintf(stderr, "[wpe-tsp] video: cached URLs failed, resolving again\n");
        cache_drop(pl.watch_url);
        pl.from_cache = FALSE;
        start_resolve();
        return;
    }
    status_changed();
    if (pl.cb.finished)
        pl.cb.finished(TRUE, pl.cb.user_data);
}

static void start_mpv(const char *video_url, const char *audio_url, int rotate)
{
    char *mpv = mpv_path();

    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup(mpv));
    g_ptr_array_add(argv, g_strdup("--fs"));
    g_ptr_array_add(argv, g_strdup("--no-terminal"));
    g_ptr_array_add(argv, g_strdup("--log-file=/tmp/mpv_last.log"));
    g_ptr_array_add(argv, g_strdup("--no-ytdl"));
    g_ptr_array_add(argv, g_strdup_printf("--config-dir=%s", pl.dir));
    g_ptr_array_add(argv, g_strdup_printf("--sub-fonts-dir=%s", pl.fonts_dir));
    if (rotate)
        g_ptr_array_add(argv, g_strdup_printf("--video-rotate=%d", rotate));
    if (audio_url && *audio_url)
        g_ptr_array_add(argv, g_strdup_printf("--audio-file=%s", audio_url));
    g_ptr_array_add(argv, g_strdup(video_url));
    g_ptr_array_add(argv, NULL);

    /* mpv's own libraries (built for the device's glibc) and certificates; FONTCONFIG_FILE is
     * the browser's (launch.sh) */
    char **env = g_get_environ();
    char *ld = g_strdup_printf("%s/lib:/usr/trimui/lib", pl.dir);
    env = g_environ_setenv(env, "LD_LIBRARY_PATH", ld, TRUE);
    char *certs = ca_path();
    env = g_environ_setenv(env, "SSL_CERT_FILE", certs, TRUE);

    if (pl.cb.release_display)
        pl.cb.release_display(pl.cb.user_data);
    GError *error = NULL;
    if (g_spawn_async(pl.dir, (char **)argv->pdata, env,
                      G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                      detach_session, NULL, &pl.pid, &error)) {
        pl.state = P_PLAYING;
        pl.started = g_get_monotonic_time();
        fprintf(stderr, "[wpe-tsp] video: playing %.100s\n", video_url);
        g_child_watch_add(pl.pid, on_mpv_exited, NULL);
    } else {
        if (pl.cb.restore_display)
            pl.cb.restore_display(pl.cb.user_data);
        fail(error->message);
        g_error_free(error);
    }

    g_strfreev(env);
    g_free(ld); g_free(certs);
    g_ptr_array_free(argv, TRUE);
    g_free(mpv);
}

void player_play_url(const char *url)
{
    if (pl.state != P_IDLE)
        return;
    pl.from_cache = FALSE;
    start_mpv(url, NULL, 0);
}

/* ---- yt-dlp --------------------------------------------------------------- */

static void redirect_output(gpointer user_data)
{
    (void)user_data;
    int out = open(RESOLVE_OUT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int err = open(RESOLVE_ERR, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out >= 0) { dup2(out, STDOUT_FILENO); close(out); }
    if (err >= 0) { dup2(err, STDERR_FILENO); close(err); }
}

/* "ERROR: [youtube] ID: Video unavailable." -> "Video unavailable" */
static char *yt_dlp_error(void)
{
    char *text = NULL;
    g_file_get_contents(RESOLVE_ERR, &text, NULL, NULL);
    char *message = NULL;
    for (char *line = text ? strtok(text, "\n") : NULL; line && !message; line = strtok(NULL, "\n")) {
        char *e = strstr(line, "ERROR:");
        if (!e)
            continue;
        char *msg = e;
        for (char *p = e; *p; p++)
            if (p[0] == ':' && p[1] == ' ')
                msg = p + 2;
        message = g_strstrip(g_strdup(msg));
        size_t n = strlen(message);
        if (n && message[n - 1] == '.')
            message[n - 1] = 0;
    }
    g_free(text);
    return message ? message : g_strdup("could not get the video stream");
}

static void on_resolve_exited(GPid pid, gint status, gpointer user_data)
{
    (void)user_data;
    reap(pid);
    if (pl.state != P_RESOLVING || pid != pl.pid) /* cancelled (or an older, cancelled run) */
        return;
    if (!g_spawn_check_wait_status(status, NULL)) {
        char *message = yt_dlp_error();
        if (!pl.hls_retry_done && strstr(message, "Requested format is not available")) {
            g_free(message);
            pl.hls_retry_done = TRUE; /* live streams: retry once with a combined HLS stream */
            start_resolve();
            return;
        }
        fail(message);
        g_free(message);
        return;
    }

    /* Output: live_status and "WxH" lines (per selected stream), then the video URL and (if separate)
     * the audio URL */
    char *text = NULL;
    g_file_get_contents(RESOLVE_OUT, &text, NULL, NULL);
    char *video = NULL, *audio = NULL;
    int rotate = 0;
    gboolean got_resolution = FALSE, post_live = FALSE, live = FALSE;
    char **lines = g_strsplit(text ? text : "", "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        int w = 0, h = 0;
        if (!strcmp(line, "post_live"))
            post_live = TRUE;
        else if (!strcmp(line, "is_live"))
            live = TRUE;
        else if (!got_resolution && sscanf(line, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            rotate = h > w ? 270 : 0; /* portrait video: counterclockwise, like the browser's portrait mode */
            got_resolution = TRUE;
        } else if (strstr(line, "://")) {
            if (!video)
                video = g_strdup(line);
            else if (!audio)
                audio = g_strdup(line);
        }
    }
    g_strfreev(lines);
    g_free(text);
    if (audio && video && !strcmp(audio, video))
        g_clear_pointer(&audio, g_free);

    if (post_live) {
        /* A live stream that just ended exists only as DASH fragments until YouTube processes it:
         * the URL from -g is a single fragment (the last few seconds), and mpv can't join the
         * fragments (no durations). Say so instead of playing those seconds. */
        fail("this live stream just ended and YouTube is still processing it, try again later");
    } else if (video) {
        if (!live) /* a live stream's URLs are a moving window: resolve each time */
            cache_add(pl.watch_url, video, audio, rotate);
        start_mpv(video, audio, rotate);
    }
    else
        fail("no stream URL from yt-dlp");
    g_free(video);
    g_free(audio);
}

static void start_resolve(void)
{
    char *ca = ca_path();
    const char *format = pl.hls_retry_done ? HLS_FORMAT : YT_DLP_FORMAT;
    char *argv[] = { pl.ytdlp, "-f", (char *)format, "--print", "live_status", "--print", "resolution", "-g",
                     pl.watch_url, NULL };
    char **env = g_environ_setenv(g_get_environ(), "SSL_CERT_FILE", ca, TRUE);

    GError *error = NULL;
    if (g_spawn_async(pl.dir, argv, env, G_SPAWN_DO_NOT_REAP_CHILD, redirect_output, NULL, &pl.pid, &error)) {
        pl.state = P_RESOLVING;
        fprintf(stderr, "[wpe-tsp] video: resolving %s (%s)\n", pl.watch_url, format);
        g_child_watch_add(pl.pid, on_resolve_exited, NULL);
        status_changed();
    } else {
        fail(error->message);
        g_error_free(error);
    }
    g_strfreev(env);
    g_free(ca);
}

void player_play_youtube(const char *watch_url)
{
    if (pl.state != P_IDLE)
        return;
    g_clear_pointer(&pl.error, g_free);
    if (!g_file_test(pl.ytdlp, G_FILE_TEST_IS_EXECUTABLE)) {
        fail("yt-dlp isn't installed");
        if (pl.cb.ytdlp_missing)
            pl.cb.ytdlp_missing(pl.cb.user_data);
        return;
    }
    g_free(pl.watch_url);
    pl.watch_url = g_strdup(watch_url);
    pl.hls_retry_done = FALSE;
    pl.from_cache = FALSE;
    GList *cached = cache_find(watch_url);
    if (cached) {
        Resolved *r = cached->data;
        g_queue_unlink(&pl.cache, cached); /* most recently used first */
        g_queue_push_head_link(&pl.cache, cached);
        fprintf(stderr, "[wpe-tsp] video: cached URLs for %s\n", watch_url);
        pl.from_cache = TRUE;
        start_mpv(r->video, r->audio, r->rotate);
        return;
    }
    start_resolve();
}

void player_cancel(void)
{
    if (pl.state != P_RESOLVING)
        return;
    kill(pl.pid, SIGTERM);
    pl.state = P_IDLE;
    fprintf(stderr, "[wpe-tsp] video: cancelled\n");
    status_changed();
    if (pl.cb.finished)
        pl.cb.finished(FALSE, pl.cb.user_data);
}
