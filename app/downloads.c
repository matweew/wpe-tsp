/*
 * Download manager, see downloads.h. WebKit streams the data straight to the destination file
 * (in the network process), so large files don't use RAM.
 */
#include "downloads.h"

#include "pages.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define NOTICE_SECONDS 5            /* how long "Saved: …" / "failed" stays in the status strip */
#define CHANGED_INTERVAL_US 250000  /* throttle progress redraws */

typedef enum { DL_ACTIVE, DL_DONE, DL_FAILED, DL_CANCELLED } DownloadState;

typedef struct {
    guint id;
    WebKitDownload *download;   /* NULL once finished */
    char *name;                 /* file name on disk */
    char *path;
    char *error;
    char *mime;                 /* from the response, to know what can be played */
    DownloadState state;
    guint64 received;
    guint64 total;              /* 0 if unknown */
} Item;

static struct {
    char *dir;
    DownloadsChanged changed;
    DownloadsPlay play;
    void *user_data;
    GPtrArray *items;           /* Item*, oldest first */
    guint next_id;
    gint64 last_changed;
    Item *notice;               /* finished/failed item shown in the status strip */
    gint64 notice_until;
    guint notice_timer;
} dl;

static void item_free(gpointer data)
{
    Item *item = data;
    if (item->download)
        g_object_unref(item->download);
    g_free(item->name);
    g_free(item->path);
    g_free(item->error);
    g_free(item->mime);
    g_free(item);
}

static void notify_changed(gboolean force)
{
    gint64 now = g_get_monotonic_time();
    if (!force && now - dl.last_changed < CHANGED_INTERVAL_US)
        return;
    dl.last_changed = now;
    if (dl.changed)
        dl.changed(dl.user_data);
}

static gboolean notice_expired(gpointer user_data)
{
    (void)user_data;
    dl.notice_timer = 0;
    dl.notice = NULL;
    notify_changed(TRUE);
    return G_SOURCE_REMOVE;
}

static void show_notice(Item *item)
{
    dl.notice = item;
    if (dl.notice_timer)
        g_source_remove(dl.notice_timer);
    dl.notice_timer = g_timeout_add_seconds(NOTICE_SECONDS, notice_expired, NULL);
    notify_changed(TRUE);
}

static Item *find_item(WebKitDownload *download)
{
    for (guint i = 0; i < dl.items->len; i++) {
        Item *item = g_ptr_array_index(dl.items, i);
        if (item->download == download)
            return item;
    }
    return NULL;
}

char *downloads_format_size(gint64 bytes)
{
    if (bytes < 0)
        return g_strdup("?");
    if (bytes < 1024 * 1024)
        return g_strdup_printf("%.0f KB", bytes / 1024.0);
    if (bytes < 1024LL * 1024 * 1024)
        return g_strdup_printf("%.1f MB", bytes / (1024.0 * 1024));
    return g_strdup_printf("%.2f GB", bytes / (1024.0 * 1024 * 1024));
}

gint64 downloads_free_space(void)
{
    struct statvfs st;
    if (statvfs(dl.dir, &st) != 0)
        return -1;
    return (gint64)st.f_bavail * (gint64)st.f_frsize;
}

/* "name.ext" -> first of "name.ext", "name (1).ext", "name (2).ext", ... that doesn't exist */
static char *unique_path(const char *name)
{
    char *path = g_build_filename(dl.dir, name, NULL);
    const char *dot = strrchr(name, '.');
    size_t stem = dot && dot != name ? (size_t)(dot - name) : strlen(name);
    for (int n = 1; g_file_test(path, G_FILE_TEST_EXISTS) && n < 1000; n++) {
        g_free(path);
        char *candidate = g_strdup_printf("%.*s (%d)%s", (int)stem, name, n, name + stem);
        path = g_build_filename(dl.dir, candidate, NULL);
        g_free(candidate);
    }
    return path;
}

/* WebKit corrects a URL-derived file name's extension when it doesn't match the Content-Type, but
 * compares MIME type names without resolving aliases: "debian.iso" served as
 * application/x-iso9660-image (an alias of the type *.iso maps to) became "debian.iso9660".
 * Returns the URL's own file name when WebKit only changed its extension and the original
 * extension's type is the response's type (or a subtype of it), else NULL. */
static char *url_name_if_same_type(WebKitDownload *download, const char *suggested)
{
    WebKitURIRequest *request = webkit_download_get_request(download);
    WebKitURIResponse *response = webkit_download_get_response(download);
    const char *mime = response ? webkit_uri_response_get_mime_type(response) : NULL;
    GUri *uri = request ? g_uri_parse(webkit_uri_request_get_uri(request), G_URI_FLAGS_NONE, NULL) : NULL;
    char *original = uri ? g_path_get_basename(g_uri_get_path(uri)) : NULL;
    if (uri)
        g_uri_unref(uri);
    const char *dot = original ? strrchr(original, '.') : NULL;
    const char *suggested_dot = strrchr(suggested, '.');
    char *result = NULL;
    if (mime && dot && dot != original && suggested_dot && strcmp(original, suggested)
        && dot - original == suggested_dot - suggested && !strncmp(original, suggested, (size_t)(dot - original))) {
        char *ext_type = g_content_type_guess(original, NULL, 0, NULL);
        char *response_type = g_content_type_from_mime_type(mime);
        if (ext_type && response_type && !g_content_type_is_unknown(ext_type)
            && (g_content_type_is_a(ext_type, response_type) || g_content_type_is_a(response_type, ext_type)))
            result = g_strdup(original);
        g_free(ext_type);
        g_free(response_type);
    }
    g_free(original);
    return result;
}

static gboolean on_decide_destination(WebKitDownload *download, const char *suggested_filename, gpointer user_data)
{
    (void)user_data;
    Item *item = find_item(download);
    char *name = g_path_get_basename(suggested_filename && *suggested_filename ? suggested_filename : "download");
    char *url_name = url_name_if_same_type(download, name);
    if (url_name) {
        fprintf(stderr, "[wpe-tsp] downloads: keeping %s (WebKit suggested %s)\n", url_name, name);
        g_free(name);
        name = url_name;
    }
    if (!strcmp(name, ".") || !strcmp(name, "/")) {
        g_free(name);
        name = g_strdup("download");
    }
    g_mkdir_with_parents(dl.dir, 0755);
    char *path = unique_path(name);
    webkit_download_set_destination(download, path);
    if (item) {
        g_free(item->path);
        g_free(item->name);
        item->path = path;
        item->name = g_path_get_basename(path);
    } else
        g_free(path);
    g_free(name);
    notify_changed(TRUE);
    return TRUE;
}

static void on_received_data(WebKitDownload *download, guint64 length, gpointer user_data)
{
    (void)length; (void)user_data;
    Item *item = find_item(download);
    if (!item)
        return;
    item->received = webkit_download_get_received_data_length(download);
    WebKitURIResponse *response = webkit_download_get_response(download);
    if (response)
        item->total = webkit_uri_response_get_content_length(response);
    notify_changed(FALSE);
}

static void on_failed(WebKitDownload *download, GError *error, gpointer user_data)
{
    (void)user_data;
    Item *item = find_item(download);
    if (!item)
        return;
    if (g_error_matches(error, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER)) {
        item->state = DL_CANCELLED;
    } else {
        item->state = DL_FAILED;
        item->error = g_strdup(error->message);
        fprintf(stderr, "[wpe-tsp] download failed: %s: %s\n", item->name ? item->name : "?", error->message);
    }
    if (item->path) /* don't leave partial files behind */
        g_unlink(item->path);
}

static void on_finished(WebKitDownload *download, gpointer user_data)
{
    (void)user_data;
    Item *item = find_item(download);
    if (!item)
        return;
    if (item->state == DL_ACTIVE) {
        item->state = DL_DONE;
        item->received = webkit_download_get_received_data_length(download);
        WebKitURIResponse *response = webkit_download_get_response(download);
        const char *mime = response ? webkit_uri_response_get_mime_type(response) : NULL;
        item->mime = g_strdup(mime);
        fprintf(stderr, "[wpe-tsp] download saved: %s\n", item->path ? item->path : "?");
    }
    /* "finished" also follows "failed": this is the one place that releases the download */
    g_signal_handlers_disconnect_by_data(download, item);
    g_object_unref(item->download);
    item->download = NULL;
    if (item->state != DL_CANCELLED)
        show_notice(item);
    else
        notify_changed(TRUE);
}

static void on_download_started(WebKitNetworkSession *session, WebKitDownload *download, gpointer user_data)
{
    (void)session; (void)user_data;
    Item *item = g_new0(Item, 1);
    item->id = ++dl.next_id;
    item->download = g_object_ref(download);
    item->state = DL_ACTIVE;
    WebKitURIRequest *request = webkit_download_get_request(download);
    item->name = g_path_get_basename(request ? webkit_uri_request_get_uri(request) : "download");
    g_ptr_array_add(dl.items, item);
    g_signal_connect(download, "decide-destination", G_CALLBACK(on_decide_destination), item);
    g_signal_connect(download, "received-data", G_CALLBACK(on_received_data), item);
    g_signal_connect(download, "failed", G_CALLBACK(on_failed), item);
    g_signal_connect(download, "finished", G_CALLBACK(on_finished), item);
    notify_changed(TRUE);
}

/* Downloads interrupted by the browser being killed (power off, an update) leave WebKit's partial
 * "<name>.wkdownload" and the empty placeholder "<name>" it creates at the start: remove both. */
static void remove_interrupted_downloads(void)
{
    GDir *dir = g_dir_open(dl.dir, 0, NULL);
    const char *name;
    while (dir && (name = g_dir_read_name(dir))) {
        if (!g_str_has_suffix(name, ".wkdownload"))
            continue;
        char *partial = g_build_filename(dl.dir, name, NULL);
        char *target = g_strndup(partial, strlen(partial) - strlen(".wkdownload"));
        GStatBuf st;
        if (g_stat(target, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 0)
            g_unlink(target);
        g_unlink(partial);
        fprintf(stderr, "[wpe-tsp] downloads: removed interrupted %s\n", name);
        g_free(partial);
        g_free(target);
    }
    if (dir)
        g_dir_close(dir);
}

void downloads_init(WebKitNetworkSession *session, const char *dir, DownloadsChanged changed,
                    DownloadsPlay play, void *user_data)
{
    dl.play = play;
    dl.dir = g_strdup(dir);
    g_mkdir_with_parents(dl.dir, 0755); /* also makes the free-space check work from the start */
    remove_interrupted_downloads();
    dl.changed = changed;
    dl.user_data = user_data;
    dl.items = g_ptr_array_new_with_free_func(item_free);
    g_signal_connect(session, "download-started", G_CALLBACK(on_download_started), NULL);
}

const char *downloads_dir(void)
{
    return dl.dir;
}

void downloads_start(const char *uri)
{
    WebKitDownload *download = webkit_network_session_download_uri(webkit_network_session_get_default(), uri);
    if (download)
        g_object_unref(download); /* tracked through "download-started" */
}

static double item_progress(const Item *item)
{
    return item->total ? MIN(1.0, (double)item->received / (double)item->total) : -1;
}

char *downloads_status(double *progress)
{
    *progress = -1;
    if (!dl.items) /* not initialized yet (first frames are drawn before WebKit is set up) */
        return NULL;
    Item *active = NULL;
    guint n_active = 0;
    for (guint i = 0; i < dl.items->len; i++) {
        Item *item = g_ptr_array_index(dl.items, i);
        if (item->state == DL_ACTIVE) {
            if (!active)
                active = item;
            n_active++;
        }
    }
    if (active) {
        char *got = downloads_format_size((gint64)active->received);
        char *more = n_active > 1 ? g_strdup_printf("   (+%u more)", n_active - 1) : g_strdup("");
        char *line;
        *progress = item_progress(active);
        if (*progress >= 0) {
            char *total = downloads_format_size((gint64)active->total);
            line = g_strdup_printf("Downloading %s \xe2\x80\x94 %d%% \xc2\xb7 %s / %s%s", active->name,
                                   (int)(*progress * 100), got, total, more);
            g_free(total);
        } else
            line = g_strdup_printf("Downloading %s \xe2\x80\x94 %s%s", active->name, got, more);
        g_free(got);
        g_free(more);
        return line;
    }
    if (dl.notice) {
        if (dl.notice->state == DL_DONE)
            return g_strdup_printf("Saved: %s", dl.notice->path);
        return g_strdup_printf("Download failed: %s (%s)", dl.notice->name, dl.notice->error ? dl.notice->error : "error");
    }
    return NULL;
}

/* Video/audio by MIME type, or by extension (servers often send application/octet-stream) */
static gboolean is_media(const char *name, const char *mime)
{
    if (mime && (g_str_has_prefix(mime, "video/") || g_str_has_prefix(mime, "audio/")))
        return TRUE;
    static const char *const extensions[] = {
        ".mp4", ".m4v", ".mkv", ".webm", ".mov", ".avi", ".3gp", ".ts",
        ".mp3", ".m4a", ".aac", ".ogg", ".oga", ".opus", ".flac", ".wav", NULL
    };
    char *lower = name ? g_ascii_strdown(name, -1) : NULL;
    gboolean media = FALSE;
    for (int i = 0; lower && extensions[i] && !media; i++)
        media = g_str_has_suffix(lower, extensions[i]);
    g_free(lower);
    return media;
}

typedef struct {
    char *name;
    gint64 size;
    gint64 mtime;
} SavedFile;

static void saved_file_free(gpointer data)
{
    SavedFile *f = data;
    g_free(f->name);
    g_free(f);
}

static gint newest_first(gconstpointer a, gconstpointer b)
{
    const SavedFile *fa = *(SavedFile *const *)a, *fb = *(SavedFile *const *)b;
    return fa->mtime < fb->mtime ? 1 : fa->mtime > fb->mtime ? -1 : 0;
}

static gboolean is_being_downloaded(const char *path)
{
    for (guint i = 0; i < dl.items->len; i++) {
        Item *item = g_ptr_array_index(dl.items, i);
        if (item->state == DL_ACTIVE && item->path && !strcmp(item->path, path))
            return TRUE;
    }
    return FALSE;
}

/* Files in the download directory (from any session), newest first */
static GPtrArray *saved_files(void)
{
    GPtrArray *files = g_ptr_array_new_with_free_func(saved_file_free);
    GDir *dir = g_dir_open(dl.dir, 0, NULL);
    const char *name;
    while (dir && (name = g_dir_read_name(dir))) {
        char *path = g_build_filename(dl.dir, name, NULL);
        GStatBuf st;
        /* WebKit writes "<name>.wkdownload" until a download completes: in progress or left over
         * from an interrupted one, not a saved file */
        if (name[0] != '.' && !g_str_has_suffix(name, ".wkdownload") && g_stat(path, &st) == 0
            && S_ISREG(st.st_mode) && !is_being_downloaded(path)) {
            SavedFile *f = g_new0(SavedFile, 1);
            f->name = g_strdup(name);
            f->size = (gint64)st.st_size;
            f->mtime = (gint64)st.st_mtime;
            g_ptr_array_add(files, f);
        }
        g_free(path);
    }
    if (dir)
        g_dir_close(dir);
    g_ptr_array_sort(files, newest_first);
    return files;
}

static char *downloads_page_html(void)
{
    GString *html = g_string_new(NULL);
    gboolean any_active = FALSE;
    for (guint i = 0; i < dl.items->len; i++)
        any_active |= ((Item *)g_ptr_array_index(dl.items, i))->state == DL_ACTIVE;

    g_string_append_printf(html, PAGE_HEAD, "Downloads");
    if (any_active) /* keep progress current while something downloads */
        g_string_append(html, "<script>setTimeout(()=>location.replace('" DOWNLOADS_URI "'),2000)</script>");
    char *free_space = downloads_format_size(downloads_free_space());
    char *dir = g_markup_escape_text(dl.dir, -1);
    g_string_append_printf(html, "<h1>Downloads</h1><div class='s' style='margin:0 14px 10px'>%s \xc2\xb7 %s free</div>",
                           dir, free_space);
    g_free(dir);
    g_free(free_space);

    for (guint n = dl.items->len; n > 0; n--) { /* newest first */
        Item *item = g_ptr_array_index(dl.items, n - 1);
        char *name = g_markup_escape_text(item->name ? item->name : "download", -1);
        char *got = downloads_format_size((gint64)item->received);
        char *status;
        switch (item->state) {
        case DL_ACTIVE: {
            double p = item_progress(item);
            char *total = item->total ? downloads_format_size((gint64)item->total) : NULL;
            status = p >= 0 ? g_strdup_printf("%d%% \xc2\xb7 %s / %s \xc2\xb7 A: cancel", (int)(p * 100), got, total)
                            : g_strdup_printf("%s \xc2\xb7 A: cancel", got);
            g_free(total);
            g_string_append_printf(html, "<a href='" DOWNLOADS_URI "/cancel/%u'><div class='t'>%s</div><div class='s'>%s</div>"
                                   "<div class='bar'><div style='width:%d%%'></div></div></a>",
                                   item->id, name, status, p >= 0 ? (int)(p * 100) : 0);
            break;
        }
        case DL_DONE: /* listed with the saved files below */
            status = NULL;
            break;
        case DL_FAILED: {
            char *err = g_markup_escape_text(item->error ? item->error : "error", -1);
            status = g_strdup_printf("Failed: %s", err);
            g_free(err);
            g_string_append_printf(html, "<div class='item'><div class='t'>%s</div><div class='s warn'>%s</div></div>", name, status);
            break;
        }
        default:
            status = g_strdup("Cancelled");
            g_string_append_printf(html, "<div class='item'><div class='t'>%s</div><div class='s'>%s</div></div>", name, status);
        }
        g_free(status);
        g_free(got);
        g_free(name);
    }
    gboolean session_entries = FALSE;
    for (guint i = 0; i < dl.items->len; i++)
        session_entries |= ((Item *)g_ptr_array_index(dl.items, i))->state != DL_DONE;

    GPtrArray *files = saved_files();
    if (files->len)
        g_string_append(html, "<div class='s' style='margin:14px 14px 4px'>Saved files</div>");
    for (guint i = 0; i < files->len; i++) {
        SavedFile *f = g_ptr_array_index(files, i);
        char *name = g_markup_escape_text(f->name, -1);
        char *size = downloads_format_size(f->size);
        GDateTime *when = g_date_time_new_from_unix_local(f->mtime);
        char *date = when ? g_date_time_format(when, "%d.%m.%Y %H:%M") : g_strdup("");
        if (when)
            g_date_time_unref(when);
        if (dl.play && is_media(f->name, NULL)) {
            char *escaped = g_uri_escape_string(f->name, NULL, FALSE);
            g_string_append_printf(html, "<a href='" DOWNLOADS_URI "/play-file/%s'><div class='t'>%s</div>"
                                   "<div class='s'>%s \xc2\xb7 %s \xc2\xb7 A: play</div></a>", escaped, name, size, date);
            g_free(escaped);
        } else
            g_string_append_printf(html, "<div class='item'><div class='t'>%s</div><div class='s'>%s \xc2\xb7 %s</div></div>",
                                   name, size, date);
        g_free(date);
        g_free(size);
        g_free(name);
    }
    if (session_entries)
        g_string_append(html, "<a class='clear' href='" DOWNLOADS_URI "/clear'><div class='t'>Clear list</div>"
                              "<div class='s'>removes failed and cancelled entries; files are kept</div></a>");
    if (!files->len && !dl.items->len)
        g_string_append(html, "<div class='empty'>No downloads yet. Use SELECT \xe2\x86\x92 Downloads to save a link or a video from a page.</div>");
    g_ptr_array_free(files, TRUE);
    g_string_append(html, PAGE_TAIL);
    return g_string_free(html, FALSE);
}

char *downloads_handle_page(const char *uri)
{
    const char *action = uri + strlen(DOWNLOADS_URI);
    gboolean is_action = *action == '/';
    if (g_str_has_prefix(action, "/cancel/")) {
        guint id = (guint)g_ascii_strtoull(action + strlen("/cancel/"), NULL, 10);
        for (guint i = 0; i < dl.items->len; i++) {
            Item *item = g_ptr_array_index(dl.items, i);
            if (item->id == id && item->download)
                webkit_download_cancel(item->download);
        }
    } else if (g_str_has_prefix(action, "/play-file/")) {
        /* a file name inside the download directory, nothing else */
        char *name = g_uri_unescape_string(action + strlen("/play-file/"), NULL);
        if (name && *name && !strchr(name, '/') && strcmp(name, "..") && dl.play) {
            char *path = g_build_filename(dl.dir, name, NULL);
            if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
                dl.play(path, dl.user_data);
            g_free(path);
        }
        g_free(name);
    } else if (!strcmp(action, "/clear")) {
        for (guint i = dl.items->len; i > 0; i--) {
            Item *item = g_ptr_array_index(dl.items, i - 1);
            if (item->state != DL_ACTIVE) {
                if (dl.notice == item)
                    dl.notice = NULL;
                g_ptr_array_remove_index(dl.items, i - 1);
            }
        }
    }
    /* After an action, go to the plain page (so reloading doesn't repeat the action) */
    if (is_action)
        return g_strdup("<!DOCTYPE html><script>location.replace('" DOWNLOADS_URI "')</script>");
    return downloads_page_html();
}
