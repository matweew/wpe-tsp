/*
 * Favorites: pages saved from the SELECT menu ("Add to Favorites"), newest first, kept in
 * $XDG_DATA_HOME/wpe-browser/favorites.txt (one "uri<TAB>title" per line) and shown as the page
 * wpe-tsp://favorites (START opens one, right -> Delete).
 */
#pragma once

#include <glib.h>

#define FAVORITES_URI "wpe-tsp://favorites"

void favorites_init(void);
gboolean favorites_contains(const char *uri);
/* Adds the page, or removes it if it's a favorite. Returns TRUE if it is one now. */
gboolean favorites_toggle(const char *uri, const char *title);
/* FAVORITES_URI[/delete?u=<uri>]: performs the action, returns the page HTML. */
char *favorites_handle_page(const char *uri);
