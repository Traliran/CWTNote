// CWTNote - main window, editor, sidebar, history and settings.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.h"
#include "git.h"
#include "markdown.h"
#include "util.h"

#include <string.h>

#define SAVE_DELAY_MS 700
#define HISTORY_LIMIT 80

// ---------------------------------------------------------------------------
// Small contexts
// ---------------------------------------------------------------------------

typedef struct {
    NfApp *app;
    char *dir;
} NewItemCtx;

typedef struct {
    NfApp *app;
    char *relpath;
    gboolean is_dir;
} RenameCtx;

typedef struct {
    NfApp *app;
    char *relpath;
    gboolean is_dir;
} DeleteCtx;

typedef struct {
    NfApp *app;
    GtkWindow *dialog;
    char *hash;
    char *relpath;
} RestoreCtx;

typedef struct {
    NfApp *app;
    GtkWindow *dialog;
} RevertCtx;

typedef struct {
    NfApp *app;
    char *relpath;
    gboolean is_dir;
} ItemCtx;

typedef enum {
    MENU_NEW_NOTE,
    MENU_NEW_FOLDER,
    MENU_RENAME,
    MENU_DELETE,
    MENU_HISTORY,
} MenuAction;

typedef struct {
    NfApp *app;
    GtkWidget *popover;
    char *relpath;
    gboolean is_dir;
    MenuAction action;
} MenuCtx;

typedef void (*ConfirmFunc)(gpointer data);
typedef void (*TextPromptFunc)(NfApp *app, const char *text, gpointer data);

typedef struct {
    ConfirmFunc callback;
    gpointer data;
    char *response;
} ConfirmCtx;

typedef struct {
    NfApp *app;
    TextPromptFunc callback;
    gpointer data;
    char *response;
} TextPromptCtx;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void build_main_window(NfApp *app);
static void build_setup_window(NfApp *app);
static void sidebar_refresh(NfApp *app);
static void update_preview(NfApp *app);
static void update_backlinks(NfApp *app);
static void update_active_row(NfApp *app);
static gboolean save_active(NfApp *app, gboolean commit);
static void clear_editor(NfApp *app);
static void open_note(NfApp *app, const char *relpath);
static void show_history(NfApp *app);
static void show_settings(NfApp *app);
static void show_about(NfApp *app);
static void prompt_new_note(NfApp *app, const char *dir);
static void prompt_new_folder(NfApp *app, const char *dir);
static void prompt_rename(NfApp *app, const char *relpath, gboolean is_dir);
static void confirm_delete(NfApp *app, const char *relpath, gboolean is_dir);
static void commit_changes(NfApp *app);
static void ensure_git(NfApp *app, GtkWidget *parent);
static void cancel_pending_save(NfApp *app);
static void show_error_dialog(GtkWidget *parent, const char *heading,
                              const char *body);

// ---------------------------------------------------------------------------
// Generic helpers
// ---------------------------------------------------------------------------

static void ensure_css(void) {
    static gboolean loaded = FALSE;
    if (loaded)
        return;

    GdkDisplay *display = gdk_display_get_default();
    if (display == NULL)
        return;
    loaded = TRUE;

    const char *css =
        ".nf-note-row { padding: 4px 6px; border-radius: 6px; }\n"
        ".nf-note-row.active { background-color: alpha(currentColor, 0.12); }\n"
        ".nf-note-row:focus { outline: none; }\n"
        ".nf-h1 { font-size: 26px; font-weight: 800; }\n"
        ".nf-h2 { font-size: 22px; font-weight: 700; }\n"
        ".nf-h3 { font-size: 18px; font-weight: 700; }\n"
        ".nf-h4 { font-size: 16px; font-weight: 700; }\n"
        ".nf-h5 { font-size: 14px; font-weight: 700; }\n"
        ".nf-h6 { font-size: 13px; font-weight: 700; opacity: 0.8; }\n"
        ".nf-code { font-family: monospace; background-color: alpha(currentColor, 0.08);"
        " border-radius: 6px; padding: 10px; }\n"
        ".nf-quote { opacity: 0.8; border-left: 3px solid alpha(currentColor, 0.25);"
        " padding-left: 10px; }\n";

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static char *buffer_text(NfApp *app) {
    if (app->source_buffer == NULL)
        return g_strdup("");

    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(app->source_buffer), &start, &end);
    return gtk_text_buffer_get_text(GTK_TEXT_BUFFER(app->source_buffer), &start,
                                    &end, FALSE);
}

static void cancel_pending_save(NfApp *app) {
    if (app->save_source_id != 0) {
        g_source_remove(app->save_source_id);
        app->save_source_id = 0;
    }
}

void nf_app_toast(NfApp *app, const char *text) {
    if (app == NULL || app->toast_overlay == NULL || text == NULL)
        return;
    adw_toast_overlay_add_toast(app->toast_overlay, adw_toast_new(text));
}

void nf_show_error(GtkWidget *parent, const char *heading, const char *body) {
    show_error_dialog(parent, heading, body);
}

static void on_alert_done(GObject *source, GAsyncResult *result,
                          gpointer user_data) {
    (void)user_data;
    adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
}

static void show_error_dialog(GtkWidget *parent, const char *heading,
                              const char *body) {
    if (parent == NULL) {
        g_warning("%s: %s", heading, body != NULL ? body : "");
        return;
    }

    AdwDialog *dialog = adw_alert_dialog_new(
        heading, body != NULL && body[0] != '\0' ? body : "Unknown error.");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", "Close");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "close");
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), parent, NULL,
                            on_alert_done, NULL);
}

static void on_confirm_done(GObject *source, GAsyncResult *result,
                            gpointer user_data) {
    ConfirmCtx *ctx = user_data;
    const char *response =
        adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
    if (g_strcmp0(response, ctx->response) == 0)
        ctx->callback(ctx->data);
    g_free(ctx->response);
    g_free(ctx);
}

static void confirm_action(GtkWidget *parent, const char *heading,
                           const char *body, const char *response_id,
                           const char *response_label, gboolean destructive,
                           ConfirmFunc callback, gpointer data) {
    if (parent == NULL) {
        callback(data);
        return;
    }

    AdwDialog *dialog = adw_alert_dialog_new(heading, body);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), response_id,
                                  response_label);
    adw_alert_dialog_set_response_appearance(
        ADW_ALERT_DIALOG(dialog), response_id,
        destructive ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog),
                                          response_id);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    ConfirmCtx *ctx = g_new0(ConfirmCtx, 1);
    ctx->callback = callback;
    ctx->data = data;
    ctx->response = g_strdup(response_id);
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), parent, NULL,
                            on_confirm_done, ctx);
}

static void on_text_prompt_done(GObject *source, GAsyncResult *result,
                                gpointer user_data) {
    TextPromptCtx *ctx = user_data;
    const char *response =
        adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);

    if (g_strcmp0(response, ctx->response) == 0) {
        GtkWidget *entry = g_object_get_data(source, "nf-entry");
        if (entry != NULL)
            ctx->callback(ctx->app, gtk_editable_get_text(GTK_EDITABLE(entry)),
                          ctx->data);
    }

    g_free(ctx->response);
    g_free(ctx);
}

static void text_prompt(NfApp *app, const char *heading, const char *body,
                        const char *initial, const char *response_id,
                        const char *response_label, TextPromptFunc callback,
                        gpointer data) {
    if (app->window == NULL) {
        g_free(data);
        return;
    }

    AdwDialog *dialog = adw_alert_dialog_new(heading, body);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Name");
    if (initial != NULL)
        gtk_editable_set_text(GTK_EDITABLE(entry), initial);
    gtk_widget_set_margin_top(entry, 12);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);

    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), response_id,
                                  response_label);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
                                             response_id,
                                             ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog),
                                          response_id);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    g_object_set_data(G_OBJECT(dialog), "nf-entry", entry);

    TextPromptCtx *ctx = g_new0(TextPromptCtx, 1);
    ctx->app = app;
    ctx->callback = callback;
    ctx->data = data;
    ctx->response = g_strdup(response_id);
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(app->window),
                            NULL, on_text_prompt_done, ctx);
}

static void expanded_add_with_parents(NfApp *app, const char *dir_relpath) {
    if (dir_relpath == NULL || dir_relpath[0] == '\0')
        return;

    char *copy = g_strdup(dir_relpath);
    while (copy[0] != '\0') {
        g_hash_table_add(app->expanded, g_strdup(copy));
        char *slash = strrchr(copy, '/');
        if (slash == NULL)
            break;
        *slash = '\0';
    }
    g_free(copy);
}

static gboolean path_matches_prefix(const char *path, const char *prefix) {
    if (path == NULL || prefix == NULL)
        return FALSE;
    if (strcmp(path, prefix) == 0)
        return TRUE;
    size_t len = strlen(prefix);
    return strncmp(path, prefix, len) == 0 && path[len] == '/';
}

static void expanded_remove_prefix(NfApp *app, const char *prefix) {
    GList *keys = g_hash_table_get_keys(app->expanded);
    GList *doomed = NULL;
    for (GList *l = keys; l != NULL; l = l->next) {
        if (path_matches_prefix(l->data, prefix))
            doomed = g_list_prepend(doomed, g_strdup(l->data));
    }
    for (GList *l = doomed; l != NULL; l = l->next)
        g_hash_table_remove(app->expanded, l->data);
    g_list_free_full(doomed, g_free);
    g_list_free(keys);
}

static void expanded_replace_prefix(NfApp *app, const char *old_prefix,
                                    const char *new_prefix) {
    GList *keys = g_hash_table_get_keys(app->expanded);
    GList *doomed = NULL;
    for (GList *l = keys; l != NULL; l = l->next) {
        if (path_matches_prefix(l->data, old_prefix))
            doomed = g_list_prepend(doomed, g_strdup(l->data));
    }

    size_t old_len = strlen(old_prefix);
    for (GList *l = doomed; l != NULL; l = l->next) {
        const char *old_key = l->data;
        char *new_key;
        if (strcmp(old_key, old_prefix) == 0) {
            new_key = g_strdup(new_prefix);
        } else {
            char *suffix = g_strdup(old_key + old_len);
            new_key = g_strconcat(new_prefix, suffix, NULL);
            g_free(suffix);
        }
        g_hash_table_add(app->expanded, g_strdup(new_key));
        g_hash_table_remove(app->expanded, old_key);
        g_free(new_key);
    }

    g_list_free_full(doomed, g_free);
    g_list_free(keys);
}

static char *note_target_dir(NfApp *app) {
    if (app->active_relpath == NULL)
        return g_strdup("");
    return nf_parent_relpath(app->active_relpath);
}

static void commit_changes(NfApp *app) {
    if (app->vault == NULL || app->vault_handle == NULL)
        return;

    if (!app->git_ready) {
        char *init_err = NULL;
        if (!nf_git_init(app->vault, &init_err)) {
            char *text = g_strdup_printf("Git: %s",
                                         init_err != NULL ? init_err
                                                          : "commit failed.");
            nf_app_toast(app, text);
            g_free(text);
            g_free(init_err);
            return;
        }
        app->git_ready = TRUE;
    }

    char *err = NULL;
    char *message = nf_git_commit_all(app->vault, &err);
    g_free(message);
    if (err != NULL) {
        char *text = g_strdup_printf("Git: %s", err);
        nf_app_toast(app, text);
        g_free(text);
        g_free(err);
    }
}

static void ensure_git(NfApp *app, GtkWidget *parent) {
    if (app->vault == NULL)
        return;

    char *err = NULL;
    if (!nf_git_init(app->vault, &err)) {
        app->git_ready = FALSE;
        if (parent != NULL)
            show_error_dialog(parent, "Git is unavailable",
                              err != NULL ? err : "Unknown error.");
        else
            g_warning("CWTNote: %s", err != NULL ? err : "Git is unavailable.");
        g_free(err);
        return;
    }

    app->git_ready = TRUE;
}

// ---------------------------------------------------------------------------
// Save / open
// ---------------------------------------------------------------------------

static gboolean on_save_timeout(gpointer user_data) {
    NfApp *app = user_data;
    app->save_source_id = 0;
    save_active(app, TRUE);
    return G_SOURCE_REMOVE;
}

static void schedule_save(NfApp *app) {
    cancel_pending_save(app);
    app->save_source_id = g_timeout_add(SAVE_DELAY_MS, on_save_timeout, app);
}

static gboolean save_active(NfApp *app, gboolean commit) {
    cancel_pending_save(app);

    if (app->active_relpath == NULL || !app->dirty)
        return TRUE;
    if (app->vault_handle == NULL || app->source_buffer == NULL)
        return TRUE;

    char *text = buffer_text(app);
    char *path = nf_vault_abs(app->vault_handle, app->active_relpath);
    char *err = NULL;
    gboolean ok = nf_write_file(path, text, strlen(text), &err);
    g_free(path);

    if (!ok) {
        show_error_dialog(app->window != NULL ? GTK_WIDGET(app->window) : NULL,
                          "Could not save the note", err != NULL ? err : "");
        g_free(err);
        g_free(text);
        return FALSE;
    }
    g_free(err);
    g_free(text);

    app->dirty = FALSE;
    if (commit)
        commit_changes(app);

    if (app->preview_button != NULL &&
        gtk_toggle_button_get_active(app->preview_button))
        update_preview(app);
    if (app->backlinks_revealer != NULL &&
        gtk_revealer_get_reveal_child(app->backlinks_revealer))
        update_backlinks(app);

    return TRUE;
}

static void update_window_title(NfApp *app) {
    if (app->window == NULL)
        return;
    if (app->active_relpath == NULL) {
        gtk_window_set_title(GTK_WINDOW(app->window), "CWTNote");
        return;
    }

    char *title = nf_title_from_relpath(app->active_relpath);
    char *full = g_strdup_printf("%s — CWTNote", title);
    gtk_window_set_title(GTK_WINDOW(app->window), full);
    g_free(full);
    g_free(title);
}

static void set_buffer_text(NfApp *app, const char *text) {
    if (app->source_buffer == NULL)
        return;
    app->loading = TRUE;
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(app->source_buffer), text, -1);
    app->loading = FALSE;
    app->dirty = FALSE;
    gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(app->source_buffer), FALSE);
}

static void reload_active(NfApp *app) {
    if (app->active_relpath == NULL)
        return;

    char *path = nf_vault_abs(app->vault_handle, app->active_relpath);
    char *content = nf_read_file(path, NULL);
    g_free(path);
    if (content == NULL) {
        clear_editor(app);
        return;
    }

    char *valid = g_utf8_make_valid(content, -1);
    g_free(content);
    set_buffer_text(app, valid);
    g_free(valid);
    update_window_title(app);

    if (app->preview_button != NULL &&
        gtk_toggle_button_get_active(app->preview_button))
        update_preview(app);
    if (app->backlinks_revealer != NULL &&
        gtk_revealer_get_reveal_child(app->backlinks_revealer))
        update_backlinks(app);
}

static void hide_completion(NfApp *app);

static void open_note(NfApp *app, const char *relpath) {
    if (relpath == NULL || app->vault_handle == NULL)
        return;
    if (app->active_relpath != NULL &&
        strcmp(app->active_relpath, relpath) == 0)
        return;

    if (!save_active(app, TRUE))
        return;

    char *path = nf_vault_abs(app->vault_handle, relpath);
    char *content = nf_read_file(path, NULL);
    g_free(path);
    if (content == NULL) {
        nf_app_toast(app, "Could not open the note.");
        return;
    }

    char *valid = g_utf8_make_valid(content, -1);
    g_free(content);
    set_buffer_text(app, valid);
    g_free(valid);

    g_free(app->active_relpath);
    app->active_relpath = g_strdup(relpath);

    hide_completion(app);
    update_window_title(app);
    if (app->content_stack != NULL)
        gtk_stack_set_visible_child_name(app->content_stack, "editor");
    update_active_row(app);

    if (app->preview_button != NULL &&
        gtk_toggle_button_get_active(app->preview_button))
        update_preview(app);
    if (app->backlinks_revealer != NULL &&
        gtk_revealer_get_reveal_child(app->backlinks_revealer))
        update_backlinks(app);

    if (app->source_view != NULL)
        gtk_widget_grab_focus(GTK_WIDGET(app->source_view));
}

static void clear_editor(NfApp *app) {
    cancel_pending_save(app);
    hide_completion(app);

    g_free(app->active_relpath);
    app->active_relpath = NULL;
    app->dirty = FALSE;

    if (app->source_buffer != NULL)
        set_buffer_text(app, "");

    if (app->active_row != NULL) {
        gtk_widget_remove_css_class(app->active_row, "active");
        app->active_row = NULL;
    }

    if (app->content_stack != NULL)
        gtk_stack_set_visible_child_name(app->content_stack, "empty");
    if (app->preview_scroll != NULL)
        gtk_scrolled_window_set_child(app->preview_scroll, NULL);
    if (app->backlinks_box != NULL) {
        GtkWidget *child;
        while ((child = gtk_widget_get_first_child(
                    GTK_WIDGET(app->backlinks_box))) != NULL)
            gtk_box_remove(app->backlinks_box, child);
    }

    update_window_title(app);
}

// ---------------------------------------------------------------------------
// Preview and backlinks
// ---------------------------------------------------------------------------

static gboolean on_wiki_link(const char *target, gpointer user_data) {
    NfApp *app = user_data;
    char *relpath = nf_vault_resolve_link(app->vault_handle, target);
    if (relpath == NULL) {
        char *text = g_strdup_printf("Note not found: %s", target);
        nf_app_toast(app, text);
        g_free(text);
        return TRUE;
    }
    open_note(app, relpath);
    g_free(relpath);
    return TRUE;
}

static gboolean on_external_link(const char *uri, gpointer user_data) {
    NfApp *app = user_data;
    GtkUriLauncher *launcher = gtk_uri_launcher_new(uri);
    gtk_uri_launcher_launch(launcher,
                            app->window != NULL ? GTK_WINDOW(app->window)
                                                : NULL,
                            NULL, NULL, NULL);
    g_object_unref(launcher);
    return TRUE;
}

static void update_preview(NfApp *app) {
    if (app->preview_scroll == NULL)
        return;

    char *text = buffer_text(app);
    GtkWidget *view =
        nf_markdown_render(text, on_wiki_link, on_external_link, app);
    gtk_scrolled_window_set_child(app->preview_scroll, view);
    g_free(text);
}

static void on_backlink_clicked(GtkButton *button, gpointer user_data) {
    NfApp *app = user_data;
    const char *relpath = g_object_get_data(G_OBJECT(button), "nf-relpath");
    if (relpath != NULL)
        open_note(app, relpath);
}

static void update_backlinks(NfApp *app) {
    if (app->backlinks_box == NULL)
        return;

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(
                GTK_WIDGET(app->backlinks_box))) != NULL)
        gtk_box_remove(app->backlinks_box, child);

    if (app->active_relpath == NULL || app->vault_handle == NULL)
        return;

    char *title = nf_title_from_relpath(app->active_relpath);
    GList *links =
        nf_vault_backlinks(app->vault_handle, title, app->active_relpath);
    g_free(title);

    if (links == NULL) {
        GtkWidget *label = gtk_label_new("No backlinks yet.");
        gtk_widget_add_css_class(label, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_box_append(app->backlinks_box, label);
        return;
    }

    for (GList *l = links; l != NULL; l = l->next) {
        NfLink *link = l->data;
        GtkWidget *button = gtk_button_new_with_label(link->title);
        gtk_widget_add_css_class(button, "flat");
        gtk_widget_set_halign(button, GTK_ALIGN_FILL);
        GtkWidget *label = gtk_label_new(link->title);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_button_set_child(GTK_BUTTON(button), label);
        g_object_set_data_full(G_OBJECT(button), "nf-relpath",
                               g_strdup(link->relpath), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(on_backlink_clicked),
                         app);
        gtk_box_append(app->backlinks_box, button);
    }

    nf_link_list_free(links);
}

// ---------------------------------------------------------------------------
// Wikilink completion
// ---------------------------------------------------------------------------

static void hide_completion(NfApp *app) {
    if (app->completion_popover != NULL && app->completion_visible) {
        gtk_popover_popdown(app->completion_popover);
        app->completion_visible = FALSE;
    }
}

// Remove the completion popover from the widget tree. GtkPopover owns itself
// through its parent, but containers warn (and GtkTextView even hangs) if they
// are finalized while a popover is still attached. Call this before closing or
// destroying the window.
static void destroy_completion_popover(NfApp *app) {
    if (app->completion_popover == NULL)
        return;

    if (app->completion_visible) {
        gtk_popover_popdown(app->completion_popover);
        app->completion_visible = FALSE;
    }
    if (gtk_widget_get_parent(GTK_WIDGET(app->completion_popover)) != NULL)
        gtk_widget_unparent(GTK_WIDGET(app->completion_popover));

    app->completion_popover = NULL;
    app->completion_list = NULL;
}

static void on_completion_row_activated(GtkListBox *list, GtkListBoxRow *row,
                                        gpointer user_data) {
    (void)list;
    NfApp *app = user_data;
    const char *title = g_object_get_data(G_OBJECT(row), "nf-title");
    if (title == NULL || app->source_buffer == NULL ||
        app->completion_mark == NULL)
        return;

    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(app->source_buffer);
    GtkTextIter start;
    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_mark(buffer, &start, app->completion_mark);
    gtk_text_buffer_get_iter_at_mark(
        buffer, &cursor, gtk_text_buffer_get_insert(buffer));

    gtk_text_buffer_begin_user_action(buffer);
    gtk_text_buffer_delete(buffer, &start, &cursor);
    gtk_text_buffer_insert(buffer, &start, title, -1);
    gtk_text_buffer_insert(buffer, &start, "]]", -1);
    gtk_text_buffer_place_cursor(buffer, &start);
    gtk_text_buffer_end_user_action(buffer);

    hide_completion(app);
    gtk_widget_grab_focus(GTK_WIDGET(app->source_view));
}

static void completion_update(NfApp *app) {
    if (app->completion_popover == NULL || app->source_buffer == NULL)
        return;

    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(app->source_buffer);
    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_mark(
        buffer, &cursor, gtk_text_buffer_get_insert(buffer));

    GtkTextIter limit = cursor;
    gtk_text_iter_backward_chars(&limit, 120);
    char *text = gtk_text_buffer_get_text(buffer, &limit, &cursor, TRUE);
    char *open = g_strrstr(text, "[[");
    gboolean valid = FALSE;
    char *query = NULL;

    if (open != NULL) {
        char *after = open + 2;
        if (strstr(after, "]]") == NULL && strchr(after, '\n') == NULL &&
            strchr(after, '|') == NULL) {
            valid = TRUE;
            query = g_strdup(after);
        }
    }
    g_free(text);

    if (!valid) {
        hide_completion(app);
        g_free(query);
        return;
    }

    GtkTextIter start = cursor;
    gtk_text_iter_backward_chars(&start, (gint)g_utf8_strlen(query, -1));
    if (app->completion_mark == NULL) {
        app->completion_mark =
            gtk_text_buffer_create_mark(buffer, "nf-completion", &start, FALSE);
    } else {
        gtk_text_buffer_move_mark(buffer, app->completion_mark, &start);
    }

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(app->completion_list)))
           != NULL)
        gtk_list_box_remove(app->completion_list, child);

    char *query_folded = g_utf8_casefold(query, -1);
    GList *notes = nf_vault_all_notes(app->vault_handle);
    guint shown = 0;

    for (GList *l = notes; l != NULL && shown < 12; l = l->next) {
        NfEntry *entry = l->data;
        char *title = nf_title_from_relpath(entry->relpath);
        char *title_folded = g_utf8_casefold(title, -1);
        gboolean match = query_folded[0] == '\0' ||
                         strstr(title_folded, query_folded) != NULL;
        if (match) {
            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *label = gtk_label_new(title);
            gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
            gtk_widget_set_margin_start(label, 8);
            gtk_widget_set_margin_end(label, 8);
            gtk_widget_set_margin_top(label, 6);
            gtk_widget_set_margin_bottom(label, 6);
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
            g_object_set_data_full(G_OBJECT(row), "nf-title", g_strdup(title),
                                   g_free);
            gtk_list_box_append(app->completion_list, row);
            shown++;
        }
        g_free(title_folded);
        g_free(title);
    }
    nf_entry_list_free(notes);
    g_free(query_folded);
    g_free(query);

    if (shown == 0) {
        hide_completion(app);
        return;
    }

    GdkRectangle rect;
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(app->source_view), &cursor,
                                    &rect);
    int x = 0;
    int y = 0;
    gtk_text_view_buffer_to_window_coords(
        GTK_TEXT_VIEW(app->source_view), GTK_TEXT_WINDOW_WIDGET, rect.x,
        rect.y + rect.height, &x, &y);

    // The popover is parented to the scrolled window, not to the text view:
    // GtkTextView only accepts its own internal children and loops forever
    // while disposing of a foreign child. Translate the cursor position into
    // the popover parent's coordinate system.
    GtkWidget *anchor =
        gtk_widget_get_parent(GTK_WIDGET(app->completion_popover));
    if (anchor != NULL) {
        graphene_point_t from = {(float)x, (float)y};
        graphene_point_t to;
        if (gtk_widget_compute_point(GTK_WIDGET(app->source_view), anchor,
                                     &from, &to)) {
            GdkRectangle point = {(int)to.x, (int)to.y, 1, 1};
            gtk_popover_set_pointing_to(app->completion_popover, &point);
        }
    }

    if (!app->completion_visible) {
        gtk_popover_popup(app->completion_popover);
        app->completion_visible = TRUE;
    }
}

static void on_buffer_changed(GtkTextBuffer *buffer, gpointer user_data) {
    (void)buffer;
    NfApp *app = user_data;
    if (app->loading)
        return;

    app->dirty = TRUE;
    schedule_save(app);
    completion_update(app);
}

static gboolean on_source_key_pressed(GtkEventControllerKey *controller,
                                      guint keyval, guint keycode,
                                      GdkModifierType state,
                                      gpointer user_data) {
    (void)controller;
    (void)keycode;
    NfApp *app = user_data;

    if (keyval == GDK_KEY_Escape && app->completion_visible) {
        hide_completion(app);
        return TRUE;
    }

    if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
        (state & GDK_CONTROL_MASK)) {
        save_active(app, TRUE);
        return TRUE;
    }

    return FALSE;
}

// ---------------------------------------------------------------------------
// Sidebar
// ---------------------------------------------------------------------------

static void on_note_clicked(GtkButton *button, gpointer user_data) {
    NfApp *app = user_data;
    const char *relpath = g_object_get_data(G_OBJECT(button), "nf-relpath");
    if (relpath != NULL)
        open_note(app, relpath);
}

static void on_folder_toggled(GObject *object, GParamSpec *pspec,
                              gpointer user_data) {
    (void)pspec;
    NfApp *app = user_data;
    const char *relpath = g_object_get_data(object, "nf-relpath");
    if (relpath == NULL)
        return;

    if (gtk_expander_get_expanded(GTK_EXPANDER(object)))
        g_hash_table_add(app->expanded, g_strdup(relpath));
    else
        g_hash_table_remove(app->expanded, relpath);
}

static void item_ctx_free(gpointer data) {
    ItemCtx *ctx = data;
    g_free(ctx->relpath);
    g_free(ctx);
}

static void menu_ctx_free(gpointer data) {
    MenuCtx *ctx = data;
    g_free(ctx->relpath);
    g_free(ctx);
}

static void on_popover_closed(GtkWidget *popover, gpointer user_data) {
    (void)user_data;
    gtk_widget_unparent(popover);
}

static void on_menu_action(GtkButton *button, gpointer user_data) {
    (void)user_data;
    MenuCtx *ctx = g_object_get_data(G_OBJECT(button), "nf-menu-ctx");
    if (ctx == NULL)
        return;

    if (ctx->popover != NULL)
        gtk_popover_popdown(GTK_POPOVER(ctx->popover));

    switch (ctx->action) {
    case MENU_NEW_NOTE: {
        char *dir = ctx->is_dir ? g_strdup(ctx->relpath)
                                : nf_parent_relpath(ctx->relpath);
        prompt_new_note(ctx->app, dir);
        g_free(dir);
        break;
    }
    case MENU_NEW_FOLDER: {
        char *dir = ctx->is_dir ? g_strdup(ctx->relpath)
                                : nf_parent_relpath(ctx->relpath);
        prompt_new_folder(ctx->app, dir);
        g_free(dir);
        break;
    }
    case MENU_RENAME:
        prompt_rename(ctx->app, ctx->relpath, ctx->is_dir);
        break;
    case MENU_DELETE:
        confirm_delete(ctx->app, ctx->relpath, ctx->is_dir);
        break;
    case MENU_HISTORY:
        open_note(ctx->app, ctx->relpath);
        show_history(ctx->app);
        break;
    }
}

static GtkWidget *menu_button_new(const char *label, MenuCtx *base,
                                  MenuAction action, GtkWidget *popover) {
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_set_halign(button, GTK_ALIGN_FILL);

    MenuCtx *ctx = g_new0(MenuCtx, 1);
    ctx->app = base->app;
    ctx->popover = popover;
    ctx->relpath = g_strdup(base->relpath);
    ctx->is_dir = base->is_dir;
    ctx->action = action;
    g_object_set_data_full(G_OBJECT(button), "nf-menu-ctx", ctx, menu_ctx_free);

    g_signal_connect(button, "clicked", G_CALLBACK(on_menu_action), NULL);
    return button;
}

static void show_item_menu(NfApp *app, GtkWidget *anchor, const char *relpath,
                           gboolean is_dir, double x, double y) {
    GtkWidget *popover = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    MenuCtx base = {.app = app, .relpath = (char *)relpath, .is_dir = is_dir};

    if (is_dir) {
        gtk_box_append(GTK_BOX(box),
                       menu_button_new("New Note", &base, MENU_NEW_NOTE,
                                       popover));
        gtk_box_append(GTK_BOX(box),
                       menu_button_new("New Folder", &base, MENU_NEW_FOLDER,
                                       popover));
    } else {
        gtk_box_append(GTK_BOX(box),
                       menu_button_new("History", &base, MENU_HISTORY,
                                       popover));
    }
    gtk_box_append(GTK_BOX(box),
                   menu_button_new("Rename", &base, MENU_RENAME, popover));
    gtk_box_append(GTK_BOX(box),
                   menu_button_new("Delete", &base, MENU_DELETE, popover));

    gtk_popover_set_child(GTK_POPOVER(popover), box);
    gtk_widget_set_parent(popover, anchor);
    GdkRectangle rect = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    g_signal_connect(popover, "closed", G_CALLBACK(on_popover_closed), NULL);
    gtk_popover_popup(GTK_POPOVER(popover));
}

static void on_item_context_pressed(GtkGestureClick *gesture, int n_press,
                                    double x, double y, gpointer user_data) {
    (void)n_press;
    (void)user_data;
    ItemCtx *ctx = g_object_get_data(G_OBJECT(gesture), "nf-item-ctx");
    if (ctx == NULL)
        return;
    GtkWidget *anchor =
        gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    show_item_menu(ctx->app, anchor, ctx->relpath, ctx->is_dir, x, y);
}

static void attach_context_menu(GtkWidget *widget, NfApp *app,
                                const char *relpath, gboolean is_dir) {
    ItemCtx *ctx = g_new0(ItemCtx, 1);
    ctx->app = app;
    ctx->relpath = g_strdup(relpath);
    ctx->is_dir = is_dir;

    GtkGesture *gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),
                                  GDK_BUTTON_SECONDARY);
    g_object_set_data_full(G_OBJECT(gesture), "nf-item-ctx", ctx,
                           item_ctx_free);
    g_signal_connect(gesture, "pressed", G_CALLBACK(on_item_context_pressed),
                     NULL);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(gesture));
}

static GtkWidget *make_note_row(NfApp *app, const NfEntry *entry, int depth) {
    char *title = nf_title_from_relpath(entry->relpath);

    GtkWidget *button = gtk_button_new();
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_add_css_class(button, "nf-note-row");
    gtk_widget_set_halign(button, GTK_ALIGN_FILL);
    gtk_widget_set_margin_start(button, 4 + depth * 14);
    gtk_widget_set_margin_end(button, 4);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon = gtk_image_new_from_icon_name("text-x-generic-symbolic");
    gtk_widget_set_opacity(icon, 0.7);
    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(row), icon);
    gtk_box_append(GTK_BOX(row), label);
    gtk_button_set_child(GTK_BUTTON(button), row);

    g_object_set_data_full(G_OBJECT(button), "nf-relpath",
                           g_strdup(entry->relpath), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(on_note_clicked), app);
    attach_context_menu(button, app, entry->relpath, FALSE);
    g_hash_table_insert(app->rows, g_strdup(entry->relpath), button);

    g_free(title);
    return button;
}

static void sidebar_add_dir(NfApp *app, GtkWidget *container,
                            const char *relpath, int depth) {
    GList *entries = nf_vault_list_dir(app->vault_handle, relpath);

    for (GList *l = entries; l != NULL; l = l->next) {
        NfEntry *entry = l->data;
        if (entry->is_dir) {
            GtkWidget *expander = gtk_expander_new(NULL);
            gtk_widget_set_margin_start(expander, 4 + depth * 14);
            gtk_widget_set_margin_end(expander, 4);

            GtkWidget *label_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget *icon = gtk_image_new_from_icon_name("folder-symbolic");
            gtk_widget_set_opacity(icon, 0.75);
            GtkWidget *label = gtk_label_new(entry->name);
            gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
            gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            gtk_widget_set_hexpand(label, TRUE);
            gtk_box_append(GTK_BOX(label_box), icon);
            gtk_box_append(GTK_BOX(label_box), label);
            gtk_expander_set_label_widget(GTK_EXPANDER(expander), label_box);

            g_object_set_data_full(G_OBJECT(expander), "nf-relpath",
                                   g_strdup(entry->relpath), g_free);
            gtk_expander_set_expanded(
                GTK_EXPANDER(expander),
                g_hash_table_contains(app->expanded, entry->relpath));
            g_signal_connect(expander, "notify::expanded",
                             G_CALLBACK(on_folder_toggled), app);
            attach_context_menu(label_box, app, entry->relpath, TRUE);

            GtkWidget *child_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
            gtk_expander_set_child(GTK_EXPANDER(expander), child_box);
            sidebar_add_dir(app, child_box, entry->relpath, depth + 1);
            gtk_box_append(GTK_BOX(container), expander);
        } else {
            gtk_box_append(GTK_BOX(container),
                           make_note_row(app, entry, depth));
        }
    }

    nf_entry_list_free(entries);
}

static void sidebar_refresh(NfApp *app) {
    if (app->tree_box == NULL)
        return;

    app->active_row = NULL;
    g_hash_table_remove_all(app->rows);

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(app->tree_box)))
           != NULL)
        gtk_box_remove(app->tree_box, child);

    const char *query =
        app->search_entry != NULL
            ? gtk_editable_get_text(GTK_EDITABLE(app->search_entry))
            : "";

    if (query != NULL && query[0] != '\0') {
        char *folded = g_utf8_casefold(query, -1);
        GList *notes = nf_vault_all_notes(app->vault_handle);
        for (GList *l = notes; l != NULL; l = l->next) {
            NfEntry *entry = l->data;
            char *title = nf_title_from_relpath(entry->relpath);
            char *title_folded = g_utf8_casefold(title, -1);
            char *path_folded = g_utf8_casefold(entry->relpath, -1);
            if (strstr(title_folded, folded) != NULL ||
                strstr(path_folded, folded) != NULL)
                gtk_box_append(GTK_BOX(app->tree_box),
                               make_note_row(app, entry, 0));
            g_free(path_folded);
            g_free(title_folded);
            g_free(title);
        }
        nf_entry_list_free(notes);
        g_free(folded);

        if (gtk_widget_get_first_child(GTK_WIDGET(app->tree_box)) == NULL) {
            GtkWidget *label = gtk_label_new("No notes match the search.");
            gtk_widget_add_css_class(label, "dim-label");
            gtk_widget_set_margin_top(label, 12);
            gtk_widget_set_margin_start(label, 8);
            gtk_box_append(GTK_BOX(app->tree_box), label);
        }
    } else {
        sidebar_add_dir(app, GTK_WIDGET(app->tree_box), "", 0);
        if (gtk_widget_get_first_child(GTK_WIDGET(app->tree_box)) == NULL) {
            GtkWidget *label = gtk_label_new("No notes yet.");
            gtk_widget_add_css_class(label, "dim-label");
            gtk_widget_set_margin_top(label, 12);
            gtk_widget_set_margin_start(label, 8);
            gtk_box_append(GTK_BOX(app->tree_box), label);
        }
    }

    update_active_row(app);
}

static void update_active_row(NfApp *app) {
    if (app->active_row != NULL) {
        gtk_widget_remove_css_class(app->active_row, "active");
        app->active_row = NULL;
    }
    if (app->active_relpath == NULL)
        return;

    app->active_row = g_hash_table_lookup(app->rows, app->active_relpath);
    if (app->active_row != NULL)
        gtk_widget_add_css_class(app->active_row, "active");
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
    (void)entry;
    sidebar_refresh(user_data);
}

static void reset_search_and_refresh(NfApp *app) {
    if (app->search_entry != NULL) {
        const char *text = gtk_editable_get_text(GTK_EDITABLE(app->search_entry));
        if (text != NULL && text[0] != '\0')
            gtk_editable_set_text(GTK_EDITABLE(app->search_entry), "");
    }
    sidebar_refresh(app);
}

// ---------------------------------------------------------------------------
// Item prompts and deletion
// ---------------------------------------------------------------------------

static void on_new_note_text(NfApp *app, const char *text, gpointer data) {
    NewItemCtx *ctx = data;
    char *err = NULL;
    char *relpath =
        nf_vault_create_note(app->vault_handle, ctx->dir, text, &err);

    if (relpath == NULL) {
        show_error_dialog(GTK_WIDGET(app->window),
                          "Could not create the note",
                          err != NULL ? err : "Unknown error.");
    } else {
        expanded_add_with_parents(app, ctx->dir);
        reset_search_and_refresh(app);
        open_note(app, relpath);
        g_free(relpath);
    }

    g_free(err);
    g_free(ctx->dir);
    g_free(ctx);
}

static void on_new_folder_text(NfApp *app, const char *text, gpointer data) {
    NewItemCtx *ctx = data;
    char *err = NULL;
    char *relpath =
        nf_vault_create_folder(app->vault_handle, ctx->dir, text, &err);

    if (relpath == NULL) {
        show_error_dialog(GTK_WIDGET(app->window),
                          "Could not create the folder",
                          err != NULL ? err : "Unknown error.");
    } else {
        expanded_add_with_parents(app, ctx->dir);
        g_hash_table_add(app->expanded, g_strdup(relpath));
        reset_search_and_refresh(app);
        g_free(relpath);
    }

    g_free(err);
    g_free(ctx->dir);
    g_free(ctx);
}

static void prompt_new_note(NfApp *app, const char *dir) {
    NewItemCtx *ctx = g_new0(NewItemCtx, 1);
    ctx->app = app;
    ctx->dir = g_strdup(dir);
    text_prompt(app, "New Note", "Enter a name for the new note.", NULL,
                "create", "Create", on_new_note_text, ctx);
}

static void prompt_new_folder(NfApp *app, const char *dir) {
    NewItemCtx *ctx = g_new0(NewItemCtx, 1);
    ctx->app = app;
    ctx->dir = g_strdup(dir);
    text_prompt(app, "New Folder", "Enter a name for the new folder.", NULL,
                "create", "Create", on_new_folder_text, ctx);
}

static void on_rename_text(NfApp *app, const char *text, gpointer data) {
    RenameCtx *ctx = data;
    char *err = NULL;
    char *new_relpath =
        nf_vault_rename(app->vault_handle, ctx->relpath, text, &err);

    if (new_relpath == NULL) {
        show_error_dialog(GTK_WIDGET(app->window), "Could not rename",
                          err != NULL ? err : "Unknown error.");
    } else {
        if (app->active_relpath != NULL) {
            if (strcmp(app->active_relpath, ctx->relpath) == 0) {
                g_free(app->active_relpath);
                app->active_relpath = g_strdup(new_relpath);
            } else if (ctx->is_dir &&
                       path_matches_prefix(app->active_relpath, ctx->relpath)) {
                size_t old_len = strlen(ctx->relpath);
                char *suffix = g_strdup(app->active_relpath + old_len);
                char *updated = g_strconcat(new_relpath, suffix, NULL);
                g_free(suffix);
                g_free(app->active_relpath);
                app->active_relpath = updated;
            }
        }
        if (ctx->is_dir)
            expanded_replace_prefix(app, ctx->relpath, new_relpath);

        sidebar_refresh(app);
        update_window_title(app);
        commit_changes(app);
        if (app->backlinks_revealer != NULL &&
            gtk_revealer_get_reveal_child(app->backlinks_revealer))
            update_backlinks(app);
        g_free(new_relpath);
    }

    g_free(err);
    g_free(ctx->relpath);
    g_free(ctx);
}

static void prompt_rename(NfApp *app, const char *relpath, gboolean is_dir) {
    RenameCtx *ctx = g_new0(RenameCtx, 1);
    ctx->app = app;
    ctx->relpath = g_strdup(relpath);
    ctx->is_dir = is_dir;

    char *initial =
        is_dir ? g_path_get_basename(relpath) : nf_title_from_relpath(relpath);
    text_prompt(app, is_dir ? "Rename Folder" : "Rename Note",
                "Enter a new name.", initial, "rename", "Rename",
                on_rename_text, ctx);
    g_free(initial);
}

static void on_delete_confirmed(gpointer data) {
    DeleteCtx *ctx = data;
    NfApp *app = ctx->app;
    char *err = NULL;

    if (!nf_vault_delete(app->vault_handle, ctx->relpath, &err)) {
        show_error_dialog(GTK_WIDGET(app->window), "Could not delete",
                          err != NULL ? err : "Unknown error.");
    } else {
        cancel_pending_save(app);
        if (app->active_relpath != NULL &&
            path_matches_prefix(app->active_relpath, ctx->relpath)) {
            clear_editor(app);
        }
        expanded_remove_prefix(app, ctx->relpath);
        sidebar_refresh(app);
        commit_changes(app);
        nf_app_toast(app, "Moved to trash.");
    }

    g_free(err);
    g_free(ctx->relpath);
    g_free(ctx);
}

static void confirm_delete(NfApp *app, const char *relpath, gboolean is_dir) {
    DeleteCtx *ctx = g_new0(DeleteCtx, 1);
    ctx->app = app;
    ctx->relpath = g_strdup(relpath);
    ctx->is_dir = is_dir;

    char *title = is_dir ? g_path_get_basename(relpath)
                         : nf_title_from_relpath(relpath);
    char *body = g_strdup_printf(
        "“%s” will be moved to the trash. This cannot be undone from CWTNote.",
        title);
    confirm_action(GTK_WIDGET(app->window), is_dir ? "Delete Folder?" : "Delete Note?",
                   body, "delete", "Delete", TRUE, on_delete_confirmed, ctx);
    g_free(body);
    g_free(title);
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

static void restore_ctx_free(gpointer data) {
    RestoreCtx *ctx = data;
    g_free(ctx->hash);
    g_free(ctx->relpath);
    g_free(ctx);
}

static void history_destroyed(GtkWindow *window, gpointer user_data) {
    NfApp *app = user_data;
    if (app->history_window == window)
        app->history_window = NULL;
}

static void do_restore(gpointer data) {
    RestoreCtx *ctx = data;
    NfApp *app = ctx->app;

    cancel_pending_save(app);
    app->dirty = FALSE;

    char *err = NULL;
    gsize len = 0;
    char *content =
        nf_git_show_file(app->vault, ctx->hash, ctx->relpath, &len, &err);
    if (content == NULL) {
        show_error_dialog(GTK_WIDGET(app->window), "Could not restore version",
                          err != NULL ? err : "Unknown error.");
        g_free(err);
        g_free(ctx->hash);
        g_free(ctx->relpath);
        g_free(ctx);
        return;
    }

    char *path = nf_vault_abs(app->vault_handle, ctx->relpath);
    if (!nf_write_file(path, content, len, &err)) {
        show_error_dialog(GTK_WIDGET(app->window), "Could not restore version",
                          err != NULL ? err : "Unknown error.");
        g_free(err);
        g_free(path);
        g_free(content);
        g_free(ctx->hash);
        g_free(ctx->relpath);
        g_free(ctx);
        return;
    }
    g_free(path);
    g_free(content);

    reload_active(app);
    commit_changes(app);
    nf_app_toast(app, "Version restored.");
    if (app->history_window != NULL)
        gtk_window_destroy(app->history_window);

    g_free(ctx->hash);
    g_free(ctx->relpath);
    g_free(ctx);
}

static void on_restore_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    RestoreCtx *source = g_object_get_data(G_OBJECT(button), "nf-restore");
    if (source == NULL)
        return;

    RestoreCtx *copy = g_new0(RestoreCtx, 1);
    copy->app = source->app;
    copy->dialog = source->dialog;
    copy->hash = g_strdup(source->hash);
    copy->relpath = g_strdup(source->relpath);
    confirm_action(
        GTK_WIDGET(source->dialog), "Restore this version?",
        "The current note will be replaced and a new commit will be created.",
        "restore", "Restore", FALSE, do_restore, copy);
}

static void do_revert(gpointer data) {
    RevertCtx *ctx = data;
    NfApp *app = ctx->app;

    cancel_pending_save(app);
    app->dirty = FALSE;

    char *err = NULL;
    if (!nf_git_revert_last(app->vault, &err)) {
        show_error_dialog(GTK_WIDGET(app->window),
                          "Could not revert the last commit",
                          err != NULL ? err : "Unknown error.");
        g_free(err);
    } else {
        reload_active(app);
        sidebar_refresh(app);
        nf_app_toast(app, "Last commit reverted.");
        if (app->history_window != NULL)
            gtk_window_destroy(app->history_window);
    }

    g_free(ctx);
}

static void on_revert_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    RevertCtx *source = user_data;
    RevertCtx *copy = g_new0(RevertCtx, 1);
    copy->app = source->app;
    copy->dialog = source->dialog;
    confirm_action(
        GTK_WIDGET(source->dialog), "Revert the last commit?",
        "This creates a new commit that undoes the most recent change. "
        "Unsaved edits in the open note will be lost.",
        "revert", "Revert", TRUE, do_revert, copy);
}

static void show_history(NfApp *app) {
    if (app->active_relpath == NULL) {
        nf_app_toast(app, "Open a note first.");
        return;
    }
    if (app->vault == NULL) {
        nf_app_toast(app, "Git history is unavailable.");
        return;
    }

    if (app->history_window != NULL) {
        gtk_window_destroy(app->history_window);
        app->history_window = NULL;
    }

    char *err = NULL;
    GList *commits =
        nf_git_log(app->vault, app->active_relpath, HISTORY_LIMIT, &err);
    if (err != NULL) {
        show_error_dialog(GTK_WIDGET(app->window), "Could not read history",
                          err);
        g_free(err);
        return;
    }

    GtkWidget *window = adw_window_new();
    app->history_window = GTK_WINDOW(window);
    gtk_window_set_title(GTK_WINDOW(window), "History");
    gtk_window_set_default_size(GTK_WINDOW(window), 560, 540);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(window),
                                 GTK_WINDOW(app->window));
    g_signal_connect(window, "destroy", G_CALLBACK(history_destroyed), app);

    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

    GtkWidget *close_button =
        gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_tooltip_text(close_button, "Close");
    g_signal_connect_swapped(close_button, "clicked",
                             G_CALLBACK(gtk_window_destroy), window);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), close_button);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_set_margin_start(list, 12);
    gtk_widget_set_margin_end(list, 12);
    gtk_widget_set_margin_top(list, 12);
    gtk_widget_set_margin_bottom(list, 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_box_append(GTK_BOX(content), scroll);

    if (commits == NULL) {
        GtkWidget *label = gtk_label_new("No history for this note yet.");
        gtk_widget_add_css_class(label, "dim-label");
        gtk_widget_set_margin_top(label, 24);
        gtk_box_append(GTK_BOX(content), label);
    } else {
        for (GList *l = commits; l != NULL; l = l->next) {
            NfCommit *commit = l->data;
            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_widget_set_margin_start(box, 10);
            gtk_widget_set_margin_end(box, 10);
            gtk_widget_set_margin_top(box, 8);
            gtk_widget_set_margin_bottom(box, 8);

            GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_set_hexpand(info, TRUE);

            GtkWidget *subject = gtk_label_new(commit->subject);
            gtk_label_set_xalign(GTK_LABEL(subject), 0.0f);
            gtk_label_set_ellipsize(GTK_LABEL(subject), PANGO_ELLIPSIZE_END);
            gtk_widget_add_css_class(subject, "heading");

            char *meta =
                g_strdup_printf("%s • %s • %s", commit->date,
                                commit->short_hash, commit->author);
            GtkWidget *meta_label = gtk_label_new(meta);
            gtk_widget_add_css_class(meta_label, "dim-label");
            gtk_label_set_xalign(GTK_LABEL(meta_label), 0.0f);
            gtk_label_set_ellipsize(GTK_LABEL(meta_label),
                                    PANGO_ELLIPSIZE_END);
            g_free(meta);

            gtk_box_append(GTK_BOX(info), subject);
            gtk_box_append(GTK_BOX(info), meta_label);

            GtkWidget *restore = gtk_button_new_with_label("Restore");
            gtk_widget_set_valign(restore, GTK_ALIGN_CENTER);

            RestoreCtx *ctx = g_new0(RestoreCtx, 1);
            ctx->app = app;
            ctx->dialog = GTK_WINDOW(window);
            ctx->hash = g_strdup(commit->hash);
            ctx->relpath = g_strdup(app->active_relpath);
            g_object_set_data_full(G_OBJECT(restore), "nf-restore", ctx,
                                   restore_ctx_free);
            g_signal_connect(restore, "clicked", G_CALLBACK(on_restore_clicked),
                             NULL);

            gtk_box_append(GTK_BOX(box), info);
            gtk_box_append(GTK_BOX(box), restore);
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
            gtk_list_box_append(GTK_LIST_BOX(list), row);
        }
    }
    nf_commit_list_free(commits);

    RevertCtx *revert = g_new0(RevertCtx, 1);
    revert->app = app;
    revert->dialog = GTK_WINDOW(window);
    g_object_set_data_full(G_OBJECT(window), "nf-revert", revert, g_free);

    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(bottom, 12);
    gtk_widget_set_margin_end(bottom, 12);
    gtk_widget_set_margin_top(bottom, 8);
    gtk_widget_set_margin_bottom(bottom, 8);
    GtkWidget *revert_button = gtk_button_new_with_label("Revert Last Commit");
    gtk_widget_add_css_class(revert_button, "destructive-action");
    gtk_widget_set_halign(revert_button, GTK_ALIGN_END);
    gtk_widget_set_hexpand(revert_button, TRUE);
    g_signal_connect(revert_button, "clicked", G_CALLBACK(on_revert_clicked),
                     revert);
    gtk_box_append(GTK_BOX(bottom), revert_button);
    gtk_box_append(GTK_BOX(content), bottom);

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), content);
    adw_window_set_content(ADW_WINDOW(window), toolbar);
    gtk_window_present(GTK_WINDOW(window));
}

// ---------------------------------------------------------------------------
// Settings and about
// ---------------------------------------------------------------------------

static void nf_app_change_vault(NfApp *app, const char *path) {
    char *expanded = nf_expand_path(path);
    if (expanded == NULL) {
        nf_app_toast(app, "Invalid vault path.");
        return;
    }

    char *err = NULL;
    if (!nf_mkdir(expanded, &err)) {
        show_error_dialog(GTK_WIDGET(app->window), "Could not use this folder",
                          err != NULL ? err : "Unknown error.");
        g_free(err);
        g_free(expanded);
        return;
    }

    if (!save_active(app, TRUE)) {
        g_free(expanded);
        return;
    }

    NfVault *new_vault = nf_vault_open(expanded);
    if (new_vault == NULL) {
        nf_app_toast(app, "Could not open the vault.");
        g_free(expanded);
        return;
    }

    clear_editor(app);
    nf_vault_free(app->vault_handle);
    g_free(app->vault);
    app->vault = expanded;
    app->vault_handle = new_vault;
    g_hash_table_remove_all(app->expanded);

    if (!nf_config_set_vault(app->vault))
        nf_app_toast(app, "Could not save the configuration.");

    ensure_git(app, GTK_WIDGET(app->window));
    sidebar_refresh(app);
    update_window_title(app);
    nf_app_toast(app, "Vault changed.");
}

static void on_settings_folder_chosen(GObject *source, GAsyncResult *result,
                                      gpointer user_data) {
    NfApp *app = user_data;
    GError *err = NULL;
    GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source),
                                                       result, &err);
    if (file != NULL) {
        char *path = g_file_get_path(file);
        if (path != NULL) {
            nf_app_change_vault(app, path);
            g_free(path);
        }
        g_object_unref(file);
    } else if (err != NULL &&
               !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        show_error_dialog(GTK_WIDGET(app->window), "Could not choose a folder",
                          err->message);
    }
    g_clear_error(&err);
}

static void on_settings_browse(GtkButton *button, gpointer user_data) {
    (void)button;
    NfApp *app = user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose Vault Folder");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(app->window), NULL,
                                  on_settings_folder_chosen, app);
    g_object_unref(dialog);
}

static void show_settings(NfApp *app) {
    AdwDialog *dialog = adw_preferences_dialog_new();
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *group =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Storage");

    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Vault folder");
    adw_action_row_set_subtitle(row, app->vault != NULL ? app->vault : "");
    GtkWidget *browse = gtk_button_new_with_label("Choose…");
    gtk_widget_set_valign(browse, GTK_ALIGN_CENTER);
    g_signal_connect(browse, "clicked", G_CALLBACK(on_settings_browse), app);
    adw_action_row_add_suffix(row, browse);
    adw_preferences_group_add(group, GTK_WIDGET(row));

    adw_preferences_page_add(page, group);
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), page);
    adw_dialog_present(dialog, GTK_WIDGET(app->window));
}

static void show_about(NfApp *app) {
    AdwDialog *dialog = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(dialog), "CWTNote");
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(dialog),
                                          "org.cwtnote.CWTNote");
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(dialog), NF_VERSION);
    adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(dialog), "Traliran");
    adw_about_dialog_set_comments(
        ADW_ABOUT_DIALOG(dialog),
        "Local-first Markdown notes with automatic Git history.");
    adw_about_dialog_set_website(ADW_ABOUT_DIALOG(dialog),
                                 "https://github.com/Traliran/CWTNote");
    adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(dialog),
                                      GTK_LICENSE_GPL_3_0);
    const char *developers[] = {"Traliran", NULL};
    adw_about_dialog_set_developers(ADW_ABOUT_DIALOG(dialog), developers);
    adw_dialog_present(dialog, GTK_WIDGET(app->window));
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

static void action_new_note(GSimpleAction *action, GVariant *parameter,
                            gpointer user_data) {
    (void)action;
    (void)parameter;
    NfApp *app = user_data;
    char *dir = note_target_dir(app);
    prompt_new_note(app, dir);
    g_free(dir);
}

static void action_new_folder(GSimpleAction *action, GVariant *parameter,
                              gpointer user_data) {
    (void)action;
    (void)parameter;
    NfApp *app = user_data;
    char *dir = note_target_dir(app);
    prompt_new_folder(app, dir);
    g_free(dir);
}

static void action_save(GSimpleAction *action, GVariant *parameter,
                        gpointer user_data) {
    (void)action;
    (void)parameter;
    save_active(user_data, TRUE);
}

static void action_history(GSimpleAction *action, GVariant *parameter,
                           gpointer user_data) {
    (void)action;
    (void)parameter;
    show_history(user_data);
}

static void action_revert_last(GSimpleAction *action, GVariant *parameter,
                               gpointer user_data) {
    (void)action;
    (void)parameter;
    NfApp *app = user_data;
    RevertCtx *ctx = g_new0(RevertCtx, 1);
    ctx->app = app;
    ctx->dialog = app->window != NULL ? GTK_WINDOW(app->window) : NULL;
    confirm_action(
        GTK_WIDGET(app->window), "Revert the last commit?",
        "This creates a new commit that undoes the most recent change. "
        "Unsaved edits in the open note will be lost.",
        "revert", "Revert", TRUE, do_revert, ctx);
}

static void action_rename(GSimpleAction *action, GVariant *parameter,
                          gpointer user_data) {
    (void)action;
    (void)parameter;
    NfApp *app = user_data;
    if (app->active_relpath == NULL) {
        nf_app_toast(app, "Open a note first.");
        return;
    }
    prompt_rename(app, app->active_relpath, FALSE);
}

static void action_delete(GSimpleAction *action, GVariant *parameter,
                          gpointer user_data) {
    (void)action;
    (void)parameter;
    NfApp *app = user_data;
    if (app->active_relpath == NULL) {
        nf_app_toast(app, "Open a note first.");
        return;
    }
    confirm_delete(app, app->active_relpath, FALSE);
}

static void action_settings(GSimpleAction *action, GVariant *parameter,
                            gpointer user_data) {
    (void)action;
    (void)parameter;
    show_settings(user_data);
}

static void action_about(GSimpleAction *action, GVariant *parameter,
                         gpointer user_data) {
    (void)action;
    (void)parameter;
    show_about(user_data);
}

static void action_quit(GSimpleAction *action, GVariant *parameter,
                        gpointer user_data) {
    (void)action;
    (void)parameter;
    NfApp *app = user_data;
    if (!save_active(app, TRUE))
        return;
    destroy_completion_popover(app);
    g_application_quit(G_APPLICATION(app->application));
}

static void action_toggle_preview(GSimpleAction *action, GVariant *parameter,
                                  gpointer user_data) {
    (void)action;
    (void)parameter;
    NfApp *app = user_data;
    if (app->preview_button != NULL)
        gtk_toggle_button_set_active(
            app->preview_button,
            !gtk_toggle_button_get_active(app->preview_button));
}

static void action_toggle_backlinks(GSimpleAction *action, GVariant *parameter,
                                    gpointer user_data) {
    (void)action;
    (void)parameter;
    NfApp *app = user_data;
    if (app->backlinks_button != NULL)
        gtk_toggle_button_set_active(
            app->backlinks_button,
            !gtk_toggle_button_get_active(app->backlinks_button));
}

static void action_focus_search(GSimpleAction *action, GVariant *parameter,
                                gpointer user_data) {
    (void)action;
    (void)parameter;
    NfApp *app = user_data;
    if (app->split_view != NULL)
        adw_overlay_split_view_set_show_sidebar(app->split_view, TRUE);
    if (app->sidebar_button != NULL)
        gtk_toggle_button_set_active(app->sidebar_button, TRUE);
    if (app->search_entry != NULL) {
        gtk_widget_grab_focus(GTK_WIDGET(app->search_entry));
        gtk_editable_select_region(GTK_EDITABLE(app->search_entry), 0, -1);
    }
}

static void action_toggle_sidebar(GSimpleAction *action, GVariant *parameter,
                                  gpointer user_data) {
    (void)action;
    (void)parameter;
    NfApp *app = user_data;
    if (app->split_view == NULL)
        return;
    gboolean show =
        adw_overlay_split_view_get_show_sidebar(app->split_view);
    adw_overlay_split_view_set_show_sidebar(app->split_view, !show);
    if (app->sidebar_button != NULL)
        gtk_toggle_button_set_active(app->sidebar_button, !show);
}

static void register_actions(NfApp *app) {
    const GActionEntry entries[] = {
        {.name = "new-note", .activate = action_new_note},
        {.name = "new-folder", .activate = action_new_folder},
        {.name = "save", .activate = action_save},
        {.name = "history", .activate = action_history},
        {.name = "revert-last", .activate = action_revert_last},
        {.name = "rename", .activate = action_rename},
        {.name = "delete", .activate = action_delete},
        {.name = "settings", .activate = action_settings},
        {.name = "about", .activate = action_about},
        {.name = "quit", .activate = action_quit},
        {.name = "toggle-preview", .activate = action_toggle_preview},
        {.name = "toggle-backlinks", .activate = action_toggle_backlinks},
        {.name = "focus-search", .activate = action_focus_search},
        {.name = "toggle-sidebar", .activate = action_toggle_sidebar},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app->application), entries,
                                    G_N_ELEMENTS(entries), app);
}

static void add_accel(GtkApplication *application, const char *action,
                      const char *accel) {
    const char *accels[] = {accel, NULL};
    gtk_application_set_accels_for_action(application, action, accels);
}

static void setup_accels(NfApp *app) {
    add_accel(app->application, "app.new-note", "<Control>n");
    add_accel(app->application, "app.new-folder", "<Control><Shift>n");
    add_accel(app->application, "app.save", "<Control>s");
    add_accel(app->application, "app.history", "<Control>h");
    add_accel(app->application, "app.focus-search", "<Control>f");
    add_accel(app->application, "app.quit", "<Control>q");
}

// ---------------------------------------------------------------------------
// Main window
// ---------------------------------------------------------------------------

static gboolean on_close_request(GtkWindow *window, gpointer user_data) {
    (void)window;
    NfApp *app = user_data;
    if (!save_active(app, TRUE))
        return TRUE;

    destroy_completion_popover(app);
    return FALSE;
}

static void on_main_destroy(GtkWindow *window, gpointer user_data) {
    NfApp *app = user_data;
    if (GTK_WINDOW(app->window) != window)
        return;

    cancel_pending_save(app);

    // The completion popover is detached in close-request/quit before the
    // widget tree is disposed. This signal runs during disposal, so only the
    // raw pointers are cleared below.
    app->window = NULL;
    app->split_view = NULL;
    app->toast_overlay = NULL;
    app->tree_box = NULL;
    app->search_entry = NULL;
    app->sidebar_button = NULL;
    app->preview_button = NULL;
    app->backlinks_button = NULL;
    app->content_stack = NULL;
    app->editor_stack = NULL;
    app->source_view = NULL;
    app->source_buffer = NULL;
    app->preview_scroll = NULL;
    app->backlinks_box = NULL;
    app->backlinks_revealer = NULL;
    app->active_row = NULL;
    app->completion_popover = NULL;
    app->completion_list = NULL;
    app->completion_mark = NULL;
    app->completion_visible = FALSE;
    g_clear_pointer(&app->active_relpath, g_free);
    app->dirty = FALSE;
}

static void on_preview_toggled(GtkToggleButton *button, gpointer user_data) {
    NfApp *app = user_data;
    if (gtk_toggle_button_get_active(button)) {
        update_preview(app);
        gtk_stack_set_visible_child_name(app->editor_stack, "preview");
    } else {
        gtk_stack_set_visible_child_name(app->editor_stack, "source");
        if (app->source_view != NULL)
            gtk_widget_grab_focus(GTK_WIDGET(app->source_view));
    }
}

static void on_backlinks_toggled(GtkToggleButton *button, gpointer user_data) {
    NfApp *app = user_data;
    gboolean active = gtk_toggle_button_get_active(button);
    if (app->backlinks_revealer != NULL)
        gtk_revealer_set_reveal_child(app->backlinks_revealer, active);
    if (active)
        update_backlinks(app);
}

static void on_sidebar_toggled(GtkToggleButton *button, gpointer user_data) {
    NfApp *app = user_data;
    if (app->split_view != NULL)
        adw_overlay_split_view_set_show_sidebar(
            app->split_view, gtk_toggle_button_get_active(button));
}

static void on_dark_changed(GObject *object, GParamSpec *pspec,
                            gpointer user_data) {
    (void)object;
    (void)pspec;
    NfApp *app = user_data;
    if (app->source_buffer == NULL)
        return;

    AdwStyleManager *manager = adw_style_manager_get_default();
    const char *scheme_id =
        adw_style_manager_get_dark(manager) ? "Adwaita-dark" : "Adwaita";
    GtkSourceStyleSchemeManager *schemes =
        gtk_source_style_scheme_manager_get_default();
    GtkSourceStyleScheme *scheme =
        gtk_source_style_scheme_manager_get_scheme(schemes, scheme_id);
    if (scheme != NULL)
        gtk_source_buffer_set_style_scheme(app->source_buffer, scheme);
}

static GtkWidget *build_editor_stack(NfApp *app) {
    GtkWidget *stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_widget_set_hexpand(stack, TRUE);
    app->editor_stack = GTK_STACK(stack);

    GtkSourceBuffer *buffer = gtk_source_buffer_new(NULL);
    app->source_buffer = buffer;
    GtkSourceLanguageManager *languages =
        gtk_source_language_manager_get_default();
    GtkSourceLanguage *markdown =
        gtk_source_language_manager_get_language(languages, "markdown");
    if (markdown != NULL)
        gtk_source_buffer_set_language(buffer, markdown);
    gtk_source_buffer_set_highlight_syntax(buffer, TRUE);

    GtkSourceView *view = GTK_SOURCE_VIEW(gtk_source_view_new_with_buffer(buffer));
    app->source_view = view;
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 18);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 18);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 18);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 18);
    gtk_source_view_set_auto_indent(view, TRUE);
    gtk_source_view_set_indent_width(view, 4);
    gtk_source_view_set_tab_width(view, 4);
    gtk_source_view_set_insert_spaces_instead_of_tabs(view, TRUE);
    gtk_source_view_set_highlight_current_line(view, TRUE);

    AdwStyleManager *style = adw_style_manager_get_default();
    g_signal_connect(style, "notify::dark", G_CALLBACK(on_dark_changed), app);
    on_dark_changed(NULL, NULL, app);

    g_signal_connect(buffer, "changed", G_CALLBACK(on_buffer_changed), app);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_source_key_pressed), app);
    gtk_widget_add_controller(GTK_WIDGET(view), key);

    GtkWidget *source_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(source_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(source_scroll, TRUE);
    gtk_widget_set_hexpand(source_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(source_scroll),
                                  GTK_WIDGET(view));
    gtk_stack_add_named(GTK_STACK(stack), source_scroll, "source");

    app->preview_scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_policy(app->preview_scroll, GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(GTK_WIDGET(app->preview_scroll), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(app->preview_scroll), TRUE);
    gtk_stack_add_named(GTK_STACK(stack),
                        GTK_WIDGET(app->preview_scroll), "preview");

    gtk_stack_set_visible_child_name(GTK_STACK(stack), "source");

    app->completion_popover = GTK_POPOVER(gtk_popover_new());
    gtk_popover_set_has_arrow(app->completion_popover, FALSE);
    GtkWidget *completion_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(completion_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(completion_scroll, 300, 220);
    app->completion_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(app->completion_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(completion_scroll),
                                  GTK_WIDGET(app->completion_list));
    gtk_popover_set_child(app->completion_popover, completion_scroll);
    // Parent the popover to the scrolled window. GtkTextView rejects foreign
    // children and hangs in gtk_text_view_dispose() if a popover is attached
    // to it directly.
    gtk_widget_set_parent(GTK_WIDGET(app->completion_popover), source_scroll);
    g_signal_connect(app->completion_list, "row-activated",
                     G_CALLBACK(on_completion_row_activated), app);

    return stack;
}

static void on_empty_new_note(GtkButton *button, gpointer user_data) {
    (void)button;
    NfApp *app = user_data;
    g_action_group_activate_action(G_ACTION_GROUP(app->application),
                                   "new-note", NULL);
}

static GtkWidget *build_empty_page(NfApp *app) {
    GtkWidget *page = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(page),
                                  "document-edit-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(page), "No note selected");
    adw_status_page_set_description(
        ADW_STATUS_PAGE(page),
        "Select a note from the sidebar, or create a new one.");
    GtkWidget *button = gtk_button_new_with_label("New Note");
    gtk_widget_add_css_class(button, "suggested-action");
    g_signal_connect(button, "clicked", G_CALLBACK(on_empty_new_note), app);
    adw_status_page_set_child(ADW_STATUS_PAGE(page), button);
    return page;
}

// ---------------------------------------------------------------------------
// Main window construction
// ---------------------------------------------------------------------------

static void on_new_note_button(GtkButton *button, gpointer user_data) {
    (void)button;
    NfApp *app = user_data;
    g_action_group_activate_action(G_ACTION_GROUP(app->application),
                                   "new-note", NULL);
}

static GtkWidget *build_sidebar(NfApp *app) {
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(sidebar, 6);
    gtk_widget_set_margin_end(sidebar, 6);
    gtk_widget_set_margin_top(sidebar, 6);
    gtk_widget_set_margin_bottom(sidebar, 6);

    app->search_entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(app->search_entry, "Search notes");
    gtk_widget_set_hexpand(GTK_WIDGET(app->search_entry), TRUE);
    g_signal_connect(app->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), app);
    gtk_box_append(GTK_BOX(sidebar), GTK_WIDGET(app->search_entry));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);

    app->tree_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 1));
    gtk_widget_set_margin_start(GTK_WIDGET(app->tree_box), 6);
    gtk_widget_set_margin_end(GTK_WIDGET(app->tree_box), 6);
    gtk_widget_set_margin_top(GTK_WIDGET(app->tree_box), 2);
    gtk_widget_set_margin_bottom(GTK_WIDGET(app->tree_box), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(app->tree_box));
    gtk_box_append(GTK_BOX(sidebar), scroll);

    return sidebar;
}

static GtkToggleButton *icon_toggle_button(const char *icon_name) {
    GtkToggleButton *button = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
    gtk_button_set_child(GTK_BUTTON(button),
                         gtk_image_new_from_icon_name(icon_name));
    return button;
}

static GtkWidget *build_header(NfApp *app) {
    AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());

    app->sidebar_button = icon_toggle_button("sidebar-show-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(app->sidebar_button),
                                "Toggle sidebar");
    gtk_toggle_button_set_active(app->sidebar_button, TRUE);
    g_signal_connect(app->sidebar_button, "toggled",
                     G_CALLBACK(on_sidebar_toggled), app);
    adw_header_bar_pack_start(header, GTK_WIDGET(app->sidebar_button));

    GtkWidget *new_button =
        gtk_button_new_from_icon_name("document-new-symbolic");
    gtk_widget_set_tooltip_text(new_button, "New note (Ctrl+N)");
    g_signal_connect(new_button, "clicked", G_CALLBACK(on_new_note_button),
                     app);
    adw_header_bar_pack_start(header, new_button);

    app->preview_button = icon_toggle_button("view-reveal-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(app->preview_button),
                                "Toggle preview");
    g_signal_connect(app->preview_button, "toggled",
                     G_CALLBACK(on_preview_toggled), app);
    adw_header_bar_pack_end(header, GTK_WIDGET(app->preview_button));

    app->backlinks_button = icon_toggle_button("insert-link-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(app->backlinks_button),
                                "Toggle backlinks");
    g_signal_connect(app->backlinks_button, "toggled",
                     G_CALLBACK(on_backlinks_toggled), app);
    adw_header_bar_pack_end(header, GTK_WIDGET(app->backlinks_button));

    GtkWidget *menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button),
                                  "open-menu-symbolic");
    gtk_widget_set_tooltip_text(menu_button, "Menu");

    GMenu *menu = g_menu_new();
    g_menu_append(menu, "New Note", "app.new-note");
    g_menu_append(menu, "New Folder", "app.new-folder");
    g_menu_append(menu, "Save", "app.save");

    GMenu *notes = g_menu_new();
    g_menu_append(notes, "Rename Note", "app.rename");
    g_menu_append(notes, "Delete Note", "app.delete");
    g_menu_append(notes, "History", "app.history");
    g_menu_append(notes, "Revert Last Commit", "app.revert-last");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(notes));
    g_object_unref(notes);

    GMenu *application = g_menu_new();
    g_menu_append(application, "Settings", "app.settings");
    g_menu_append(application, "About CWTNote", "app.about");
    g_menu_append(application, "Quit", "app.quit");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(application));
    g_object_unref(application);

    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button),
                                   G_MENU_MODEL(menu));
    g_object_unref(menu);
    adw_header_bar_pack_end(header, menu_button);

    return GTK_WIDGET(header);
}

static GtkWidget *build_backlinks_panel(NfApp *app) {
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(panel, 250, -1);

    GtkWidget *title = gtk_label_new("Backlinks");
    gtk_widget_add_css_class(title, "heading");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_widget_set_margin_start(title, 12);
    gtk_widget_set_margin_end(title, 12);
    gtk_widget_set_margin_top(title, 12);
    gtk_box_append(GTK_BOX(panel), title);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    app->backlinks_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 2));
    gtk_widget_set_margin_start(GTK_WIDGET(app->backlinks_box), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(app->backlinks_box), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(app->backlinks_box), 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(app->backlinks_box));
    gtk_box_append(GTK_BOX(panel), scroll);

    GtkWidget *revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 150);
    gtk_revealer_set_child(GTK_REVEALER(revealer), panel);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);
    app->backlinks_revealer = GTK_REVEALER(revealer);
    return revealer;
}

static void build_main_window(NfApp *app) {
    if (app->window != NULL) {
        gtk_window_present(GTK_WINDOW(app->window));
        return;
    }

    ensure_css();

    AdwApplicationWindow *window = ADW_APPLICATION_WINDOW(
        adw_application_window_new(app->application));
    app->window = window;
    gtk_window_set_title(GTK_WINDOW(window), "CWTNote");
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 720);
    g_signal_connect(window, "close-request", G_CALLBACK(on_close_request),
                     app);
    g_signal_connect(window, "destroy", G_CALLBACK(on_main_destroy), app);

    app->toast_overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *header = build_header(app);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toast_overlay_set_child(app->toast_overlay, toolbar);
    adw_application_window_set_content(window, GTK_WIDGET(app->toast_overlay));

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(content, TRUE);
    gtk_widget_set_vexpand(content, TRUE);

    app->content_stack = GTK_STACK(gtk_stack_new());
    gtk_widget_set_hexpand(GTK_WIDGET(app->content_stack), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(app->content_stack), TRUE);
    gtk_stack_add_named(app->content_stack, build_empty_page(app), "empty");
    gtk_stack_add_named(app->content_stack, build_editor_stack(app), "editor");
    gtk_stack_set_visible_child_name(app->content_stack, "empty");
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(app->content_stack));

    GtkWidget *backlinks = build_backlinks_panel(app);
    gtk_box_append(GTK_BOX(content), backlinks);

    AdwOverlaySplitView *split =
        ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    app->split_view = split;
    adw_overlay_split_view_set_sidebar(split, build_sidebar(app));
    adw_overlay_split_view_set_content(split, content);
    adw_overlay_split_view_set_min_sidebar_width(split, 220);
    adw_overlay_split_view_set_max_sidebar_width(split, 340);
    adw_overlay_split_view_set_show_sidebar(split, TRUE);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(split));

    gtk_window_present(GTK_WINDOW(window));
    sidebar_refresh(app);
    update_window_title(app);

    if (!app->git_ready)
        nf_app_toast(app, "Git is unavailable; history is disabled.");
}

// ---------------------------------------------------------------------------
// First-run setup
// ---------------------------------------------------------------------------

static gboolean app_use_vault(NfApp *app, const char *path, char **err);

static void on_setup_destroy(GtkWindow *window, gpointer user_data) {
    NfApp *app = user_data;
    if (GTK_WINDOW(app->setup_window) == window)
        app->setup_window = NULL;
}

static void on_setup_folder_chosen(GObject *source, GAsyncResult *result,
                                   gpointer user_data) {
    NfApp *app = user_data;
    GError *err = NULL;
    GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source),
                                                       result, &err);

    if (file != NULL) {
        // The setup window may have been closed while the picker was open.
        if (app->setup_window != NULL) {
            GtkEntry *entry = g_object_get_data(G_OBJECT(app->setup_window),
                                                "nf-vault-entry");
            char *path = g_file_get_path(file);
            if (entry != NULL && path != NULL)
                gtk_editable_set_text(GTK_EDITABLE(entry), path);
            g_free(path);
        }
        g_object_unref(file);
    }
    g_clear_error(&err);
}

static void on_setup_browse(GtkButton *button, gpointer user_data) {
    (void)button;
    NfApp *app = user_data;
    if (app->setup_window == NULL)
        return;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose Vault Folder");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(app->setup_window), NULL,
                                  on_setup_folder_chosen, app);
    g_object_unref(dialog);
}

static void setup_continue(NfApp *app) {
    if (app->setup_window == NULL)
        return;

    GtkEntry *entry = g_object_get_data(G_OBJECT(app->setup_window),
                                        "nf-vault-entry");
    if (entry == NULL)
        return;

    char *path = nf_expand_path(gtk_editable_get_text(GTK_EDITABLE(entry)));
    if (path == NULL || path[0] == '\0') {
        show_error_dialog(GTK_WIDGET(app->setup_window), "Invalid folder",
                          "Please choose a folder for your vault.");
        g_free(path);
        return;
    }

    char *err = NULL;
    if (!app_use_vault(app, path, &err)) {
        show_error_dialog(GTK_WIDGET(app->setup_window),
                          "Could not use this folder",
                          err != NULL ? err : "Unknown error.");
        g_free(err);
        g_free(path);
        return;
    }
    g_free(path);

    if (!nf_config_set_vault(app->vault)) {
        show_error_dialog(GTK_WIDGET(app->setup_window),
                          "Could not save the configuration",
                          "The vault is usable for this session only.");
        return;
    }

    ensure_git(app, NULL);

    AdwApplicationWindow *setup = app->setup_window;
    app->setup_window = NULL;
    gtk_window_destroy(GTK_WINDOW(setup));

    if (app->fast_after_setup)
        nf_quick_open(app);
    else
        build_main_window(app);
    app->fast_after_setup = FALSE;
}

static void on_setup_continue(GtkButton *button, gpointer user_data) {
    (void)button;
    setup_continue(user_data);
}

static void on_setup_entry_activate(GtkEntry *entry, gpointer user_data) {
    (void)entry;
    setup_continue(user_data);
}

static void build_setup_window(NfApp *app) {
    if (app->setup_window != NULL) {
        gtk_window_present(GTK_WINDOW(app->setup_window));
        return;
    }

    ensure_css();

    AdwApplicationWindow *window = ADW_APPLICATION_WINDOW(
        adw_application_window_new(app->application));
    app->setup_window = window;
    gtk_window_set_title(GTK_WINDOW(window), "Welcome to CWTNote");
    gtk_window_set_default_size(GTK_WINDOW(window), 640, 520);
    g_signal_connect(window, "destroy", G_CALLBACK(on_setup_destroy), app);

    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

    GtkWidget *page = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(page),
                                  "document-edit-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(page), "Welcome to CWTNote");
    adw_status_page_set_description(
        ADW_STATUS_PAGE(page),
        "Your notes stay as plain Markdown files. Choose a folder for your "
        "vault; Git history is created automatically.");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(box, 460, -1);

    GtkWidget *label = gtk_label_new("Vault folder");
    gtk_widget_add_css_class(label, "heading");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_append(GTK_BOX(box), label);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "~/CWTNote");
    gtk_widget_set_hexpand(entry, TRUE);
    g_object_set_data(G_OBJECT(window), "nf-vault-entry", entry);
    gtk_box_append(GTK_BOX(row), entry);

    GtkWidget *browse = gtk_button_new_with_label("Browse…");
    g_signal_connect(browse, "clicked", G_CALLBACK(on_setup_browse), app);
    gtk_box_append(GTK_BOX(row), browse);
    gtk_box_append(GTK_BOX(box), row);

    char *initial = NULL;
    if (app->setup_hint != NULL)
        initial = g_strdup(app->setup_hint);
    else
        initial = g_build_filename(g_get_home_dir(), "CWTNote", NULL);
    gtk_editable_set_text(GTK_EDITABLE(entry), initial);
    g_free(initial);

    GtkWidget *continue_button = gtk_button_new_with_label("Continue");
    gtk_widget_add_css_class(continue_button, "suggested-action");
    gtk_widget_set_halign(continue_button, GTK_ALIGN_END);
    g_signal_connect(continue_button, "clicked", G_CALLBACK(on_setup_continue),
                     app);
    gtk_box_append(GTK_BOX(box), continue_button);
    g_signal_connect(entry, "activate", G_CALLBACK(on_setup_entry_activate),
                     app);

    adw_status_page_set_child(ADW_STATUS_PAGE(page), box);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_application_window_set_content(window, toolbar);

    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(entry);
}

// ---------------------------------------------------------------------------
// Vault switching and application lifecycle
// ---------------------------------------------------------------------------

static gboolean app_use_vault(NfApp *app, const char *path, char **err) {
    char *expanded = nf_expand_path(path);
    if (expanded == NULL) {
        if (err != NULL)
            *err = g_strdup("Invalid vault path.");
        return FALSE;
    }

    if (!nf_mkdir(expanded, err)) {
        g_free(expanded);
        return FALSE;
    }

    NfVault *vault = nf_vault_open(expanded);
    if (vault == NULL) {
        if (err != NULL)
            *err = g_strdup("Could not open the vault folder.");
        g_free(expanded);
        return FALSE;
    }

    nf_vault_free(app->vault_handle);
    g_free(app->vault);
    app->vault = expanded;
    app->vault_handle = vault;
    g_hash_table_remove_all(app->expanded);
    return TRUE;
}

void nf_app_start(NfApp *app, gboolean fast_mode) {
    ensure_css();

    if (fast_mode) {
        if (app->quick_window != NULL) {
            gtk_window_present(app->quick_window);
            return;
        }
    } else {
        if (app->window != NULL) {
            gtk_window_present(GTK_WINDOW(app->window));
            return;
        }
    }

    if (app->setup_window != NULL) {
        app->fast_after_setup = fast_mode;
        gtk_window_present(GTK_WINDOW(app->setup_window));
        return;
    }

    if (app->vault_handle == NULL) {
        char *stored = nf_config_get_vault();
        if (stored != NULL) {
            char *expanded = nf_expand_path(stored);
            g_free(stored);

            if (expanded != NULL && g_file_test(expanded, G_FILE_TEST_IS_DIR)) {
                char *err = NULL;
                if (!app_use_vault(app, expanded, &err)) {
                    g_free(app->setup_hint);
                    app->setup_hint = g_strdup(expanded);
                    g_free(err);
                }
                g_free(expanded);
            } else {
                g_free(app->setup_hint);
                app->setup_hint = expanded;
            }
        }
    }

    if (app->vault_handle != NULL) {
        if (!app->git_ready)
            ensure_git(app, NULL);
        if (fast_mode)
            nf_quick_open(app);
        else
            build_main_window(app);
        return;
    }

    app->fast_after_setup = fast_mode;
    build_setup_window(app);
}

void nf_app_refresh(NfApp *app) {
    if (app != NULL && app->window != NULL)
        sidebar_refresh(app);
}

NfApp *nf_app_new(GtkApplication *application) {
    NfApp *app = g_new0(NfApp, 1);
    app->application = application;
    app->expanded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    app->rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    register_actions(app);
    setup_accels(app);
    return app;
}

void nf_app_free(NfApp *app) {
    if (app == NULL)
        return;

    cancel_pending_save(app);
    g_free(app->vault);
    nf_vault_free(app->vault_handle);
    g_free(app->setup_hint);
    g_free(app->active_relpath);
    g_hash_table_destroy(app->expanded);
    g_hash_table_destroy(app->rows);
    g_free(app);
}

const char *nf_app_get_vault(NfApp *app) { return app->vault; }

GtkApplication *nf_app_get_application(NfApp *app) {
    return app->application;
}
