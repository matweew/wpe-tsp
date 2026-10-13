/*
 * Favorites, see favorites.h.
 */
#include "favorites.h"

#include "pages.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    char *uri;
    char *title;
} Favorite;

static GPtrArray *favorites; /* of Favorite*, newest first */
static char *favorites_path;

static void favorite_free(gpointer data)
{
    Favorite *f = data;
    g_free(f->uri);
    g_free(f->title);
    g_free(f);
}

static void save(void)
{
    GString *out = g_string_new(NULL);
    for (guint i = 0; i < favorites->len; i++) {
        Favorite *f = g_ptr_array_index(favorites, i);
        g_string_append_printf(out, "%s\t%s\n", f->uri, f->title);
    }
    GError *error = NULL;
    if (!g_file_set_contents(favorites_path, out->str, (gssize)out->len, &error)) {
        fprintf(stderr, "[wpe-tsp] saving favorites failed: %s\n", error->message);
        g_error_free(error);
    }
    g_string_free(out, TRUE);
}

void favorites_init(void)
{
    favorites = g_ptr_array_new_with_free_func(favorite_free);
    char *dir = g_build_filename(g_get_user_data_dir(), "wpe-browser", NULL);
    g_mkdir_with_parents(dir, 0755);
    favorites_path = g_build_filename(dir, "favorites.txt", NULL);
    g_free(dir);
    char *contents = NULL;
    if (!g_file_get_contents(favorites_path, &contents, NULL, NULL))
        return;
    char **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char **fields = g_strsplit(lines[i], "\t", 2);
        if (fields[0] && *fields[0]) {
            Favorite *f = g_new0(Favorite, 1);
            f->uri = g_strdup(fields[0]);
            f->title = g_strdup(fields[1] ? fields[1] : "");
            g_ptr_array_add(favorites, f);
        }
        g_strfreev(fields);
    }
    g_strfreev(lines);
    g_free(contents);
}

static int find(const char *uri)
{
    for (guint i = 0; uri && i < favorites->len; i++)
        if (!strcmp(((Favorite *)g_ptr_array_index(favorites, i))->uri, uri))
            return (int)i;
    return -1;
}

gboolean favorites_contains(const char *uri)
{
    return find(uri) >= 0;
}

gboolean favorites_toggle(const char *uri, const char *title)
{
    int i = find(uri);
    if (i >= 0) {
        g_ptr_array_remove_index(favorites, (guint)i);
        save();
        return FALSE;
    }
    Favorite *f = g_new0(Favorite, 1);
    f->uri = g_strdup(uri);
    /* one line per favorite: no tabs or line breaks in the title */
    f->title = g_strdelimit(g_strdup(title ? title : ""), "\t\r\n", ' ');
    g_ptr_array_insert(favorites, 0, f);
    save();
    return TRUE;
}

static char *page_html(void)
{
    GString *html = g_string_new(NULL);
    g_string_append_printf(html, PAGE_HEAD "<h1>Favorites</h1>", "Favorites");
    for (guint i = 0; i < favorites->len; i++) {
        Favorite *f = g_ptr_array_index(favorites, i);
        GUri *parsed = g_uri_parse(f->uri, G_URI_FLAGS_NONE, NULL);
        const char *host = parsed && g_uri_get_host(parsed) ? g_uri_get_host(parsed) : f->uri;
        if (g_str_has_prefix(host, "www."))
            host += 4;
        char *uri = g_markup_escape_text(f->uri, -1);
        char *title = g_markup_escape_text(*f->title ? f->title : host, -1);
        char *site = g_markup_escape_text(host, -1);
        char *param = g_uri_escape_string(f->uri, NULL, FALSE);
        g_string_append_printf(html, "<div class='row'><a class='main' href='%s'><div class='t'>%s</div>"
                                     "<div class='s'>%s</div></a><a class='side' href='" FAVORITES_URI "/delete?u=%s'>"
                                     "Delete</a></div>", uri, title, site, param);
        g_free(uri); g_free(title); g_free(site); g_free(param);
        if (parsed)
            g_uri_unref(parsed);
    }
    if (!favorites->len)
        g_string_append(html, "<div class='empty'>No favorites yet: SELECT &rarr; Add to Favorites on a page.</div>");
    g_string_append(html, PAGE_TAIL);
    return g_string_free(html, FALSE);
}

char *favorites_handle_page(const char *uri)
{
    const char *prefix = FAVORITES_URI "/delete?u=";
    if (!g_str_has_prefix(uri, prefix))
        return page_html();
    char *target = g_uri_unescape_string(uri + strlen(prefix), NULL);
    int i = find(target);
    if (i >= 0) {
        g_ptr_array_remove_index(favorites, (guint)i);
        save();
    }
    g_free(target);
    /* Back to the list, replacing this action in the history (dark: no white flash) */
    return g_strdup("<!DOCTYPE html><html style='background:#202124'><script>location.replace('"
                    FAVORITES_URI "')</script></html>");
}
