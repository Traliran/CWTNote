// Noteflow - vault access: notes, folders, links.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "vault.h"
#include "util.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#define QUICK_DIR "Inbox"

NfVault *nf_vault_open(const char *root) {
    if (root == NULL || root[0] == '\0')
        return NULL;

    NfVault *vault = g_new0(NfVault, 1);
    vault->root = g_canonicalize_filename(root, NULL);
    return vault;
}

void nf_vault_free(NfVault *vault) {
    if (vault == NULL)
        return;
    g_free(vault->root);
    g_free(vault);
}

char *nf_vault_abs(const NfVault *vault, const char *relpath) {
    if (relpath == NULL || relpath[0] == '\0')
        return g_strdup(vault->root);
    return g_build_filename(vault->root, relpath, NULL);
}

static NfEntry *entry_new(const char *name, const char *relpath,
                          gboolean is_dir) {
    NfEntry *entry = g_new0(NfEntry, 1);
    entry->name = g_strdup(name);
    entry->relpath = g_strdup(relpath);
    entry->is_dir = is_dir;
    return entry;
}

void nf_entry_free(NfEntry *entry) {
    if (entry == NULL)
        return;
    g_free(entry->name);
    g_free(entry->relpath);
    g_free(entry);
}

void nf_entry_list_free(GList *entries) {
    g_list_free_full(entries, (GDestroyNotify)nf_entry_free);
}

static gint entry_compare(gconstpointer a, gconstpointer b) {
    const NfEntry *left = a;
    const NfEntry *right = b;
    if (left->is_dir != right->is_dir)
        return left->is_dir ? -1 : 1;
    return g_utf8_collate(left->name, right->name);
}

GList *nf_vault_list_dir(const NfVault *vault, const char *relpath) {
    char *dir_path = nf_vault_abs(vault, relpath);
    GError *gerr = NULL;
    GDir *dir = g_dir_open(dir_path, 0, &gerr);
    if (dir == NULL) {
        g_clear_error(&gerr);
        g_free(dir_path);
        return NULL;
    }

    GList *entries = NULL;
    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (name[0] == '.')
            continue;

        char *child_path = g_build_filename(dir_path, name, NULL);
        GStatBuf stat_buf;
        if (g_lstat(child_path, &stat_buf) != 0) {
            g_free(child_path);
            continue;
        }

        // Do not follow symlinks, they can create cycles in a vault.
        if (S_ISLNK(stat_buf.st_mode)) {
            g_free(child_path);
            continue;
        }

        char *child_rel = nf_relpath_join(relpath, name);
        if (S_ISDIR(stat_buf.st_mode)) {
            entries = g_list_append(entries,
                                    entry_new(name, child_rel, TRUE));
        } else if (S_ISREG(stat_buf.st_mode) &&
                   g_str_has_suffix(name, ".md")) {
            entries = g_list_append(entries,
                                    entry_new(name, child_rel, FALSE));
        }

        g_free(child_rel);
        g_free(child_path);
    }
    g_dir_close(dir);
    g_free(dir_path);

    return g_list_sort(entries, entry_compare);
}

static void collect_notes(const NfVault *vault, const char *dir_relpath,
                          GList **notes) {
    GList *entries = nf_vault_list_dir(vault, dir_relpath);

    for (GList *l = entries; l != NULL; l = l->next) {
        NfEntry *entry = l->data;
        if (entry->is_dir) {
            collect_notes(vault, entry->relpath, notes);
            nf_entry_free(entry);
        } else {
            *notes = g_list_append(*notes, entry);
        }
    }

    g_list_free(entries);
}

static gint note_compare(gconstpointer a, gconstpointer b) {
    const NfEntry *left = a;
    const NfEntry *right = b;
    return g_utf8_collate(left->relpath, right->relpath);
}

GList *nf_vault_all_notes(const NfVault *vault) {
    GList *notes = NULL;
    collect_notes(vault, "", &notes);
    return g_list_sort(notes, note_compare);
}

static char *unique_path(const char *dir_abs, const char *stem,
                         const char *extension) {
    char *candidate = g_strconcat(stem, extension, NULL);
    char *path = g_build_filename(dir_abs, candidate, NULL);

    for (guint n = 2; g_file_test(path, G_FILE_TEST_EXISTS); n++) {
        g_free(candidate);
        g_free(path);
        candidate = g_strdup_printf("%s %u%s", stem, n, extension);
        path = g_build_filename(dir_abs, candidate, NULL);
    }

    g_free(candidate);
    return path;
}

char *nf_vault_create_note(NfVault *vault, const char *dir_relpath,
                           const char *title, char **err) {
    if (err != NULL)
        *err = NULL;

    char *safe_title = nf_sanitize_name(title);
    char *dir_abs = nf_vault_abs(vault, dir_relpath);

    if (!nf_mkdir(dir_abs, err)) {
        g_free(safe_title);
        g_free(dir_abs);
        return NULL;
    }

    char *path = unique_path(dir_abs, safe_title, ".md");
    char *filename = g_path_get_basename(path);

    gboolean ok = nf_write_file(path, "", 0, err);
    char *relpath = ok ? nf_relpath_join(dir_relpath, filename) : NULL;

    g_free(filename);
    g_free(path);
    g_free(dir_abs);
    g_free(safe_title);
    return relpath;
}

char *nf_vault_create_folder(NfVault *vault, const char *dir_relpath,
                             const char *name, char **err) {
    if (err != NULL)
        *err = NULL;

    char *safe_name = nf_sanitize_name(name);
    char *dir_abs = nf_vault_abs(vault, dir_relpath);

    if (!nf_mkdir(dir_abs, err)) {
        g_free(safe_name);
        g_free(dir_abs);
        return NULL;
    }

    char *path = unique_path(dir_abs, safe_name, "");
    char *folder = g_path_get_basename(path);
    gboolean ok = nf_mkdir(path, err);
    char *relpath = ok ? nf_relpath_join(dir_relpath, folder) : NULL;

    g_free(folder);
    g_free(path);
    g_free(dir_abs);
    g_free(safe_name);
    return relpath;
}

char *nf_vault_rename(NfVault *vault, const char *relpath,
                      const char *new_name, char **err) {
    if (err != NULL)
        *err = NULL;

    char *old_abs = nf_vault_abs(vault, relpath);
    gboolean is_dir = g_file_test(old_abs, G_FILE_TEST_IS_DIR);
    char *safe_name = nf_sanitize_name(new_name);
    char *filename = is_dir ? g_strdup(safe_name)
                            : g_strconcat(safe_name, ".md", NULL);
    char *parent = nf_parent_relpath(relpath);
    char *new_relpath = nf_relpath_join(parent, filename);
    char *new_abs = nf_vault_abs(vault, new_relpath);

    gboolean ok = TRUE;
    if (strcmp(old_abs, new_abs) != 0) {
        if (g_file_test(new_abs, G_FILE_TEST_EXISTS)) {
            if (err != NULL)
                *err = g_strdup("A note or folder with that name already exists.");
            ok = FALSE;
        } else if (g_rename(old_abs, new_abs) != 0) {
            if (err != NULL)
                *err = g_strdup("Could not rename the item.");
            ok = FALSE;
        }
    }

    g_free(old_abs);
    g_free(new_abs);
    g_free(parent);
    g_free(filename);
    g_free(safe_name);

    if (!ok) {
        g_free(new_relpath);
        return NULL;
    }
    return new_relpath;
}

gboolean nf_vault_delete(NfVault *vault, const char *relpath, char **err) {
    if (err != NULL)
        *err = NULL;

    char *path = nf_vault_abs(vault, relpath);
    GFile *file = g_file_new_for_path(path);
    GError *gerr = NULL;
    gboolean ok = g_file_trash(file, NULL, &gerr);

    if (!ok && err != NULL)
        *err = g_strdup(gerr != NULL ? gerr->message : "Could not delete the item.");

    g_clear_error(&gerr);
    g_object_unref(file);
    g_free(path);
    return ok;
}

char *nf_vault_quick_note(NfVault *vault, const char *text, char **err) {
    if (err != NULL)
        *err = NULL;

    char *inbox_abs = nf_vault_abs(vault, QUICK_DIR);
    if (!nf_mkdir(inbox_abs, err)) {
        g_free(inbox_abs);
        return NULL;
    }

    char *timestamp = nf_timestamp_name();
    char *path = unique_path(inbox_abs, timestamp, ".md");
    char *filename = g_path_get_basename(path);

    char *trimmed = nf_trim_dup(text);
    GString *content = g_string_new(trimmed);
    if (content->len > 0 && content->str[content->len - 1] != '\n')
        g_string_append_c(content, '\n');

    gboolean ok = nf_write_file(path, content->str, content->len, err);
    char *relpath =
        ok ? nf_relpath_join(QUICK_DIR, filename) : NULL;

    g_string_free(content, TRUE);
    g_free(trimmed);
    g_free(filename);
    g_free(path);
    g_free(timestamp);
    g_free(inbox_abs);
    return relpath;
}

void nf_link_free(NfLink *link) {
    if (link == NULL)
        return;
    g_free(link->relpath);
    g_free(link->title);
    g_free(link);
}

void nf_link_list_free(GList *links) {
    g_list_free_full(links, (GDestroyNotify)nf_link_free);
}

static gint link_compare(gconstpointer a, gconstpointer b) {
    const NfLink *left = a;
    const NfLink *right = b;
    return g_utf8_collate(left->title, right->title);
}

GList *nf_vault_backlinks(const NfVault *vault, const char *title,
                          const char *exclude_relpath) {
    char *title_folded = g_utf8_casefold(title, -1);
    GList *notes = nf_vault_all_notes(vault);
    GList *links = NULL;
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);

    for (GList *l = notes; l != NULL; l = l->next) {
        NfEntry *entry = l->data;
        if (exclude_relpath != NULL &&
            strcmp(entry->relpath, exclude_relpath) == 0)
            continue;
        if (g_hash_table_contains(seen, entry->relpath))
            continue;

        char *path = nf_vault_abs(vault, entry->relpath);
        char *content = nf_read_file(path, NULL);
        g_free(path);
        if (content == NULL)
            continue;

        gboolean found = FALSE;
        const char *cursor = content;
        while ((cursor = strstr(cursor, "[[")) != NULL) {
            // Skip embeds: ![[image.png]]
            if (cursor > content && cursor[-1] == '!') {
                cursor += 2;
                continue;
            }

            const char *start = cursor + 2;
            const char *end = strstr(start, "]]");
            if (end == NULL)
                break;

            const char *stop = start;
            while (stop < end && *stop != '|' && *stop != '#')
                stop++;

            char *target = g_strndup(start, (gsize)(stop - start));
            g_strstrip(target);

            if (target[0] != '\0') {
                const char *base = strrchr(target, '/');
                base = base != NULL ? base + 1 : target;
                char *base_folded = g_utf8_casefold(base, -1);
                if (strcmp(base_folded, title_folded) == 0)
                    found = TRUE;
                g_free(base_folded);
            }

            g_free(target);
            cursor = end + 2;
            if (found)
                break;
        }

        g_free(content);

        if (found) {
            NfLink *link = g_new0(NfLink, 1);
            link->relpath = g_strdup(entry->relpath);
            link->title = nf_title_from_relpath(entry->relpath);
            links = g_list_append(links, link);
            g_hash_table_add(seen, g_strdup(entry->relpath));
        }
    }

    g_hash_table_destroy(seen);
    nf_entry_list_free(notes);
    g_free(title_folded);

    links = g_list_sort(links, link_compare);
    return links;
}

char *nf_vault_resolve_link(const NfVault *vault, const char *target) {
    if (target == NULL)
        return NULL;

    char *copy = g_strdup(target);
    char *trimmed = g_strstrip(copy);
    char *bar = strchr(trimmed, '|');
    if (bar != NULL)
        *bar = '\0';
    char *hash = strchr(trimmed, '#');
    if (hash != NULL)
        *hash = '\0';
    g_strstrip(trimmed);

    if (trimmed[0] == '\0') {
        g_free(copy);
        return NULL;
    }

    const char *base = strrchr(trimmed, '/');
    base = base != NULL ? base + 1 : trimmed;

    char *target_folded = g_utf8_casefold(trimmed, -1);
    char *base_folded = g_utf8_casefold(base, -1);
    char *result = NULL;

    GList *notes = nf_vault_all_notes(vault);
    for (GList *l = notes; l != NULL && result == NULL; l = l->next) {
        NfEntry *entry = l->data;
        char *title = nf_title_from_relpath(entry->relpath);
        char *title_folded = g_utf8_casefold(title, -1);
        char *relpath_no_ext = g_strdup(entry->relpath);
        if (g_str_has_suffix(relpath_no_ext, ".md"))
            relpath_no_ext[strlen(relpath_no_ext) - 3] = '\0';
        char *relpath_folded = g_utf8_casefold(relpath_no_ext, -1);

        if (strcmp(relpath_folded, target_folded) == 0 ||
            strcmp(title_folded, base_folded) == 0)
            result = g_strdup(entry->relpath);

        g_free(relpath_folded);
        g_free(relpath_no_ext);
        g_free(title_folded);
        g_free(title);
    }

    nf_entry_list_free(notes);
    g_free(base_folded);
    g_free(target_folded);
    g_free(copy);
    return result;
}
