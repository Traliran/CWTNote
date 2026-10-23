// CWTNote - local Git history for a vault (system git binary).
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NF_GIT_H
#define NF_GIT_H

#include <glib.h>

typedef struct {
    char *hash;
    char *short_hash;
    char *author;
    char *date;
    char *subject;
} NfCommit;

// TRUE when the git binary is available in PATH.
gboolean nf_git_available(void);

// TRUE when the vault itself is the root of a git repository.
gboolean nf_git_is_repo(const char *vault);

// Initialise a repository if needed and make sure an identity exists.
gboolean nf_git_init(const char *vault, char **err);

// Stage every change and create a commit. Returns the commit message, or NULL
// when there was nothing to commit. On failure returns NULL and sets *err.
char *nf_git_commit_all(const char *vault, char **err);

// History of one vault-relative file, newest first. Returns NULL on error.
GList *nf_git_log(const char *vault, const char *relpath, guint limit,
                  char **err);

// Content of relpath at the given commit. Returns NULL on error.
char *nf_git_show_file(const char *vault, const char *commit,
                       const char *relpath, gsize *len_out, char **err);

// Revert the most recent commit as a new commit.
gboolean nf_git_revert_last(const char *vault, char **err);

void nf_commit_free(NfCommit *commit);
void nf_commit_list_free(GList *commits);

#endif
