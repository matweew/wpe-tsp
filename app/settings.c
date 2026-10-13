/*
 * settings.conf from the UI, see settings.h.
 */
#include "settings.h"

#include "pages.h"
#include <stdio.h>
#include <string.h>

/* Choices: value/label pairs, NULL-terminated. A value in the file that isn't one of them (a custom
 * user agent, an empty "use the default" value) is offered as an extra choice. */
static const char *const start_pages[] = { "home", "Home page", "last", "Last visited page", "address", "Address bar", NULL };
static const char *const search_engines[] = {
    "https://www.google.com/search?q=", "Google", "https://duckduckgo.com/?q=", "DuckDuckGo",
    "https://www.bing.com/search?q=", "Bing", NULL };
static const char *const scales[] = { "", "Per device", /* 1.0 to 3.0 (the browser's limits) in 0.1 steps */
    "1.0", "1.0", "1.1", "1.1", "1.2", "1.2", "1.3", "1.3", "1.4", "1.4",
    "1.5", "1.5", "1.6", "1.6", "1.7", "1.7", "1.8", "1.8", "1.9", "1.9",
    "2.0", "2.0", "2.1", "2.1", "2.2", "2.2", "2.3", "2.3", "2.4", "2.4",
    "2.5", "2.5", "2.6", "2.6", "2.7", "2.7", "2.8", "2.8", "2.9", "2.9",
    "3.0", "3.0", NULL };
static const char *const user_agents[] = { "mobile", "Mobile", "desktop", "Desktop", NULL };
static const char *const on_off[] = { "1", "On", "0", "Off", NULL };
static const char *const youtube_players[] = { "0", "In the browser", "1", "In mpv", NULL };
static const char *const memory_limits[] = { "350", "350 MB", "400", "400 MB", "450", "450 MB", "500", "500 MB",
                                             "550", "550 MB", "600", "600 MB", "650", "650 MB", "700", "700 MB", NULL };
static const char *const hide_times[] = { "0", "Never", "5", "5 s", "10", "10 s", "20", "20 s", "30", "30 s", "60", "60 s", NULL };
static const char *const history_sizes[] = { "0", "Off", "10", "10", "20", "20", "40", "40", "60", "60", NULL };

typedef struct {
    const char *key, *title, *help;
    const char *const *choices; /* NULL: free text */
    const char *group;          /* starts a group of settings (heading) */
} Setting;

static const Setting setting_list[] = {
    { "HOME_URL", "Home page", "Opened at start and by Home in the menu", NULL, "Pages" },
    { "START_PAGE", "At start open", "The home page, the last visited page or the address bar", start_pages, NULL },
    { "SEARCH_URL", "Search with", "Address-bar input that isn't an address is searched here", search_engines, NULL },
    { "SCALE", "Page scale", "Text and page size (pages still render at full resolution)", scales, NULL },
    { "USER_AGENT", "Site version", "Mobile sites are lighter: best for 1 GB of RAM", user_agents, NULL },
    { "AD_BLOCK", "Block ads and trackers", "EasyList + EasyPrivacy; pages load faster and use less memory", on_off, "Ads" },
    { "AD_BLOCK_EXCEPTIONS", "Ads allowed on", "Sites separated by spaces (with their subdomains); also from the menu", NULL, NULL },
    { "PLAY_YOUTUBE_IN_MPV", "YouTube plays", "In mpv: hardware H.264, smooth 720p, light on CPU and memory", youtube_players, "Video" },
    { "HW_VIDEO_DECODE", "Hardware H.264 in pages", "720p at ~13% CPU instead of ~70%", on_off, NULL },
    { "WEBGL", "WebGL (3D graphics)", "3D pages use more memory", on_off, NULL },
    { "KEYBOARD_LAYOUTS", "Keyboard layouts", "XKB names, e.g. \"us,ua,de(nodeadkeys)\"; the first is the default", NULL, "Input" },
    { "POINTER_HIDE_SECONDS", "Hide the pointer after", "Without moving it; the stick or A shows it again", hide_times, NULL },
    { "DOWNLOAD_DIR", "Download folder", "Created if missing", NULL, "Storage and memory" },
    { "HISTORY_SIZE", "History size", "Recently visited pages kept in History", history_sizes, NULL },
    { "PAGE_MEMORY_LIMIT_MB", "Page memory limit", "A page using more is closed (\"needs more memory\")", memory_limits, NULL },
};
#define N_SETTINGS G_N_ELEMENTS(setting_list)

static struct {
    char *path;
    GHashTable *values;     /* key -> value saved in the file ("" if absent) */
    GHashTable *started;    /* key -> value at start: the one in effect for settings applied at start */
    GPtrArray *pending;     /* titles of settings saved that apply after a restart */
    SettingsApply apply;
    SettingsRestart restart;
} settings;

static const Setting *find_setting(const char *key)
{
    for (guint i = 0; i < N_SETTINGS; i++)
        if (!strcmp(setting_list[i].key, key))
            return &setting_list[i];
    return NULL;
}

/* A shell value as launch.sh sees it: "double-quoted" (\" \\ \$ \` escapes), 'single-quoted' or bare */
static char *unquote(const char *s)
{
    GString *out = g_string_new(NULL);
    if (*s == '"') {
        for (s++; *s && *s != '"'; s++) {
            if (*s == '\\' && s[1] && strchr("\"\\$`", s[1]))
                s++;
            g_string_append_c(out, *s);
        }
    } else if (*s == '\'') {
        for (s++; *s && *s != '\''; s++)
            g_string_append_c(out, *s);
    } else {
        for (; *s && !g_ascii_isspace(*s) && *s != '#'; s++)
            g_string_append_c(out, *s);
    }
    return g_string_free(out, FALSE);
}

/* Numbers and words as they are (AD_BLOCK=1), anything else in double quotes */
static char *quote(const char *value)
{
    gboolean plain = *value != '\0';
    for (const char *c = value; *c; c++)
        plain &= g_ascii_isalnum(*c) || *c == '.' || *c == '_' || *c == '-';
    if (plain)
        return g_strdup(value);
    GString *out = g_string_new("\"");
    for (const char *c = value; *c; c++) {
        if (strchr("\"\\$`", *c))
            g_string_append_c(out, '\\');
        g_string_append_c(out, *c);
    }
    g_string_append_c(out, '"');
    return g_string_free(out, FALSE);
}

void settings_init(const char *path, SettingsApply apply, SettingsRestart restart)
{
    settings.path = g_strdup(path);
    settings.apply = apply;
    settings.restart = restart;
    settings.values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    settings.started = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    settings.pending = g_ptr_array_new();
    char *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return;
    char **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        const char *eq = strchr(lines[i], '=');
        if (lines[i][0] == '#' || !eq)
            continue;
        char *key = g_strndup(lines[i], (gsize)(eq - lines[i]));
        if (find_setting(key)) {
            g_hash_table_replace(settings.started, g_strdup(key), unquote(eq + 1));
            g_hash_table_replace(settings.values, key, unquote(eq + 1));
        } else
            g_free(key);
    }
    g_strfreev(lines);
    g_free(contents);
}

static const char *value_of(const char *key)
{
    const char *value = g_hash_table_lookup(settings.values, key);
    return value ? value : "";
}

void settings_save(const char *key, const char *value)
{
    g_hash_table_replace(settings.values, g_strdup(key), g_strdup(value));
    char *quoted = quote(value);
    char *line = g_strdup_printf("%s=%s", key, quoted);
    char *prefix = g_strdup_printf("%s=", key);
    char *contents = NULL;
    g_file_get_contents(settings.path, &contents, NULL, NULL);
    char **lines = g_strsplit(contents ? contents : "", "\n", -1);
    gboolean found = FALSE;
    for (int i = 0; lines[i]; i++)
        if (g_str_has_prefix(lines[i], prefix)) {
            g_free(lines[i]);
            lines[i] = g_strdup(found ? "" : line);
            found = TRUE;
        }
    char *joined = g_strjoinv("\n", lines);
    GString *out = g_string_new(joined);
    if (!found) {
        const Setting *setting = find_setting(key);
        if (out->len && out->str[out->len - 1] != '\n')
            g_string_append_c(out, '\n');
        if (setting)
            g_string_append_printf(out, "\n# %s: %s.\n", setting->title, setting->help);
        g_string_append_printf(out, "%s\n", line);
    }
    GError *error = NULL;
    if (!g_file_set_contents(settings.path, out->str, (gssize)out->len, &error)) {
        fprintf(stderr, "[wpe-tsp] settings: can't save %s: %s\n", settings.path, error->message);
        g_error_free(error);
    } else
        fprintf(stderr, "[wpe-tsp] settings: %s\n", line);
    g_string_free(out, TRUE);
    g_free(joined);
    g_strfreev(lines);
    g_free(contents);
    g_free(prefix);
    g_free(line);
    g_free(quoted);
}

/* Save and apply a value changed on the page */
static void change(const Setting *setting, const char *value)
{
    if (!strcmp(value_of(setting->key), value))
        return;
    settings_save(setting->key, value);
    if (settings.apply(setting->key, value))
        return;
    /* Applies at start: a restart is needed unless it's back to the value it started with */
    const char *started = g_hash_table_lookup(settings.started, setting->key);
    g_ptr_array_remove(settings.pending, (gpointer)setting->title);
    if (strcmp(started ? started : "", value))
        g_ptr_array_add(settings.pending, (gpointer)setting->title);
}

/* The choices offered for a setting: its own, plus the current value if it isn't one of them */
static GPtrArray *choices_of(const Setting *setting)
{
    GPtrArray *values = g_ptr_array_new();
    const char *current = value_of(setting->key);
    gboolean listed = FALSE;
    for (int i = 0; setting->choices[i]; i += 2)
        listed |= !strcmp(setting->choices[i], current);
    if (!listed)
        g_ptr_array_add(values, (gpointer)current);
    for (int i = 0; setting->choices[i]; i += 2)
        g_ptr_array_add(values, (gpointer)setting->choices[i]);
    return values;
}

static const char *label_of(const Setting *setting, const char *value)
{
    for (int i = 0; setting->choices && setting->choices[i]; i += 2)
        if (!strcmp(setting->choices[i], value))
            return setting->choices[i + 1];
    return *value ? value : "Default";
}

static void step(const Setting *setting, int dir)
{
    GPtrArray *values = choices_of(setting);
    guint i = 0;
    while (i < values->len && strcmp(g_ptr_array_index(values, i), value_of(setting->key)))
        i++;
    char *next = g_strdup(g_ptr_array_index(values, (i + values->len + (guint)dir) % values->len));
    g_ptr_array_free(values, TRUE);
    change(setting, next);
    g_free(next);
}

/* Without the browser's message handler: the page reloads itself after an action, with the changed
 * setting focused (#key). Dark, like the page: a white document would flash. */
static char *back_to_page(const char *key)
{
    return g_strdup_printf("<!DOCTYPE html><html style='background:#202124'><script>location.replace('"
                           SETTINGS_URI "%s%s')</script></html>", key ? "#" : "", key ? key : "");
}

/* Left/right or START on a choice: the previous/next value. START on a text: an input field (the
 * browser opens the on-screen keyboard for it); Enter saves, leaving it (B) cancels. A change goes to
 * the browser as a message (wpeTspSettings), which answers with the updated list: no reload, the
 * page keeps its place. (Restart is a navigation: the browser exits.) */
#define SETTINGS_SCRIPT \
    "<script>" \
    "function act(u){" \
    "const h=window.webkit&&window.webkit.messageHandlers&&window.webkit.messageHandlers.wpeTspSettings;" \
    "if(!h||u.endsWith('/restart')){location.replace(u);return;}" \
    "const id=u.split('/')[4].split('?')[0];" /* the setting: wpe-tsp://settings/<action>/<key> */ \
    "h.postMessage(u).then(html=>{document.getElementById('list').innerHTML=html;" \
    "const f=document.getElementById(id);if(f){f.focus();f.scrollIntoView({block:'nearest'});}});}" \
    "const target=()=>document.activeElement&&document.activeElement.closest('a');" \
    "function edit(a){" \
    "const i=document.createElement('input');i.value=a.dataset.value;i.className='field';" \
    "a.style.display='none';a.after(i);i.focus();" \
    "i.addEventListener('blur',()=>{i.remove();a.style.display='';});" \
    "i.addEventListener('keydown',e=>{e.stopImmediatePropagation();" \
    "if(e.key==='Enter'){e.preventDefault();act('" SETTINGS_URI "/set/'+a.id+'?v='+encodeURIComponent(i.value.trim()));}" \
    "else if(e.key==='Escape'){a.focus();}});}" \
    "function activate(a,e){if(!a)return;e.preventDefault();e.stopImmediatePropagation();" \
    "if(a.classList.contains('text'))edit(a);else act(a.href);}" \
    "document.addEventListener('keydown',e=>{const a=target();" \
    "if(e.key==='Enter')activate(a,e);" \
    "else if((e.key==='ArrowLeft'||e.key==='ArrowRight')&&a&&a.classList.contains('choice')){" \
    "e.preventDefault();e.stopImmediatePropagation();" \
    "act('" SETTINGS_URI "/'+(e.key==='ArrowLeft'?'prev':'next')+'/'+a.id);}});" \
    "document.addEventListener('click',e=>activate(e.target.closest('a'),e));" \
    "addEventListener('DOMContentLoaded',()=>{" \
    "document.querySelector('.hint').insertAdjacentHTML('beforeend',' &nbsp;&middot;&nbsp; <b>Left/Right</b> change');" \
    "const f=location.hash&&document.getElementById(location.hash.slice(1));" \
    "if(f){f.focus();f.scrollIntoView({block:'center'});}});" \
    "</script>"

/* The settings, from the restart offer to the last one: the part of the page a change replaces */
static char *list_html(const char *previous_uri)
{
    GString *html = g_string_new(NULL);
    if (settings.pending->len) {
        g_ptr_array_add(settings.pending, NULL);
        char *names = g_strjoinv(", ", (char **)settings.pending->pdata);
        g_ptr_array_set_size(settings.pending, settings.pending->len - 1);
        char *escaped = g_markup_escape_text(names, -1);
        g_string_append_printf(html, "<a id='restart' class='restart' href='" SETTINGS_URI "/restart'>"
                                     "<div class='t'>Restart the browser to apply</div><div class='s'>%s</div></a>", escaped);
        g_free(escaped);
        g_free(names);
    }
    for (guint i = 0; i < N_SETTINGS; i++) {
        const Setting *setting = &setting_list[i];
        const char *value = value_of(setting->key);
        if (setting->group)
            g_string_append_printf(html, "<h2>%s</h2>", setting->group);
        char *title = g_markup_escape_text(setting->title, -1);
        char *help = g_markup_escape_text(setting->help, -1);
        char *shown = g_markup_escape_text(setting->choices ? label_of(setting, value) : *value ? value : "Not set", -1);
        if (setting->choices) {
            g_string_append_printf(html, "<a id='%s' class='choice' href='" SETTINGS_URI "/next/%s'><div class='t'>%s"
                                         "<span class='v'>&#9664; %s &#9654;</span></div><div class='s'>%s</div></a>",
                                   setting->key, setting->key, title, shown, help);
        } else {
            char *attr = g_markup_escape_text(value, -1);
            g_string_append_printf(html, "<a id='%s' class='text' href='#' data-value='%s'><div class='t'>%s"
                                         "<span class='v'>%s</span></div><div class='s'>%s</div></a>",
                                   setting->key, attr, title, shown, help);
            g_free(attr);
        }
        /* The page the settings were opened from, as the home page */
        if (!strcmp(setting->key, "HOME_URL") && previous_uri && strcmp(previous_uri, value) &&
            (g_str_has_prefix(previous_uri, "http://") || g_str_has_prefix(previous_uri, "https://"))) {
            char *escaped_uri = g_uri_escape_string(previous_uri, NULL, FALSE);
            char *escaped = g_markup_escape_text(previous_uri, -1);
            g_string_append_printf(html, "<a id='HOME_URL_PREVIOUS' class='set' href='" SETTINGS_URI "/set/HOME_URL?v=%s'>"
                                         "<div class='t'>Use the previous page as home</div><div class='s'>%s</div></a>",
                                   escaped_uri, escaped);
            g_free(escaped);
            g_free(escaped_uri);
        }
        g_free(shown);
        g_free(help);
        g_free(title);
    }
    return g_string_free(html, FALSE);
}

static char *page_html(const char *previous_uri)
{
    GString *html = g_string_new(NULL);
    g_string_append_printf(html, PAGE_HEAD, "Settings");
    g_string_append(html, "<style>.v{float:right;margin-left:16px;color:#8ab4f8}a:focus .v{color:#202124}"
                          "h2{font-size:.9em;color:#9aa0a6;margin:18px 14px 4px;font-weight:normal}"
                          ".field{box-sizing:border-box;width:100%;font-size:1.05em;padding:10px 14px;border-radius:8px;"
                          "border:2px solid #8ab4f8;background:#303134;color:#e8eaed;outline:none}"
                          ".restart{color:#fdd663}</style>");
    char *list = list_html(previous_uri);
    g_string_append_printf(html, SETTINGS_SCRIPT "<h1>Settings</h1><div id='list'>%s</div>", list);
    g_free(list);
    g_string_append(html, PAGE_TAIL);
    return g_string_free(html, FALSE);
}

/* /next/<key>, /prev/<key>, /set/<key>?v=<value>: performs it. Returns the key (NULL if none). */
static char *perform(const char *action)
{
    if (!g_str_has_prefix(action, "/next/") && !g_str_has_prefix(action, "/prev/") && !g_str_has_prefix(action, "/set/"))
        return NULL;
    const char *name = strchr(action + 1, '/') + 1;
    char *key = g_strndup(name, strcspn(name, "?#"));
    const Setting *setting = find_setting(key);
    if (setting && setting->choices && g_str_has_prefix(action, "/next/"))
        step(setting, 1);
    else if (setting && setting->choices && g_str_has_prefix(action, "/prev/"))
        step(setting, -1);
    else if (setting && g_str_has_prefix(action, "/set/")) {
        const char *v = strstr(action, "?v=");
        char *value = g_uri_unescape_string(v ? v + 3 : "", NULL);
        if (value)
            change(setting, value);
        g_free(value);
    }
    return key;
}

char *settings_handle_page(const char *uri, const char *previous_uri)
{
    const char *action = uri + strlen(SETTINGS_URI);
    if (!strcmp(action, "/restart")) {
        settings.restart();
        return g_strdup("<!DOCTYPE html><body style='background:#202124'></body>");
    }
    char *key = perform(action);
    if (!key)
        return page_html(previous_uri);
    char *html = back_to_page(key);
    g_free(key);
    return html;
}

char *settings_handle_message(const char *uri, const char *previous_uri)
{
    if (g_str_has_prefix(uri, SETTINGS_URI))
        g_free(perform(uri + strlen(SETTINGS_URI)));
    return list_html(previous_uri);
}
