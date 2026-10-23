// Noteflow - small helpers for paths, config and files.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NF_UTIL_H
#define NF_UTIL_H

#include <glib.h>

// Path of the per-user config file ($XDG_CONFIG_HOME/noteflow/config.conf).
char *nf_config_path(void);

// Vault path stored in the config file, or NULL when not configured.
char *nf_config_get_vault(void);

// Persist the vault path with mode 0600.
gboolean nf_config_set_vault(const char *path);

// Expand a leading "~/" and make the path absolute. Caller frees the result.
char *nf_expand_path(const char *path);

// Create a directory and all parents. On failure sets *err (caller frees).
gboolean nf_mkdir(const char *path, char **err);

// Read a whole file. Returns NULL on error. Caller frees the result.
char *nf_read_file(const char *path, gsize *len_out);

// Write a file atomically. On failure sets *err (caller frees).
gboolean nf_write_file(const char *path, const char *data, gsize len, char **err);

// Trim leading/trailing whitespace. Caller frees the result.
char *nf_trim_dup(const char *text);

// Collapse all whitespace runs into single spaces. Caller frees the result.
char *nf_collapse_whitespace(const char *text);

// Copy at most n UTF-8 characters from the beginning of text.
char *nf_utf8_prefix(const char *text, guint n);

// Local timestamp used for quick notes: YYYY-MM-DD-HHMMSS.
char *nf_timestamp_name(void);

// Turn an arbitrary title into a safe file name (without extension).
char *nf_sanitize_name(const char *name);

// Basename of a vault-relative path without the .md extension.
char *nf_title_from_relpath(const char *relpath);

// Parent directory of a vault-relative path ("" for top level).
char *nf_parent_relpath(const char *relpath);

// Join two vault-relative paths ("dir/file.md").
char *nf_relpath_join(const char *dir, const char *name);

#endif
