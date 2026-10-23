// CWTNote - application entry point.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.h"

static gboolean fast_mode = FALSE;

static void on_startup(GtkApplication *application, gpointer user_data) {
    (void)application;
    (void)user_data;
    gtk_source_init();
}

static void on_activate(GtkApplication *application, gpointer user_data) {
    (void)application;
    NfApp *app = user_data;
    nf_app_start(app, fast_mode);
    // --fast only affects the launch that requested it.
    fast_mode = FALSE;
}

static int on_command_line(GApplication *application, GApplicationCommandLine *cmd,
                           gpointer user_data) {
    (void)user_data;

    GVariantDict *options = g_application_command_line_get_options_dict(cmd);
    fast_mode = g_variant_dict_contains(options, "fast");

    g_application_activate(application);
    return 0;
}

int main(int argc, char **argv) {
    AdwApplication *application = adw_application_new(
        "org.cwtnote.CWTNote", G_APPLICATION_HANDLES_COMMAND_LINE);
    g_application_set_version(G_APPLICATION(application), NF_VERSION);

    const GOptionEntry options[] = {
        {"fast", 'f', 0, G_OPTION_ARG_NONE, NULL,
         "Open the quick note window", NULL},
        {NULL, 0, 0, 0, NULL, NULL, NULL},
    };
    g_application_add_main_option_entries(G_APPLICATION(application), options);

    NfApp *app = nf_app_new(GTK_APPLICATION(application));

    g_signal_connect(application, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(application, "activate", G_CALLBACK(on_activate), app);
    g_signal_connect(application, "command-line", G_CALLBACK(on_command_line),
                     app);

    int status = g_application_run(G_APPLICATION(application), argc, argv);
    nf_app_free(app);
    g_object_unref(application);
    return status;
}
