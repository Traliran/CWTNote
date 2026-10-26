// CWTNote - small Markdown renderer for the preview pane.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NF_MARKDOWN_H
#define NF_MARKDOWN_H

#include <gtk/gtk.h>

// Return TRUE when the link was handled.
typedef gboolean (*NfWikiLinkFunc)(const char *target, gpointer user_data);
typedef gboolean (*NfUriFunc)(const char *uri, gpointer user_data);

// Resolve an embedded file (![[target]]) to a vault-relative path, or NULL.
// source_relpath is the note that contains the embed.
typedef char *(*NfEmbedFunc)(const char *target, const char *source_relpath,
                             gpointer user_data);

// Build a widget tree for text. The caller owns the returned widget.
GtkWidget *nf_markdown_render(const char *text, const char *vault_root,
                              const char *source_relpath,
                              NfWikiLinkFunc wiki_cb, NfUriFunc uri_cb,
                              NfEmbedFunc embed_cb, gpointer user_data);

#endif
