// CWTNote - editable Markdown preview for the main editor.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "livepreview.h"

#include <string.h>

#define REFRESH_DELAY_MS 60
#define MAX_INLINE_DEPTH 8

struct _NfLivePreview {
    GtkSourceView *view;
    GtkTextBuffer *buffer;
    gboolean enabled;

    NfLiveWikiFunc wiki_cb;
    NfLiveUriFunc uri_cb;
    gpointer user_data;

    GtkTextTag *hidden;
    GtkTextTag *bold;
    GtkTextTag *italic;
    GtkTextTag *strike;
    GtkTextTag *code;
    GtkTextTag *quote;
    GtkTextTag *list;
    GtkTextTag *dim;
    GtkTextTag *heading[6];

    GPtrArray *link_tags;
    guint update_id;
    gulong mark_set_id;
    gulong cursor_id;
    gulong scheme_id;

    GtkEventController *click;
    gulong click_id;
};

static void inline_scan(NfLivePreview *preview, const char *line, gsize len,
                        gsize base, gsize from, gsize to, guint depth);

static gsize line_chars(const char *line, gsize bytes) {
    return (gsize)g_utf8_strlen(line, (gssize)bytes);
}

static void apply_tag_range(NfLivePreview *preview, GtkTextTag *tag,
                            gsize start, gsize end) {
    if (preview->buffer == NULL || tag == NULL || end <= start)
        return;
    GtkTextIter from;
    GtkTextIter to;
    gtk_text_buffer_get_iter_at_offset(preview->buffer, &from, (int)start);
    gtk_text_buffer_get_iter_at_offset(preview->buffer, &to, (int)end);
    gtk_text_buffer_apply_tag(preview->buffer, tag, &from, &to);
}

static gsize find_token(const char *line, gsize from, gsize to,
                        const char *token) {
    gsize token_len = strlen(token);
    if (to < from || to - from < token_len)
        return G_MAXSIZE;
    for (gsize i = from; i + token_len <= to; i++) {
        if (memcmp(line + i, token, token_len) == 0)
            return i;
    }
    return G_MAXSIZE;
}

static GtkTextTag *make_link_tag(NfLivePreview *preview, const char *target,
                                 gboolean wiki, gboolean styled) {
    GtkTextTag *tag;
    if (styled) {
        tag = gtk_text_buffer_create_tag(GTK_TEXT_BUFFER(preview->buffer), NULL,
                                         "underline", PANGO_UNDERLINE_SINGLE,
                                         "foreground", "#3584e4", NULL);
    } else {
        tag = gtk_text_buffer_create_tag(GTK_TEXT_BUFFER(preview->buffer), NULL,
                                         NULL);
        g_object_set_data_full(G_OBJECT(tag), "nf-live-link-target",
                               g_strdup(target), g_free);
        g_object_set_data(G_OBJECT(tag), "nf-live-link-wiki",
                          GINT_TO_POINTER(wiki ? 1 : 0));
    }
    g_ptr_array_add(preview->link_tags, tag);
    return tag;
}

static void format_link(NfLivePreview *preview, const char *line, gsize base,
                        gsize syntax_start, gsize syntax_end,
                        gsize display_start, gsize display_end,
                        const char *target, gboolean wiki) {
    GtkTextTag *hit = make_link_tag(preview, target, wiki, FALSE);
    GtkTextTag *styled = make_link_tag(preview, target, wiki, TRUE);

    apply_tag_range(preview, hit, base + line_chars(line, syntax_start),
                    base + line_chars(line, syntax_end));
    if (display_end > display_start) {
        apply_tag_range(preview, styled, base + line_chars(line, display_start),
                        base + line_chars(line, display_end));
    }
}

static void inline_scan(NfLivePreview *preview, const char *line, gsize len,
                        gsize base, gsize from, gsize to, guint depth) {
    if (depth > MAX_INLINE_DEPTH)
        return;

    gsize i = from;
    while (i < to) {
        if (line[i] == '\\') {
            i++;
            if (i < to)
                i += (gsize)(g_utf8_next_char(line + i) - (line + i));
            continue;
        }

        // Inline code: markers are hidden and the content is monospaced.
        if (line[i] == '`') {
            gsize close = find_token(line, i + 1, to, "`");
            if (close != G_MAXSIZE && close > i + 1) {
                apply_tag_range(preview, preview->hidden, base + line_chars(line, i),
                                base + line_chars(line, i + 1));
                apply_tag_range(preview, preview->hidden,
                                base + line_chars(line, close),
                                base + line_chars(line, close + 1));
                apply_tag_range(preview, preview->code,
                                base + line_chars(line, i + 1),
                                base + line_chars(line, close));
                i = close + 1;
                continue;
            }
        }

        // Wikilink: [[target]] or [[target|display text]].
        if (line[i] == '[' && i + 1 < to && line[i + 1] == '[' &&
            !(i > 0 && line[i - 1] == '!')) {
            gsize close = find_token(line, i + 2, to, "]]");
            if (close != G_MAXSIZE) {
                gsize inner_start = i + 2;
                gsize inner_end = close;
                const char *bar = memchr(line + inner_start, '|',
                                         inner_end - inner_start);
                gsize bar_pos = bar != NULL ? (gsize)(bar - line) : G_MAXSIZE;
                gsize display_start =
                    bar_pos != G_MAXSIZE ? bar_pos + 1 : inner_start;
                char *target = g_strndup(
                    line + inner_start,
                    (bar_pos != G_MAXSIZE ? bar_pos : inner_end) - inner_start);
                g_strstrip(target);

                if (target[0] != '\0') {
                    apply_tag_range(preview, preview->hidden,
                                    base + line_chars(line, i),
                                    base + line_chars(line, i + 2));
                    apply_tag_range(preview, preview->hidden,
                                    base + line_chars(line, close),
                                    base + line_chars(line, close + 2));
                    if (bar_pos != G_MAXSIZE) {
                        apply_tag_range(preview, preview->hidden,
                                        base + line_chars(line, inner_start),
                                        base + line_chars(line, bar_pos + 1));
                    }
                    format_link(preview, line, base, i, close + 2,
                                display_start, inner_end, target, TRUE);
                    g_free(target);
                    i = close + 2;
                    continue;
                }
                g_free(target);
            }
        }

        // Markdown link: [label](url).
        if (line[i] == '[' && !(i > 0 && line[i - 1] == '!')) {
            const char *close_bracket = memchr(line + i + 1, ']', to - i - 1);
            if (close_bracket != NULL) {
                gsize label_end = (gsize)(close_bracket - line);
                gsize url_start = label_end + 2;
                if (label_end + 1 < to && line[label_end + 1] == '(') {
                    const char *close_paren =
                        memchr(line + url_start, ')', to - url_start);
                    if (close_paren != NULL && label_end > i + 1) {
                        gsize paren_end = (gsize)(close_paren - line);
                        char *target =
                            g_strndup(line + url_start, paren_end - url_start);
                        apply_tag_range(preview, preview->hidden,
                                        base + line_chars(line, i),
                                        base + line_chars(line, i + 1));
                        apply_tag_range(preview, preview->hidden,
                                        base + line_chars(line, label_end),
                                        base + line_chars(line, paren_end + 1));
                        format_link(preview, line, base, i, paren_end + 1, i + 1,
                                    label_end, target, FALSE);
                        g_free(target);
                        i = paren_end + 1;
                        continue;
                    }
                }
            }
        }

        // Bold.
        if ((i + 1 < to) &&
            ((line[i] == '*' && line[i + 1] == '*') ||
             (line[i] == '_' && line[i + 1] == '_'))) {
            const char *marker = line[i] == '*' ? "**" : "__";
            gsize close = find_token(line, i + 2, to, marker);
            if (close != G_MAXSIZE && close > i + 2) {
                apply_tag_range(preview, preview->hidden, base + line_chars(line, i),
                                base + line_chars(line, i + 2));
                apply_tag_range(preview, preview->hidden,
                                base + line_chars(line, close),
                                base + line_chars(line, close + 2));
                apply_tag_range(preview, preview->bold,
                                base + line_chars(line, i + 2),
                                base + line_chars(line, close));
                inline_scan(preview, line, len, base, i + 2, close, depth + 1);
                i = close + 2;
                continue;
            }
        }

        // Strikethrough.
        if (i + 1 < to && line[i] == '~' && line[i + 1] == '~') {
            gsize close = find_token(line, i + 2, to, "~~");
            if (close != G_MAXSIZE && close > i + 2) {
                apply_tag_range(preview, preview->hidden, base + line_chars(line, i),
                                base + line_chars(line, i + 2));
                apply_tag_range(preview, preview->hidden,
                                base + line_chars(line, close),
                                base + line_chars(line, close + 2));
                apply_tag_range(preview, preview->strike,
                                base + line_chars(line, i + 2),
                                base + line_chars(line, close));
                i = close + 2;
                continue;
            }
        }

        // Italic.
        if (line[i] == '*' || line[i] == '_') {
            char marker = line[i];
            gsize close = find_token(line, i + 1, to,
                                     marker == '*' ? "*" : "_");
            if (close != G_MAXSIZE && close > i + 1) {
                apply_tag_range(preview, preview->hidden, base + line_chars(line, i),
                                base + line_chars(line, i + 1));
                apply_tag_range(preview, preview->hidden,
                                base + line_chars(line, close),
                                base + line_chars(line, close + 1));
                apply_tag_range(preview, preview->italic,
                                base + line_chars(line, i + 1),
                                base + line_chars(line, close));
                inline_scan(preview, line, len, base, i + 1, close, depth + 1);
                i = close + 1;
                continue;
            }
        }

        i += (gsize)(g_utf8_next_char(line + i) - (line + i));
    }
}

// A horizontal rule is three or more identical markers separated by spaces.
static gboolean is_rule_line(const char *line, gsize len) {
    if (len == 0)
        return FALSE;
    char marker = line[0];
    if (marker != '-' && marker != '*' && marker != '_')
        return FALSE;

    guint count = 0;
    for (gsize i = 0; i < len; i++) {
        if (line[i] == marker) {
            count++;
        } else if (line[i] != ' ' && line[i] != '\t') {
            return FALSE;
        }
    }
    return count >= 3;
}

static void process_line(NfLivePreview *preview, const char *line, gsize len,
                         gsize base) {
    if (len == 0)
        return;

    // Table rows stay raw in live preview; the reading view renders them.
    // A single pipe can be a wikilink display separator, so require a real
    // table-like row.
    guint pipe_count = 0;
    for (gsize i = 0; i < len; i++) {
        if (line[i] == '|')
            pipe_count++;
    }
    if (pipe_count >= 2)
        return;

    gsize lead = 0;
    while (lead < len && (line[lead] == ' ' || line[lead] == '\t'))
        lead++;
    if (lead == len)
        return;

    // Heading.
    guint level = 0;
    while (level < 6 && lead + level < len && line[lead + level] == '#')
        level++;
    if (level > 0 && lead + level < len && line[lead + level] == ' ') {
        gsize prefix = lead + level + 1;
        apply_tag_range(preview, preview->hidden, base,
                        base + line_chars(line, prefix));
        apply_tag_range(preview, preview->heading[level - 1],
                        base + line_chars(line, prefix),
                        base + line_chars(line, len));
        inline_scan(preview, line, len, base, prefix, len, 0);
        return;
    }

    // Horizontal rule.
    if (is_rule_line(line + lead, len - lead)) {
        apply_tag_range(preview, preview->dim, base,
                        base + line_chars(line, len));
        return;
    }

    // Block quote: hide all leading "> " markers.
    if (line[lead] == '>') {
        gsize pos = lead;
        while (pos < len && line[pos] == '>') {
            pos++;
            if (pos < len && line[pos] == ' ')
                pos++;
        }
        apply_tag_range(preview, preview->hidden, base,
                        base + line_chars(line, pos));
        apply_tag_range(preview, preview->quote, base,
                        base + line_chars(line, len));
        inline_scan(preview, line, len, base, pos, len, 0);
        return;
    }

    // List item. Ordered markers already look like their rendered form, so
    // only the unordered marker is hidden.
    gsize marker_len = 0;
    gboolean ordered = FALSE;
    if ((line[lead] == '-' || line[lead] == '*' || line[lead] == '+') &&
        lead + 1 < len && line[lead + 1] == ' ') {
        marker_len = 2;
    } else {
        gsize digits = lead;
        while (digits < len && g_ascii_isdigit(line[digits]))
            digits++;
        if (digits > lead && digits + 1 < len && line[digits] == '.' &&
            line[digits + 1] == ' ') {
            marker_len = digits - lead + 2;
            ordered = TRUE;
        }
    }
    if (marker_len > 0) {
        apply_tag_range(preview, preview->list, base,
                        base + line_chars(line, len));
        if (!ordered) {
            apply_tag_range(preview, preview->hidden,
                            base + line_chars(line, lead),
                            base + line_chars(line, lead + marker_len));
        }
        inline_scan(preview, line, len, base, lead + marker_len, len, 0);
        return;
    }

    inline_scan(preview, line, len, base, lead, len, 0);
}

// Keep live tags above syntax highlighting tags so hidden markers stay hidden
// after a style scheme change.
static void raise_live_tags(NfLivePreview *preview) {
    if (preview->buffer == NULL)
        return;

    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(preview->buffer);
    int top = (int)gtk_text_tag_table_get_size(table) - 1;
    if (top < 0)
        return;

    GtkTextTag *tags[] = {
        preview->hidden, preview->bold,   preview->italic, preview->strike,
        preview->code,   preview->quote,  preview->list,   preview->dim,
        preview->heading[0], preview->heading[1], preview->heading[2],
        preview->heading[3], preview->heading[4], preview->heading[5],
    };
    // Assign descending priorities so every live tag stays above the syntax
    // tags, with the hidden marker tag on top.
    int priority = top;
    for (gsize i = 0; i < G_N_ELEMENTS(tags); i++) {
        if (tags[i] == NULL)
            continue;
        if (priority < 0)
            break;
        gtk_text_tag_set_priority(tags[i], priority--);
    }
}

static void clear_live_tags(NfLivePreview *preview) {
    if (preview->buffer == NULL)
        return;

    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_bounds(preview->buffer, &start, &end);

    GtkTextTag *tags[] = {
        preview->hidden, preview->bold,   preview->italic, preview->strike,
        preview->code,   preview->quote,  preview->list,   preview->dim,
        preview->heading[0], preview->heading[1], preview->heading[2],
        preview->heading[3], preview->heading[4], preview->heading[5],
    };
    for (gsize i = 0; i < G_N_ELEMENTS(tags); i++) {
        if (tags[i] != NULL)
            gtk_text_buffer_remove_tag(preview->buffer, tags[i], &start, &end);
    }

    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(preview->buffer);
    for (guint i = 0; i < preview->link_tags->len; i++) {
        gtk_text_tag_table_remove(table,
                                  g_ptr_array_index(preview->link_tags, i));
    }
    g_ptr_array_set_size(preview->link_tags, 0);
}

void nf_live_preview_refresh(NfLivePreview *preview) {
    if (preview == NULL)
        return;

    if (preview->update_id != 0) {
        g_source_remove(preview->update_id);
        preview->update_id = 0;
    }
    if (preview->buffer == NULL)
        return;

    clear_live_tags(preview);
    if (!preview->enabled)
        return;

    raise_live_tags(preview);

    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_mark(preview->buffer, &cursor,
                                     gtk_text_buffer_get_insert(preview->buffer));
    int reveal_first = gtk_text_iter_get_line(&cursor);
    int reveal_last = reveal_first;

    GtkTextIter sel_start;
    GtkTextIter sel_end;
    if (gtk_text_buffer_get_selection_bounds(preview->buffer, &sel_start,
                                             &sel_end)) {
        int start_line = gtk_text_iter_get_line(&sel_start);
        int end_line = gtk_text_iter_get_line(&sel_end);
        if (start_line < reveal_first)
            reveal_first = start_line;
        if (end_line > reveal_last)
            reveal_last = end_line;
    }

    GtkTextIter bounds_start;
    GtkTextIter bounds_end;
    gtk_text_buffer_get_bounds(preview->buffer, &bounds_start, &bounds_end);
    char *text = gtk_text_buffer_get_text(preview->buffer, &bounds_start,
                                          &bounds_end, FALSE);

    const char *line = text;
    gsize char_offset = 0;
    int line_index = 0;
    while (TRUE) {
        const char *newline = strchr(line, '\n');
        gsize byte_len =
            newline != NULL ? (gsize)(newline - line) : strlen(line);

        if (line_index < reveal_first || line_index > reveal_last)
            process_line(preview, line, byte_len, char_offset);

        char_offset += line_chars(line, byte_len);
        if (newline == NULL)
            break;
        char_offset += 1;
        line = newline + 1;
        line_index++;
    }

    g_free(text);
}

static gboolean on_refresh_timeout(gpointer user_data) {
    NfLivePreview *preview = user_data;
    preview->update_id = 0;
    nf_live_preview_refresh(preview);
    return G_SOURCE_REMOVE;
}

void nf_live_preview_schedule(NfLivePreview *preview) {
    if (preview == NULL || preview->buffer == NULL || !preview->enabled)
        return;
    if (preview->update_id != 0)
        return;
    preview->update_id = g_timeout_add(REFRESH_DELAY_MS, on_refresh_timeout,
                                       preview);
}

void nf_live_preview_set_enabled(NfLivePreview *preview, gboolean enabled) {
    if (preview == NULL)
        return;
    preview->enabled = enabled;
    nf_live_preview_refresh(preview);
}

static void on_mark_set(GtkTextBuffer *buffer, GtkTextIter *iter,
                        GtkTextMark *mark, gpointer user_data) {
    (void)buffer;
    (void)iter;
    (void)mark;
    nf_live_preview_schedule(user_data);
}

static void on_cursor_changed(GObject *object, GParamSpec *pspec,
                              gpointer user_data) {
    (void)object;
    (void)pspec;
    nf_live_preview_schedule(user_data);
}

static void on_style_scheme_changed(GObject *object, GParamSpec *pspec,
                                    gpointer user_data) {
    (void)object;
    (void)pspec;
    NfLivePreview *preview = user_data;
    raise_live_tags(preview);
    nf_live_preview_refresh(preview);
}

static void on_click_pressed(GtkGestureClick *gesture, int n_press, double x,
                             double y, gpointer user_data) {
    NfLivePreview *preview = user_data;
    if (n_press != 1 || preview->view == NULL)
        return;

    GdkModifierType state = gtk_event_controller_get_current_event_state(
        GTK_EVENT_CONTROLLER(gesture));
    if (!(state & GDK_CONTROL_MASK))
        return;

    int buffer_x = 0;
    int buffer_y = 0;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(preview->view),
                                          GTK_TEXT_WINDOW_TEXT, (int)x, (int)y,
                                          &buffer_x, &buffer_y);
    GtkTextIter iter;
    if (!gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(preview->view), &iter,
                                            buffer_x, buffer_y))
        return;

    GSList *tags = gtk_text_iter_get_tags(&iter);
    for (GSList *item = tags; item != NULL; item = item->next) {
        GtkTextTag *tag = item->data;
        const char *target =
            g_object_get_data(G_OBJECT(tag), "nf-live-link-target");
        if (target == NULL)
            continue;
        gboolean wiki =
            GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tag), "nf-live-link-wiki"));
        if (wiki && preview->wiki_cb != NULL)
            preview->wiki_cb(target, preview->user_data);
        else if (!wiki && preview->uri_cb != NULL)
            preview->uri_cb(target, preview->user_data);
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        break;
    }
    g_slist_free(tags);
}

static void on_buffer_gone(gpointer data, GObject *object) {
    (void)object;
    ((NfLivePreview *)data)->buffer = NULL;
}

static void on_view_gone(gpointer data, GObject *object) {
    (void)object;
    ((NfLivePreview *)data)->view = NULL;
}

NfLivePreview *nf_live_preview_new(GtkSourceView *view, NfLiveWikiFunc wiki_cb,
                                   NfLiveUriFunc uri_cb, gpointer user_data) {
    NfLivePreview *preview = g_new0(NfLivePreview, 1);
    preview->view = view;
    preview->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    preview->wiki_cb = wiki_cb;
    preview->uri_cb = uri_cb;
    preview->user_data = user_data;
    preview->enabled = TRUE;
    preview->link_tags = g_ptr_array_new();

    g_object_weak_ref(G_OBJECT(preview->buffer), on_buffer_gone, preview);
    g_object_weak_ref(G_OBJECT(view), on_view_gone, preview);

    GtkTextBuffer *buffer = preview->buffer;
    GdkRGBA transparent = {0.0f, 0.0f, 0.0f, 0.0f};
    GdkRGBA dim = {0.0f, 0.0f, 0.0f, 0.45f};
    GdkRGBA code_bg = {0.5f, 0.5f, 0.5f, 0.16f};
    static const double heading_scales[6] = {1.7, 1.5, 1.3, 1.15, 1.05, 1.0};

    preview->hidden = gtk_text_buffer_create_tag(
        buffer, "nf-live-hidden", "foreground-rgba", &transparent, "scale", 0.01,
        NULL);
    preview->bold = gtk_text_buffer_create_tag(buffer, "nf-live-bold", "weight",
                                               PANGO_WEIGHT_BOLD, NULL);
    preview->italic = gtk_text_buffer_create_tag(buffer, "nf-live-italic",
                                                 "style", PANGO_STYLE_ITALIC,
                                                 NULL);
    preview->strike = gtk_text_buffer_create_tag(buffer, "nf-live-strike",
                                                 "strikethrough", TRUE, NULL);
    preview->code = gtk_text_buffer_create_tag(
        buffer, "nf-live-code", "family", "monospace", "background-rgba",
        &code_bg, NULL);
    preview->quote = gtk_text_buffer_create_tag(
        buffer, "nf-live-quote", "left-margin", 16, "style", PANGO_STYLE_ITALIC,
        NULL);
    preview->list = gtk_text_buffer_create_tag(buffer, "nf-live-list",
                                               "left-margin", 12, NULL);
    preview->dim = gtk_text_buffer_create_tag(buffer, "nf-live-dim",
                                              "foreground-rgba", &dim, NULL);

    for (guint i = 0; i < 6; i++) {
        preview->heading[i] = gtk_text_buffer_create_tag(
            buffer, NULL, "weight", PANGO_WEIGHT_BOLD, "scale",
            heading_scales[i], "pixels-above-lines", i < 2 ? 8 : 4, NULL);
    }

    preview->mark_set_id = g_signal_connect(buffer, "mark-set",
                                            G_CALLBACK(on_mark_set), preview);
    preview->cursor_id = g_signal_connect(
        buffer, "notify::cursor-position", G_CALLBACK(on_cursor_changed),
        preview);
    preview->scheme_id = g_signal_connect(
        buffer, "notify::style-scheme", G_CALLBACK(on_style_scheme_changed),
        preview);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    preview->click_id =
        g_signal_connect(click, "pressed", G_CALLBACK(on_click_pressed), preview);
    gtk_widget_add_controller(GTK_WIDGET(view), GTK_EVENT_CONTROLLER(click));
    preview->click = GTK_EVENT_CONTROLLER(click);

    return preview;
}

void nf_live_preview_free(NfLivePreview *preview) {
    if (preview == NULL)
        return;

    if (preview->update_id != 0)
        g_source_remove(preview->update_id);

    if (preview->buffer != NULL) {
        if (preview->mark_set_id != 0)
            g_signal_handler_disconnect(preview->buffer, preview->mark_set_id);
        if (preview->cursor_id != 0)
            g_signal_handler_disconnect(preview->buffer, preview->cursor_id);
        if (preview->scheme_id != 0)
            g_signal_handler_disconnect(preview->buffer, preview->scheme_id);
        g_object_weak_unref(G_OBJECT(preview->buffer), on_buffer_gone, preview);
    }

    if (preview->view != NULL) {
        if (preview->click != NULL && preview->click_id != 0)
            g_signal_handler_disconnect(preview->click, preview->click_id);
        if (preview->click != NULL)
            gtk_widget_remove_controller(GTK_WIDGET(preview->view),
                                         preview->click);
        g_object_weak_unref(G_OBJECT(preview->view), on_view_gone, preview);
    }

    g_ptr_array_free(preview->link_tags, TRUE);
    g_free(preview);
}
