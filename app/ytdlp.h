/*
 * yt-dlp version check and update (ported from youtube-tsp's src/updater.c).
 *
 * Checks the latest GitHub release of yt-dlp against the installed binary (at most once a day
 * over the network) and, on request, installs yt-dlp_linux_aarch64 safely:
 *   download to <path>.new (SHA-256 computed on the way) -> compare with the release's
 *   SHA2-256SUMS -> "--version" smoke test -> keep the old binary as <path>.bak -> rename()
 *   into place. A failed step never touches the working binary.
 * Everything runs asynchronously on the GLib main loop.
 */
#pragma once

#include <glib.h>

/* installed: version of the installed binary, NULL if it's missing or doesn't run.
 * latest: newest release, NULL if GitHub couldn't be reached.
 * fresh: the release was looked up just now (not taken from the once-a-day cache). */
typedef void (*YtdlpChecked)(const char *installed, const char *latest, gboolean fresh, void *user_data);
/* ok: installed; message: what happened, for the user ("yt-dlp updated to …" / the error) */
typedef void (*YtdlpInstalled)(gboolean ok, const char *message, void *user_data);

void ytdlp_init(const char *path);
void ytdlp_check(gboolean force, YtdlpChecked done, void *user_data);
/* Download and install the release found by the last check. */
void ytdlp_install(YtdlpInstalled done, void *user_data);
gboolean ytdlp_busy(void);
/* Comparison of "2026.09.28" / "2025.01.15.232754" style versions: <0, 0, >0 */
int ytdlp_version_cmp(const char *a, const char *b);
/* Status strip text while installing ("Downloading yt-dlp …"), newly allocated, or NULL;
 * progress 0..1, or <0 if unknown. */
char *ytdlp_status(double *progress);
