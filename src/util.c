// CWTNote - small helpers for paths, config and files.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "util.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

char *nf_config_path(void) {
    return g_build_filename(g_get_user_config_dir(), "cwtnote", "config.conf", NULL);
}

char *nf_config_get_vault(void) {
    char *path = nf_config_path();
    gsize len = 0;
    char *data = nf_read_file(path, &len);
    g_free(path);

    if (data == NULL)
        return NULL;

    char *vault = NULL;
    char **lines = g_strsplit(data, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        char *line = g_strstrip(lines[i]);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
            continue;
        char *eq = strchr(line, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        char *key = g_strstrip(line);
        char *value = g_strstrip(eq + 1);
        if (strcmp(key, "vault") == 0 && value[0] != '\0') {
            g_free(vault);
            vault = g_strdup(value);
        }
    }
    g_strfreev(lines);
    g_free(data);
    return vault;
}

gboolean nf_config_set_vault(const char *path) {
    char *cfg_path = nf_config_path();
    char *dir = g_path_get_dirname(cfg_path);
    gboolean ok = nf_mkdir(dir, NULL);
    chmod(dir, 0700);
    g_free(dir);

    if (ok) {
        char *data = g_strdup_printf("# CWTNote configuration\nvault=%s\n", path);
        ok = g_file_set_contents_full(cfg_path, data, -1,
                                      G_FILE_SET_CONTENTS_CONSISTENT |
                                          G_FILE_SET_CONTENTS_DURABLE,
                                      0600, NULL);
        chmod(cfg_path, 0600);
        g_free(data);
    }

    g_free(cfg_path);
    return ok;
}

char *nf_expand_path(const char *path) {
    if (path == NULL || path[0] == '\0')
        return NULL;

    char *expanded;
    if (g_str_has_prefix(path, "~/"))
        expanded = g_build_filename(g_get_home_dir(), path + 2, NULL);
    else if (strcmp(path, "~") == 0)
        expanded = g_strdup(g_get_home_dir());
    else
        expanded = g_strdup(path);

    char *absolute = g_canonicalize_filename(expanded, NULL);
    g_free(expanded);
    return absolute;
}

gboolean nf_mkdir(const char *path, char **err) {
    if (g_mkdir_with_parents(path, 0755) != 0) {
        if (err != NULL)
            *err = g_strdup_printf("Could not create directory %s: %s", path,
                                   g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

char *nf_read_file(const char *path, gsize *len_out) {
    char *data = NULL;
    gsize len = 0;

    if (!g_file_get_contents(path, &data, &len, NULL))
        return NULL;

    if (len_out != NULL)
        *len_out = len;
    return data;
}

gboolean nf_write_file(const char *path, const char *data, gsize len, char **err) {
    GError *gerr = NULL;
    gboolean ok = g_file_set_contents_full(path, data, (gssize)len,
                                           G_FILE_SET_CONTENTS_CONSISTENT |
                                               G_FILE_SET_CONTENTS_DURABLE,
                                           0644, &gerr);
    if (!ok && err != NULL) {
        *err = g_strdup(gerr != NULL ? gerr->message : "Unknown write error");
    }
    g_clear_error(&gerr);
    return ok;
}

char *nf_trim_dup(const char *text) {
    if (text == NULL)
        return g_strdup("");
    char *copy = g_strdup(text);
    return g_strstrip(copy);
}

char *nf_collapse_whitespace(const char *text) {
    GString *out = g_string_new(NULL);
    gboolean pending_space = FALSE;

    for (const char *p = text; p != NULL && *p != '\0'; p = g_utf8_next_char(p)) {
        gunichar ch = g_utf8_get_char(p);
        if (g_unichar_isspace(ch)) {
            if (out->len > 0)
                pending_space = TRUE;
            continue;
        }
        if (pending_space) {
            g_string_append_c(out, ' ');
            pending_space = FALSE;
        }
        g_string_append_unichar(out, ch);
    }

    return g_string_free(out, FALSE);
}

char *nf_utf8_prefix(const char *text, guint n) {
    if (text == NULL)
        return g_strdup("");

    const char *end = text;
    for (guint i = 0; i < n && *end != '\0'; i++)
        end = g_utf8_next_char(end);

    return g_strndup(text, (gsize)(end - text));
}

char *nf_timestamp_name(void) {
    GDateTime *now = g_date_time_new_now_local();
    if (now == NULL)
        return g_strdup("note");

    char *name = g_date_time_format(now, "%Y-%m-%d-%H%M%S");
    g_date_time_unref(now);
    return name;
}

char *nf_sanitize_name(const char *name) {
    char *trimmed = nf_trim_dup(name);
    GString *out = g_string_new(NULL);

    for (const char *p = trimmed; *p != '\0'; p = g_utf8_next_char(p)) {
        gunichar ch = g_utf8_get_char(p);
        if (ch == '/' || ch == '\\' || ch < 0x20)
            g_string_append_c(out, '-');
        else
            g_string_append_unichar(out, ch);
    }
    g_free(trimmed);

    // Avoid hidden files and trailing dots/spaces.
    while (out->len > 0 && (out->str[0] == '.' || out->str[0] == ' '))
        g_string_erase(out, 0, 1);
    while (out->len > 0 &&
           (out->str[out->len - 1] == '.' || out->str[out->len - 1] == ' '))
        g_string_truncate(out, out->len - 1);

    if (out->len == 0)
        g_string_append(out, "Untitled");

    return g_string_free(out, FALSE);
}

char *nf_title_from_relpath(const char *relpath) {
    char *base = g_path_get_basename(relpath);
    if (g_str_has_suffix(base, ".md"))
        base[strlen(base) - 3] = '\0';
    return base;
}

char *nf_parent_relpath(const char *relpath) {
    char *parent = g_path_get_dirname(relpath);
    if (strcmp(parent, ".") == 0) {
        g_free(parent);
        return g_strdup("");
    }
    return parent;
}

char *nf_relpath_join(const char *dir, const char *name) {
    if (dir == NULL || dir[0] == '\0')
        return g_strdup(name);
    return g_build_filename(dir, name, NULL);
}
