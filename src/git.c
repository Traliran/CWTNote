// CWTNote - local Git history for a vault (system git binary).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "git.h"
#include "util.h"

#include <string.h>
#include <sys/wait.h>

#define COMMIT_PREFIX_CHARS 15

static void set_error(char **err, const char *message) {
    if (err == NULL)
        return;
    g_free(*err);
    *err = g_strdup((message != NULL && message[0] != '\0')
                        ? message
                        : "Git command failed.");
}

static char *strip_copy(const char *text) {
    char *copy = g_strdup(text != NULL ? text : "");
    return g_strstrip(copy);
}

// Run "git -C vault <args>" and capture stdout/stderr.
// Returns FALSE only when the process could not be started.
static gboolean run_git(const char *vault, const char *const *args, char **out,
                        char **err_out, int *exit_status) {
    gsize n = 0;
    while (args[n] != NULL)
        n++;

    char **argv = g_new0(char *, n + 4);
    argv[0] = (char *)"git";
    argv[1] = (char *)"-C";
    argv[2] = (char *)vault;
    for (gsize i = 0; i < n; i++)
        argv[i + 3] = (char *)args[i];

    char *stdout_buf = NULL;
    char *stderr_buf = NULL;
    GError *gerr = NULL;
    int status = 0;

    gboolean started = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
                                    NULL, &stdout_buf, &stderr_buf, &status,
                                    &gerr);
    g_free(argv);

    if (!started) {
        set_error(err_out, gerr != NULL ? gerr->message : "Could not run git.");
        g_clear_error(&gerr);
        g_free(stdout_buf);
        g_free(stderr_buf);
        return FALSE;
    }

    if (out != NULL)
        *out = stdout_buf;
    else
        g_free(stdout_buf);

    if (err_out != NULL)
        *err_out = stderr_buf;
    else
        g_free(stderr_buf);

    if (exit_status != NULL)
        *exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return TRUE;
}

gboolean nf_git_available(void) {
    char *git = g_find_program_in_path("git");
    gboolean found = git != NULL;
    g_free(git);
    return found;
}

gboolean nf_git_is_repo(const char *vault) {
    char *out = NULL;
    char *err = NULL;
    int status = 0;

    if (!run_git(vault, (const char *[]){"rev-parse", "--show-toplevel", NULL},
                 &out, &err, &status))
        return FALSE;

    gboolean same = FALSE;
    if (status == 0) {
        char *top = strip_copy(out);
        char *top_canonical = g_canonicalize_filename(top, NULL);
        char *vault_canonical = g_canonicalize_filename(vault, NULL);
        same = g_strcmp0(top_canonical, vault_canonical) == 0;
        g_free(top);
        g_free(top_canonical);
        g_free(vault_canonical);
    }

    g_free(out);
    g_free(err);
    return same;
}

static gboolean ensure_identity(const char *vault, char **err) {
    struct {
        const char *key;
        const char *fallback;
    } const settings[] = {
        {"user.name", "CWTNote"},
        {"user.email", "cwtnote@localhost"},
    };

    for (gsize i = 0; i < G_N_ELEMENTS(settings); i++) {
        char *out = NULL;
        char *gerr = NULL;
        int status = 0;
        gboolean started = run_git(
            vault, (const char *[]){"config", "--get", settings[i].key, NULL},
            &out, &gerr, &status);
        if (!started) {
            g_free(out);
            g_free(gerr);
            set_error(err, "Could not check the git identity.");
            return FALSE;
        }

        char *value = strip_copy(out);
        gboolean missing = status != 0 || value[0] == '\0';
        g_free(value);
        g_free(out);
        g_free(gerr);

        if (!missing)
            continue;

        out = NULL;
        gerr = NULL;
        status = 0;
        started = run_git(vault,
                          (const char *[]){"config", settings[i].key,
                                           settings[i].fallback, NULL},
                          &out, &gerr, &status);
        if (!started || status != 0) {
            char *message = strip_copy(gerr);
            set_error(err, message);
            g_free(message);
            g_free(out);
            g_free(gerr);
            return FALSE;
        }
        g_free(out);
        g_free(gerr);
    }

    return TRUE;
}

gboolean nf_git_init(const char *vault, char **err) {
    if (!nf_git_available()) {
        set_error(err, "The git executable was not found in PATH.");
        return FALSE;
    }

    if (!nf_git_is_repo(vault)) {
        char *out = NULL;
        char *gerr = NULL;
        int status = 0;
        gboolean started = run_git(
            vault, (const char *[]){"init", "-q", NULL}, &out, &gerr, &status);
        if (!started || status != 0) {
            char *message = strip_copy(gerr);
            set_error(err, message);
            g_free(message);
            g_free(out);
            g_free(gerr);
            return FALSE;
        }
        g_free(out);
        g_free(gerr);
    }

    return ensure_identity(vault, err);
}

static char *build_commit_message(const char *diff) {
    GString *added = g_string_new(NULL);

    char **lines = g_strsplit(diff, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        if (lines[i][0] == '+' && !g_str_has_prefix(lines[i], "+++ ")) {
            g_string_append(added, lines[i] + 1);
            g_string_append_c(added, ' ');
        }
    }
    g_strfreev(lines);

    char *normalized = nf_collapse_whitespace(added->str);
    g_string_free(added, TRUE);

    char *prefix = nf_utf8_prefix(normalized, COMMIT_PREFIX_CHARS);
    char *message = g_strconcat(prefix[0] != '\0' ? prefix : "Update notes",
                                "...", NULL);
    g_free(prefix);
    g_free(normalized);
    return message;
}

char *nf_git_commit_all(const char *vault, char **err) {
    if (err != NULL)
        *err = NULL;

    char *out = NULL;
    char *gerr = NULL;
    int status = 0;

    if (!run_git(vault, (const char *[]){"add", "-A", NULL}, &out, &gerr,
                 &status)) {
        set_error(err, "Could not run git add.");
        g_free(out);
        g_free(gerr);
        return NULL;
    }
    g_free(out);
    out = NULL;

    if (status != 0) {
        char *message = strip_copy(gerr);
        set_error(err, message);
        g_free(message);
        g_free(gerr);
        return NULL;
    }
    g_free(gerr);
    gerr = NULL;

    status = 0;
    if (!run_git(vault,
                 (const char *[]){"diff", "--cached", "--quiet", NULL}, &out,
                 &gerr, &status)) {
        set_error(err, "Could not inspect staged changes.");
        g_free(out);
        g_free(gerr);
        return NULL;
    }
    g_free(gerr);
    gerr = NULL;

    if (status == 0) { // nothing staged
        g_free(out);
        return NULL;
    }
    if (status != 1) {
        set_error(err, "Could not inspect staged changes.");
        g_free(out);
        return NULL;
    }
    g_free(out);
    out = NULL;

    if (!run_git(vault,
                 (const char *[]){"diff", "--cached", "--unified=0",
                                  "--no-color", "--no-ext-diff", NULL},
                 &out, &gerr, &status)) {
        set_error(err, "Could not read the staged diff.");
        g_free(out);
        g_free(gerr);
        return NULL;
    }
    if (status != 0) {
        char *message = strip_copy(gerr);
        set_error(err, message);
        g_free(message);
        g_free(out);
        g_free(gerr);
        return NULL;
    }
    g_free(gerr);
    gerr = NULL;

    char *message = build_commit_message(out);
    g_free(out);
    out = NULL;

    if (!run_git(vault,
                 (const char *[]){"-c", "commit.gpgsign=false", "commit", "-q",
                                  "-m", message, NULL},
                 &out, &gerr, &status)) {
        set_error(err, "Could not run git commit.");
        g_free(message);
        g_free(out);
        g_free(gerr);
        return NULL;
    }
    if (status != 0) {
        char *error_text = strip_copy(gerr);
        set_error(err, error_text);
        g_free(error_text);
        g_free(message);
        g_free(out);
        g_free(gerr);
        return NULL;
    }

    g_free(out);
    g_free(gerr);
    return message;
}

static void commit_free(NfCommit *commit) {
    if (commit == NULL)
        return;
    g_free(commit->hash);
    g_free(commit->short_hash);
    g_free(commit->author);
    g_free(commit->date);
    g_free(commit->subject);
    g_free(commit);
}

void nf_commit_free(NfCommit *commit) { commit_free(commit); }

void nf_commit_list_free(GList *commits) {
    g_list_free_full(commits, (GDestroyNotify)commit_free);
}

GList *nf_git_log(const char *vault, const char *relpath, guint limit,
                  char **err) {
    if (err != NULL)
        *err = NULL;

    char *limit_text = g_strdup_printf("%u", limit);
    char *out = NULL;
    char *gerr = NULL;
    int status = 0;

    gboolean started = run_git(
        vault,
        (const char *[]){"log", "--follow", "-n", limit_text, "--date=short",
                         "--format=%H%x1f%h%x1f%an%x1f%ad%x1f%s", "--",
                         relpath, NULL},
        &out, &gerr, &status);
    g_free(limit_text);

    if (!started) {
        set_error(err, "Could not run git log.");
        g_free(out);
        g_free(gerr);
        return NULL;
    }

    if (status != 0) {
        gboolean no_commits =
            gerr != NULL && (strstr(gerr, "does not have any commits") != NULL ||
                             strstr(gerr, "unknown revision") != NULL ||
                             strstr(gerr, "bad revision") != NULL);
        if (!no_commits) {
            char *message = strip_copy(gerr);
            set_error(err, message);
            g_free(message);
        }
        g_free(out);
        g_free(gerr);
        return NULL;
    }
    g_free(gerr);

    GList *commits = NULL;
    char **lines = g_strsplit(out, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        if (lines[i][0] == '\0')
            continue;
        char **fields = g_strsplit(lines[i], "\x1f", 5);
        if (g_strv_length(fields) != 5) {
            g_strfreev(fields);
            continue;
        }
        NfCommit *commit = g_new0(NfCommit, 1);
        commit->hash = g_strdup(fields[0]);
        commit->short_hash = g_strdup(fields[1]);
        commit->author = g_strdup(fields[2]);
        commit->date = g_strdup(fields[3]);
        commit->subject = g_strdup(fields[4]);
        commits = g_list_append(commits, commit);
        g_strfreev(fields);
    }
    g_strfreev(lines);
    g_free(out);

    return commits;
}

char *nf_git_show_file(const char *vault, const char *commit,
                       const char *relpath, gsize *len_out, char **err) {
    if (err != NULL)
        *err = NULL;

    char *spec = g_strdup_printf("%s:%s", commit, relpath);
    char *out = NULL;
    char *gerr = NULL;
    int status = 0;

    gboolean started =
        run_git(vault, (const char *[]){"show", spec, NULL}, &out, &gerr,
                &status);
    g_free(spec);

    if (!started) {
        set_error(err, "Could not run git show.");
        g_free(out);
        g_free(gerr);
        return NULL;
    }

    if (status != 0) {
        char *message = strip_copy(gerr);
        set_error(err, message);
        g_free(message);
        g_free(out);
        g_free(gerr);
        return NULL;
    }

    g_free(gerr);
    if (len_out != NULL)
        *len_out = out != NULL ? strlen(out) : 0;
    return out != NULL ? out : g_strdup("");
}

gboolean nf_git_revert_last(const char *vault, char **err) {
    if (err != NULL)
        *err = NULL;

    char *out = NULL;
    char *gerr = NULL;
    int status = 0;

    if (!run_git(vault, (const char *[]){"rev-parse", "--verify", "HEAD", NULL},
                 &out, &gerr, &status)) {
        set_error(err, "Could not run git.");
        g_free(out);
        g_free(gerr);
        return FALSE;
    }
    g_free(out);
    out = NULL;

    if (status != 0) {
        set_error(err, "There are no commits to revert.");
        g_free(gerr);
        return FALSE;
    }
    g_free(gerr);
    gerr = NULL;

    gboolean started = run_git(
        vault,
        (const char *[]){"-c", "commit.gpgsign=false", "revert", "--no-edit",
                         "HEAD", NULL},
        &out, &gerr, &status);
    if (!started) {
        set_error(err, "Could not run git revert.");
        g_free(out);
        g_free(gerr);
        return FALSE;
    }

    if (status != 0) {
        char *message = strip_copy(gerr);
        g_free(out);
        g_free(gerr);

        // Leave the repository clean after a failed revert.
        char *abort_out = NULL;
        char *abort_err = NULL;
        int abort_status = 0;
        run_git(vault, (const char *[]){"revert", "--abort", NULL}, &abort_out,
                &abort_err, &abort_status);
        g_free(abort_out);
        g_free(abort_err);

        set_error(err, message);
        g_free(message);
        return FALSE;
    }

    g_free(out);
    g_free(gerr);
    return TRUE;
}
