// Noteflow - minimal quick capture window (noteflow --fast).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.h"
#include "git.h"
#include "util.h"

typedef struct {
    NfApp *app;
    GtkTextView *text_view;
} QuickUI;

typedef struct {
    GtkWindow *window;
} CloseCtx;

static void on_commit_alert(GObject *source, GAsyncResult *result,
                            gpointer user_data) {
    adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
    CloseCtx *ctx = user_data;
    gtk_window_close(ctx->window);
    g_object_unref(ctx->window);
    g_free(ctx);
}

static void show_commit_error_then_close(GtkWindow *window, const char *message) {
    AdwDialog *dialog =
        adw_alert_dialog_new("Note saved, but the commit failed", message);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", "Close");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "close");

    CloseCtx *ctx = g_new0(CloseCtx, 1);
    ctx->window = g_object_ref(window);
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window), NULL,
                            on_commit_alert, ctx);
}

static void quick_save(QuickUI *ui) {
    NfApp *app = ui->app;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(ui->text_view);
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *raw = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    char *text = nf_trim_dup(raw);
    g_free(raw);

    if (text[0] == '\0') {
        g_free(text);
        gtk_window_close(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(ui->text_view))));
        return;
    }

    char *err = NULL;
    char *relpath = nf_vault_quick_note(app->vault_handle, text, &err);
    g_free(text);
    if (relpath == NULL) {
        nf_show_error(GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(ui->text_view))),
                      "Could not save the note",
                      err != NULL ? err : "Unknown error.");
        g_free(err);
        return;
    }
    g_free(relpath);

    char *git_err = NULL;
    if (!app->git_ready && nf_git_init(app->vault, &git_err))
        app->git_ready = TRUE;

    char *message = NULL;
    if (git_err == NULL) {
        message = nf_git_commit_all(app->vault, &git_err);
        g_free(message);
    }

    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(ui->text_view));
    if (git_err != NULL) {
        show_commit_error_then_close(GTK_WINDOW(root), git_err);
        g_free(git_err);
        return;
    }

    nf_app_refresh(app);
    gtk_window_close(GTK_WINDOW(root));
}

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data) {
    (void)controller;
    (void)keycode;
    QuickUI *ui = user_data;

    if (keyval == GDK_KEY_Escape) {
        gtk_window_close(
            GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(ui->text_view))));
        return TRUE;
    }

    if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
        (state & GDK_CONTROL_MASK)) {
        quick_save(ui);
        return TRUE;
    }

    return FALSE;
}

static void on_save_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    quick_save(user_data);
}

static void on_destroy(GtkWindow *window, gpointer user_data) {
    NfApp *app = user_data;
    if (app->quick_window == window)
        app->quick_window = NULL;
}

void nf_quick_open(NfApp *app) {
    if (app->quick_window != NULL) {
        gtk_window_present(app->quick_window);
        return;
    }

    if (app->vault == NULL || app->vault_handle == NULL) {
        nf_app_start(app, TRUE);
        return;
    }

    QuickUI *ui = g_new0(QuickUI, 1);
    ui->app = app;

    AdwApplicationWindow *window =
        ADW_APPLICATION_WINDOW(adw_application_window_new(app->application));
    app->quick_window = GTK_WINDOW(window);
    gtk_window_set_title(GTK_WINDOW(window), "Quick note");
    gtk_window_set_default_size(GTK_WINDOW(window), 460, 280);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), app);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    adw_application_window_set_content(window, box);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), scroll);

    ui->text_view = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_wrap_mode(ui->text_view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_accepts_tab(ui->text_view, FALSE);
    gtk_text_view_set_left_margin(ui->text_view, 4);
    gtk_text_view_set_right_margin(ui->text_view, 4);
    gtk_text_view_set_top_margin(ui->text_view, 4);
    gtk_text_view_set_bottom_margin(ui->text_view, 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(ui->text_view));

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), ui);
    gtk_widget_add_controller(GTK_WIDGET(ui->text_view), key);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(box), row);

    GtkWidget *hint = gtk_label_new("Ctrl+Enter to save, Esc to close");
    gtk_widget_set_opacity(hint, 0.6);
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_widget_set_hexpand(hint, TRUE);
    gtk_box_append(GTK_BOX(row), hint);

    GtkWidget *save = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_widget_set_halign(save, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(row), save);
    g_signal_connect(save, "clicked", G_CALLBACK(on_save_clicked), ui);

    g_signal_connect_data(window, "destroy", G_CALLBACK(g_free), ui, NULL,
                          G_CONNECT_SWAPPED);

    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(GTK_WIDGET(ui->text_view));
}
