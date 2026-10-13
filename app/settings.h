/*
 * settings.conf from the UI: the wpe-tsp://settings page (each setting with its value and what it
 * does; d-pad left/right or START changes a choice, START edits a text with the on-screen keyboard)
 * and saving single values into the file, keeping the rest of it (comments, other settings).
 */
#pragma once

#include <glib.h>

#define SETTINGS_URI "wpe-tsp://settings"

/* A changed value was saved: apply it now and return TRUE, or FALSE if it applies at the next
 * start (the page then offers a restart). */
typedef gboolean (*SettingsApply)(const char *key, const char *value);
/* The page's "Restart browser" was chosen */
typedef void (*SettingsRestart)(void);

/* path: settings.conf (read now: the values the page shows) */
void settings_init(const char *path, SettingsApply apply, SettingsRestart restart);

/* Save one value into settings.conf: replaces its line, or adds it at the end. Doesn't apply it. */
void settings_save(const char *key, const char *value);

/* SETTINGS_URI[/next|prev/<key>|/set/<key>?v=<value>|/restart]: performs the action, returns the
 * page HTML. previous_uri: the page the settings were opened from (offered as the home page). */
char *settings_handle_page(const char *uri, const char *previous_uri);

/* The page's change requests (message handler "wpeTspSettings", with the same action URIs): performs
 * the action, returns the page's updated list of settings (HTML). */
char *settings_handle_message(const char *uri, const char *previous_uri);
