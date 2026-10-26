// CWTNote - main application window and shared state.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NF_APP_H
#define NF_APP_H

#include <adwaita.h>
#include <gtksourceview/gtksource.h>

#include "vault.h"

#define NF_VERSION "1.1.0"

typedef struct _NfApp NfApp;

struct _NfApp {
    GtkApplication *application;
    char *vault;
    NfVault *vault_handle;
    char *setup_hint;
    gboolean fast_after_setup;
    gboolean git_ready;

    AdwApplicationWindow *window;
    AdwApplicationWindow *setup_window;
    GtkWindow *quick_window;
    GtkWindow *history_window;

    AdwOverlaySplitView *split_view;
    AdwToastOverlay *toast_overlay;
    GtkBox *tree_box;
    GtkSearchEntry *search_entry;
    GtkToggleButton *sidebar_button;
    GtkToggleButton *preview_button;
    GtkToggleButton *backlinks_button;

    GtkStack *content_stack;
    GtkStack *editor_stack;
    GtkSourceView *source_view;
    GtkSourceBuffer *source_buffer;
    GtkScrolledWindow *preview_scroll;
    GtkBox *backlinks_box;
    GtkRevealer *backlinks_revealer;

    char *active_relpath;
    gboolean dirty;
    gboolean loading;
    guint save_source_id;
    GHashTable *expanded;
    GHashTable *rows;
    GtkWidget *active_row;

    GtkPopover *completion_popover;
    GtkListBox *completion_list;
    GtkTextMark *completion_mark;
    gboolean completion_visible;
};

NfApp *nf_app_new(GtkApplication *application);
void nf_app_free(NfApp *app);

// Show the main window, the quick note window, or the first-run setup.
void nf_app_start(NfApp *app, gboolean fast_mode);

const char *nf_app_get_vault(NfApp *app);
GtkApplication *nf_app_get_application(NfApp *app);

// Small helpers shared with the quick note window.
void nf_app_toast(NfApp *app, const char *text);
void nf_show_error(GtkWidget *parent, const char *heading, const char *body);

// Open (or present) the quick note window.
void nf_quick_open(NfApp *app);

// Refresh the sidebar after files changed outside the editor.
void nf_app_refresh(NfApp *app);

#endif
