/*
 * Downloads: saves files into a directory on the SD card (DOWNLOAD_DIR), tracks progress for the
 * status strip, and serves the wpe-tsp://downloads page.
 */
#pragma once

#include <glib.h>
#include <wpe/webkit.h>

#define DOWNLOADS_URI "wpe-tsp://downloads"

/* Called (throttled) whenever progress or the status text changes. */
typedef void (*DownloadsChanged)(void *user_data);
void downloads_init(WebKitNetworkSession *session, const char *dir, DownloadsChanged changed,
                    void *user_data);
const char *downloads_dir(void);

/* Start downloading a URL right away (user explicitly chose it). */
void downloads_start(const char *uri);

/* Download prompt: the response starts downloading while the prompt is shown, hidden until
 * downloads_confirm(). (Some servers, e.g. ftp.acc.umu.se, drop a connection that isn't read
 * for ~15 s, so the response can't simply wait for the answer.) */
void downloads_start_unconfirmed(WebKitPolicyDecision *decision, const char *uri);
/* The prompt's answer: keep = show it (and its result), else cancel it and delete the file. */
void downloads_confirm(gboolean keep);

/* Status strip: returns a newly allocated line ("name — 45% · 12.3 / 30.1 MB", "Saved: …") and
 * the progress (0..1, or <0 for none), or NULL when there is nothing to show. */
char *downloads_status(double *progress);

/* wpe-tsp://downloads[/cancel/<id>|/play-file/<name>|/clear]: performs the action, returns the page HTML. */
char *downloads_handle_page(const char *uri);
/* DOWNLOADS_URI/open/<name> (a saved file): its file:// URI for the browser to load, else NULL */
char *downloads_open_file_uri(const char *uri);

/* Free space in the download directory, in bytes (-1 if unknown). */
gint64 downloads_free_space(void);

/* "12.3 MB" etc. (newly allocated) */
char *downloads_format_size(gint64 bytes);
