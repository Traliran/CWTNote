// Noteflow - vault access: notes, folders, links.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NF_VAULT_H
#define NF_VAULT_H

#include <glib.h>

typedef struct {
    char *name;    // file or folder name
    char *relpath; // path relative to the vault root
    gboolean is_dir;
} NfEntry;

typedef struct {
    char *root;
} NfVault;

typedef struct {
    char *relpath;
    char *title;
} NfLink;

NfVault *nf_vault_open(const char *root);
void nf_vault_free(NfVault *vault);

char *nf_vault_abs(const NfVault *vault, const char *relpath);

// Immediate children of a vault-relative directory, sorted (folders first).
// Returns NULL when the directory cannot be read.
GList *nf_vault_list_dir(const NfVault *vault, const char *relpath);

// Every note in the vault, sorted by path.
GList *nf_vault_all_notes(const NfVault *vault);

// Create an empty note. Returns its new relative path, or NULL on error.
char *nf_vault_create_note(NfVault *vault, const char *dir_relpath,
                           const char *title, char **err);

// Create a folder. Returns its new relative path, or NULL on error.
char *nf_vault_create_folder(NfVault *vault, const char *dir_relpath,
                             const char *name, char **err);

// Rename a note or folder. Returns the new relative path, or NULL on error.
char *nf_vault_rename(NfVault *vault, const char *relpath,
                      const char *new_name, char **err);

// Move a note or folder to the trash.
gboolean nf_vault_delete(NfVault *vault, const char *relpath, char **err);

// Create Inbox/YYYY-MM-DD-HHMMSS.md and return its relative path.
char *nf_vault_quick_note(NfVault *vault, const char *text, char **err);

// Notes containing a [[wikilink]] to title, excluding exclude_relpath.
GList *nf_vault_backlinks(const NfVault *vault, const char *title,
                          const char *exclude_relpath);

// Resolve a wikilink target to a note relative path, or NULL.
char *nf_vault_resolve_link(const NfVault *vault, const char *target);

void nf_entry_free(NfEntry *entry);
void nf_entry_list_free(GList *entries);
void nf_link_free(NfLink *link);
void nf_link_list_free(GList *links);

#endif
