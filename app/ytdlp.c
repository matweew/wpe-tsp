/*
 * yt-dlp version check and update, see ytdlp.h.
 */
#include "ytdlp.h"

#include <fcntl.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define RELEASES_LATEST "https://github.com/yt-dlp/yt-dlp/releases/latest"
#define RELEASES_DL "https://github.com/yt-dlp/yt-dlp/releases/download"
#define ASSET_NAME "yt-dlp_linux_aarch64"
#define SUMS_NAME "SHA2-256SUMS"
#define CHECK_INTERVAL (24 * 3600)
#define VERSION_TIMEOUT_S 60   /* the onefile build unpacks itself first: slow on the SD card */
#define VERSION_OUT "/tmp/wpe-tsp-ytdlp-version"
#define CHUNK 65536

typedef enum { Y_IDLE, Y_CHECKING, Y_DOWNLOADING, Y_VERIFYING } State;

static struct {
    char *path;
    SoupSession *session;
    State state;
    char *current;              /* installed version, or NULL */
    char *latest;               /* newest release tag, or NULL */
    gboolean force, fresh;
    YtdlpChecked checked;
    YtdlpInstalled installed;
    void *user_data;
    /* --version runs */
    GPid version_pid;
    guint version_timeout;
    void (*version_done)(char *version);
    /* download */
    char want[65];
    GChecksum *sum;
    GInputStream *in;
    GOutputStream *out;
    goffset total, now;
} y;

static char *sibling(const char *suffix)
{
    return g_strconcat(y.path, suffix, NULL);
}

int ytdlp_version_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        char *ea, *eb;
        long x = strtol(a, &ea, 10), yv = strtol(b, &eb, 10);
        if (x != yv)
            return x < yv ? -1 : 1;
        a = ea; b = eb;
        if (*a == '.') a++; else if (*a) return 0;   /* unexpected text: don't claim a difference */
        if (*b == '.') b++; else if (*b) return 0;
    }
    return 0;
}

/* Release tags are digits and dots only: reject anything else before it reaches a URL/path. */
static gboolean valid_tag(const char *tag, size_t n)
{
    if (!n || n >= 40)
        return FALSE;
    for (size_t i = 0; i < n; i++)
        if (!g_ascii_isdigit(tag[i]) && tag[i] != '.')
            return FALSE;
    return TRUE;
}

/* ---- "<bin> --version" -------------------------------------------------------- */

static void version_output(gpointer user_data)
{
    (void)user_data;
    int fd = open(VERSION_OUT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }
    int null = open("/dev/null", O_WRONLY);
    if (null >= 0) { dup2(null, STDERR_FILENO); close(null); }
}

static gboolean version_timed_out(gpointer user_data)
{
    (void)user_data;
    y.version_timeout = 0;
    kill(y.version_pid, SIGKILL);
    return G_SOURCE_REMOVE;
}

static void version_exited(GPid pid, gint status, gpointer user_data)
{
    (void)user_data;
    waitpid(pid, NULL, WNOHANG); /* GLib can leave it a zombie on kernel 4.9 */
    g_spawn_close_pid(pid);
    if (y.version_timeout) {
        g_source_remove(y.version_timeout);
        y.version_timeout = 0;
    }
    char *version = NULL;
    if (g_spawn_check_wait_status(status, NULL) && g_file_get_contents(VERSION_OUT, &version, NULL, NULL)) {
        g_strdelimit(version, "\r\n", '\0');
        g_strstrip(version);
        if (!*version)
            g_clear_pointer(&version, g_free);
    }
    y.version_done(version);
}

static void run_version(const char *bin, void (*done)(char *version))
{
    char *argv[] = { (char *)bin, "--version", NULL };
    y.version_done = done;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, version_output, NULL, &y.version_pid, NULL)) {
        done(NULL);
        return;
    }
    g_child_watch_add(y.version_pid, version_exited, NULL);
    y.version_timeout = g_timeout_add_seconds(VERSION_TIMEOUT_S, version_timed_out, NULL);
}

/* "<size> <mtime>" of the binary: the cached version in <path>.version is valid while it matches */
static char *binary_signature(const char *path)
{
    struct stat st;
    if (stat(path, &st))
        return NULL;
    return g_strdup_printf("%lld %lld", (long long)st.st_size, (long long)st.st_mtime);
}

static void save_version(const char *version)
{
    char *signature = binary_signature(y.path), *vpath = sibling(".version");
    if (signature) {
        char *text = g_strdup_printf("%s\n%s\n", version, signature);
        g_file_set_contents(vpath, text, -1, NULL);
        g_free(text);
    }
    g_free(signature);
    g_free(vpath);
}

/* ---- check ------------------------------------------------------------------- */

static void check_finish(void)
{
    y.state = Y_IDLE;
    fprintf(stderr, "[wpe-tsp] yt-dlp: installed %s, latest %s%s\n", y.current ? y.current : "none",
            y.latest ? y.latest : "unknown", y.fresh ? "" : " (checked today)");
    if (y.checked)
        y.checked(y.current, y.latest, y.fresh, y.user_data);
}

/* HEAD /releases/latest -> 302 Location: .../releases/tag/<tag> */
static void latest_received(GObject *source, GAsyncResult *result, gpointer user_data)
{
    SoupMessage *msg = user_data;
    GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, NULL);
    const char *location = soup_message_headers_get_one(soup_message_get_response_headers(msg), "Location");
    const char *tag = location ? strstr(location, "/tag/") : NULL;
    if (tag) {
        tag += 5;
        size_t n = strcspn(tag, "/?#");
        if (valid_tag(tag, n)) {
            g_free(y.latest);
            y.latest = g_strndup(tag, n);
            char *cpath = sibling(".checked"), *text = g_strdup_printf("%s\n", y.latest);
            g_file_set_contents(cpath, text, -1, NULL);
            g_free(cpath);
            g_free(text);
        }
    }
    if (body)
        g_bytes_unref(body);
    g_object_unref(msg);
    check_finish();
}

static void check_latest(void)
{
    /* Looked up within the last day: reuse that, no network */
    char *cpath = sibling(".checked");
    struct stat st;
    char *cached = NULL;
    if (!y.force && !stat(cpath, &st) && time(NULL) - st.st_mtime < CHECK_INTERVAL
        && g_file_get_contents(cpath, &cached, NULL, NULL)) {
        g_strstrip(cached);
        if (valid_tag(cached, strlen(cached))) {
            g_free(y.latest);
            y.latest = cached;
            g_free(cpath);
            check_finish();
            return;
        }
        g_free(cached);
    }
    g_free(cpath);
    y.fresh = TRUE;
    SoupMessage *msg = soup_message_new(SOUP_METHOD_HEAD, RELEASES_LATEST);
    soup_message_add_flags(msg, SOUP_MESSAGE_NO_REDIRECT);
    soup_session_send_and_read_async(y.session, msg, G_PRIORITY_DEFAULT, NULL, latest_received, msg);
}

static void current_version_done(char *version)
{
    g_free(y.current);
    y.current = version;
    if (version)
        save_version(version);
    check_latest();
}

void ytdlp_init(const char *path)
{
    y.path = g_strdup(path);
    y.session = soup_session_new_with_options("user-agent", "wpe-tsp", "timeout", 30, NULL);
}

void ytdlp_check(gboolean force, YtdlpChecked done, void *user_data)
{
    if (y.state != Y_IDLE)
        return;
    y.state = Y_CHECKING;
    y.force = force;
    y.fresh = FALSE;
    y.checked = done;
    y.user_data = user_data;

    /* Installed version, cached with the binary's size+mtime: "--version" (slow: Python
     * startup) runs only after the binary has changed. */
    g_clear_pointer(&y.current, g_free);
    char *signature = binary_signature(y.path);
    if (!signature || access(y.path, X_OK)) {
        g_free(signature);
        check_latest();
        return;
    }
    char *vpath = sibling(".version"), *cached = NULL;
    if (g_file_get_contents(vpath, &cached, NULL, NULL)) {
        char **lines = g_strsplit(cached, "\n", 3);
        if (lines[0] && lines[1] && *lines[0] && !strcmp(g_strstrip(lines[1]), signature))
            y.current = g_strdup(g_strstrip(lines[0]));
        g_strfreev(lines);
        g_free(cached);
    }
    g_free(vpath);
    g_free(signature);
    if (y.current)
        check_latest();
    else
        run_version(y.path, current_version_done);
}

/* ---- install ------------------------------------------------------------------- */

static void install_finish(gboolean ok, const char *message)
{
    g_clear_object(&y.in);
    g_clear_object(&y.out);
    g_clear_pointer(&y.sum, g_checksum_free);
    y.state = Y_IDLE;
    fprintf(stderr, "[wpe-tsp] yt-dlp: %s\n", message);
    if (y.installed)
        y.installed(ok, message, y.user_data);
}

static void install_failed(const char *message)
{
    char *newp = sibling(".new");
    g_remove(newp);
    g_free(newp);
    install_finish(FALSE, message);
}

static void new_version_done(char *version)
{
    char *newp = sibling(".new"), *bakp = sibling(".bak"), *cpath = sibling(".checked");
    if (!version) {
        install_failed("the new yt-dlp doesn't run on this device");
        goto out;
    }
    /* Keep the old binary as .bak; rename() over the target is atomic and safe even while
     * the old binary is running. */
    gboolean had_old = g_file_test(y.path, G_FILE_TEST_EXISTS);
    if (had_old && g_rename(y.path, bakp)) {
        install_failed("can't back up the current yt-dlp");
        goto out;
    }
    if (g_rename(newp, y.path)) {
        if (had_old)
            g_rename(bakp, y.path);
        install_failed("can't install the new yt-dlp");
        goto out;
    }
    save_version(version);
    g_remove(cpath);
    char *old = y.current;
    y.current = g_strdup(version);
    char *message = old ? g_strdup_printf("yt-dlp updated from %s to %s", old, version)
                        : g_strdup_printf("yt-dlp %s installed", version);
    install_finish(TRUE, message);
    g_free(message);
    g_free(old);
out:
    g_free(version);
    g_free(newp); g_free(bakp); g_free(cpath);
}

static void chunk_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)user_data;
    GError *error = NULL;
    GBytes *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);
    if (!bytes) {
        install_failed("download failed");
        g_error_free(error);
        return;
    }
    gsize n = g_bytes_get_size(bytes);
    if (n) {
        const guint8 *data = g_bytes_get_data(bytes, NULL);
        g_checksum_update(y.sum, data, n);
        gboolean written = g_output_stream_write_all(y.out, data, n, NULL, NULL, NULL);
        g_bytes_unref(bytes);
        if (!written) {
            install_failed("write failed - SD card full?");
            return;
        }
        y.now += n;
        g_input_stream_read_bytes_async(y.in, CHUNK, G_PRIORITY_DEFAULT, NULL, chunk_read, NULL);
        return;
    }
    g_bytes_unref(bytes);

    /* Complete: verify */
    y.state = Y_VERIFYING;
    gboolean closed = g_output_stream_close(y.out, NULL, NULL);
    char *newp = sibling(".new");
    if (!closed)
        install_failed("write failed - SD card full?");
    else if (strcmp(g_checksum_get_string(y.sum), y.want))
        install_failed("checksum mismatch, download discarded");
    else if (chmod(newp, 0755))
        install_failed("can't make the new yt-dlp executable");
    else
        run_version(newp, new_version_done);
    g_free(newp);
}

static void binary_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
    SoupMessage *msg = user_data;
    y.in = soup_session_send_finish(SOUP_SESSION(source), result, NULL);
    guint status = soup_message_get_status(msg);
    y.total = soup_message_headers_get_content_length(soup_message_get_response_headers(msg));
    g_object_unref(msg);
    if (!y.in || status != SOUP_STATUS_OK) {
        install_failed("download failed");
        return;
    }
    char *newp = sibling(".new");
    GFile *file = g_file_new_for_path(newp);
    y.out = G_OUTPUT_STREAM(g_file_replace(file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL));
    g_object_unref(file);
    g_free(newp);
    if (!y.out) {
        install_failed("can't write next to the current yt-dlp");
        return;
    }
    y.sum = g_checksum_new(G_CHECKSUM_SHA256);
    y.now = 0;
    g_input_stream_read_bytes_async(y.in, CHUNK, G_PRIORITY_DEFAULT, NULL, chunk_read, NULL);
}

/* SHA2-256SUMS: "<hex>  <name>" per line */
static void sums_received(GObject *source, GAsyncResult *result, gpointer user_data)
{
    SoupMessage *msg = user_data;
    GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, NULL);
    guint status = soup_message_get_status(msg);
    g_object_unref(msg);
    y.want[0] = 0;
    if (body && status == SOUP_STATUS_OK) {
        char *text = g_strndup(g_bytes_get_data(body, NULL), g_bytes_get_size(body));
        char **lines = g_strsplit(text, "\n", -1);
        for (int i = 0; lines[i] && !y.want[0]; i++) {
            char hash[80], name[256];
            if (sscanf(lines[i], "%79s %255s", hash, name) == 2 && strlen(hash) == 64
                && !strcmp(name[0] == '*' ? name + 1 : name, ASSET_NAME)) {
                char *lower = g_ascii_strdown(hash, -1);
                memcpy(y.want, lower, 65);
                g_free(lower);
            }
        }
        g_strfreev(lines);
        g_free(text);
    }
    if (body)
        g_bytes_unref(body);
    if (!y.want[0]) {
        install_failed("couldn't get the release checksums");
        return;
    }
    char *url = g_strdup_printf("%s/%s/%s", RELEASES_DL, y.latest, ASSET_NAME);
    SoupMessage *bin = soup_message_new(SOUP_METHOD_GET, url);
    g_free(url);
    soup_session_send_async(y.session, bin, G_PRIORITY_DEFAULT, NULL, binary_response, bin);
}

void ytdlp_install(YtdlpInstalled done, void *user_data)
{
    if (y.state != Y_IDLE)
        return;
    y.installed = done;
    y.user_data = user_data;
    if (!y.latest) {
        install_finish(FALSE, "couldn't reach GitHub");
        return;
    }
    y.state = Y_DOWNLOADING;
    y.total = y.now = 0;
    char *url = g_strdup_printf("%s/%s/%s", RELEASES_DL, y.latest, SUMS_NAME);
    SoupMessage *msg = soup_message_new(SOUP_METHOD_GET, url);
    g_free(url);
    soup_session_send_and_read_async(y.session, msg, G_PRIORITY_DEFAULT, NULL, sums_received, msg);
}

gboolean ytdlp_busy(void)
{
    return y.state != Y_IDLE;
}

char *ytdlp_status(double *progress)
{
    *progress = -1;
    if (y.state == Y_DOWNLOADING) {
        if (y.total > 0)
            *progress = (double)y.now / y.total;
        return g_strdup_printf("Downloading yt-dlp %s\xe2\x80\xa6 %.1f MB", y.latest, y.now / 1e6);
    }
    if (y.state == Y_VERIFYING)
        return g_strdup_printf("Checking yt-dlp %s\xe2\x80\xa6", y.latest);
    return NULL;
}
