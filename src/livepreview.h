// CWTNote - editable Markdown preview for the main editor.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NF_LIVE_PREVIEW_H
#define NF_LIVE_PREVIEW_H

#include <gtksourceview/gtksource.h>

typedef struct _NfLivePreview NfLivePreview;

typedef void (*NfLiveWikiFunc)(const char *target, gpointer user_data);
typedef void (*NfLiveUriFunc)(const char *uri, gpointer user_data);

// Attach live formatting to an editor view. The buffer text is never changed;
// only text tags are applied and removed.
NfLivePreview *nf_live_preview_new(GtkSourceView *view,
                                   NfLiveWikiFunc wiki_cb,
                                   NfLiveUriFunc uri_cb,
                                   gpointer user_data);

void nf_live_preview_free(NfLivePreview *preview);

// Enable or disable live formatting. Disabling removes every live tag.
void nf_live_preview_set_enabled(NfLivePreview *preview, gboolean enabled);

// Reapply formatting immediately.
void nf_live_preview_refresh(NfLivePreview *preview);

// Queue a refresh after a short delay.
void nf_live_preview_schedule(NfLivePreview *preview);

#endif
