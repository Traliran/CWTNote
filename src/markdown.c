// CWTNote - small Markdown renderer for the preview pane.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "markdown.h"

#include <string.h>

typedef struct {
    NfWikiLinkFunc wiki_cb;
    NfUriFunc uri_cb;
    gpointer user_data;
} MdCtx;

static void append_escaped_n(GString *out, const char *text, gsize len) {
    char *escaped = g_markup_escape_text(text, (gssize)len);
    g_string_append(out, escaped);
    g_free(escaped);
}

static char *inline_markup(const char *text) {
    GString *out = g_string_new(NULL);
    const char *p = text;

    while (*p != '\0') {
        if (g_str_has_prefix(p, "**") || g_str_has_prefix(p, "__")) {
            const char *marker = g_str_has_prefix(p, "**") ? "**" : "__";
            const char *close = strstr(p + 2, marker);
            if (close != NULL && close > p + 2) {
                g_string_append(out, "<b>");
                append_escaped_n(out, p + 2, (gsize)(close - (p + 2)));
                g_string_append(out, "</b>");
                p = close + 2;
                continue;
            }
        }

        if (g_str_has_prefix(p, "~~")) {
            const char *close = strstr(p + 2, "~~");
            if (close != NULL && close > p + 2) {
                g_string_append(out, "<s>");
                append_escaped_n(out, p + 2, (gsize)(close - (p + 2)));
                g_string_append(out, "</s>");
                p = close + 2;
                continue;
            }
        }

        if (g_str_has_prefix(p, "[[")) {
            const char *close = strstr(p + 2, "]]");
            if (close != NULL) {
                char *inner = g_strndup(p + 2, (gsize)(close - (p + 2)));
                char *bar = strchr(inner, '|');
                char *target = inner;
                char *display = inner;
                if (bar != NULL) {
                    *bar = '\0';
                    display = bar + 1;
                }
                g_strstrip(target);
                g_strstrip(display);
                gboolean valid = target[0] != '\0';
                if (valid) {
                    char *encoded = g_uri_escape_string(target, NULL, TRUE);
                    char *href = g_markup_escape_text(encoded, -1);
                    char *shown = g_markup_escape_text(
                        display[0] != '\0' ? display : target, -1);
                    g_string_append_printf(out,
                                           "<a href=\"nf-wiki:%s\">%s</a>",
                                           href, shown);
                    g_free(shown);
                    g_free(href);
                    g_free(encoded);
                }
                g_free(inner);
                if (valid) {
                    p = close + 2;
                    continue;
                }
            }
        }

        if (*p == '`') {
            const char *close = strchr(p + 1, '`');
            if (close != NULL && close > p + 1) {
                g_string_append(out, "<tt>");
                append_escaped_n(out, p + 1, (gsize)(close - (p + 1)));
                g_string_append(out, "</tt>");
                p = close + 1;
                continue;
            }
        }

        if (*p == '[' || g_str_has_prefix(p, "![")) {
            const char *label_start = (*p == '!') ? p + 2 : p + 1;
            const char *close_bracket = strchr(label_start, ']');
            if (close_bracket != NULL && close_bracket[1] == '(') {
                const char *url_start = close_bracket + 2;
                const char *url_end = strchr(url_start, ')');
                if (url_end != NULL) {
                    char *label = g_strndup(label_start,
                                            (gsize)(close_bracket - label_start));
                    char *url =
                        g_strndup(url_start, (gsize)(url_end - url_start));
                    char *shown = g_markup_escape_text(
                        label[0] != '\0' ? label : url, -1);
                    char *href = g_markup_escape_text(url, -1);
                    g_string_append_printf(out, "<a href=\"%s\">%s</a>",
                                           href, shown);
                    g_free(href);
                    g_free(shown);
                    g_free(url);
                    g_free(label);
                    p = url_end + 1;
                    continue;
                }
            }
        }

        if (*p == '*' || *p == '_') {
            char marker = *p;
            const char *close = strchr(p + 1, marker);
            if (close != NULL && close > p + 1 &&
                strchr(p + 1, '\n') == NULL) {
                g_string_append(out, "<i>");
                append_escaped_n(out, p + 1, (gsize)(close - (p + 1)));
                g_string_append(out, "</i>");
                p = close + 1;
                continue;
            }
        }

        const char *next = g_utf8_next_char(p);
        append_escaped_n(out, p, (gsize)(next - p));
        p = next;
    }

    return g_string_free(out, FALSE);
}

static gboolean on_activate_link(GtkLabel *label, const char *uri,
                                 gpointer user_data) {
    (void)label;
    MdCtx *ctx = user_data;
    if (uri == NULL)
        return TRUE;

    const char *wiki_prefix = "nf-wiki:";
    if (g_str_has_prefix(uri, wiki_prefix)) {
        const char *encoded = uri + strlen(wiki_prefix);
        char *target = g_uri_unescape_string(encoded, NULL);
        if (target == NULL)
            target = g_strdup(encoded);
        if (ctx->wiki_cb != NULL)
            ctx->wiki_cb(target, ctx->user_data);
        g_free(target);
        return TRUE;
    }

    if (ctx->uri_cb != NULL)
        ctx->uri_cb(uri, ctx->user_data);
    return TRUE;
}

static GtkWidget *make_label(const char *markup, MdCtx *ctx) {
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_widget_set_hexpand(label, TRUE);
    g_signal_connect(label, "activate-link", G_CALLBACK(on_activate_link),
                     ctx);
    return label;
}

static gboolean is_rule(const char *line) {
    if (line[0] == '\0')
        return FALSE;
    char marker = line[0];
    if (marker != '-' && marker != '*' && marker != '_')
        return FALSE;

    guint count = 0;
    for (const char *p = line; *p != '\0'; p++) {
        if (*p == marker)
            count++;
        else if (!g_ascii_isspace(*p))
            return FALSE;
    }
    return count >= 3;
}

static guint heading_level(const char *line) {
    guint level = 0;
    while (level < 6 && line[level] == '#')
        level++;
    if (level == 0 || line[level] != ' ')
        return 0;
    return level;
}

static gboolean list_marker(const char *line, gsize *marker_len) {
    if ((line[0] == '-' || line[0] == '*' || line[0] == '+') &&
        line[1] == ' ') {
        *marker_len = 2;
        return TRUE;
    }

    const char *p = line;
    while (g_ascii_isdigit(*p))
        p++;
    if (p > line && p[0] == '.' && p[1] == ' ') {
        *marker_len = (gsize)(p - line) + 2;
        return TRUE;
    }
    return FALSE;
}

GtkWidget *nf_markdown_render(const char *text, NfWikiLinkFunc wiki_cb,
                              NfUriFunc uri_cb, gpointer user_data) {
    MdCtx *ctx = g_new0(MdCtx, 1);
    ctx->wiki_cb = wiki_cb;
    ctx->uri_cb = uri_cb;
    ctx->user_data = user_data;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_valign(box, GTK_ALIGN_START);
    g_object_set_data_full(G_OBJECT(box), "nf-md-ctx", ctx, g_free);

    if (text == NULL)
        return box;

    char **lines = g_strsplit(text, "\n", -1);
    guint count = g_strv_length(lines);
    guint i = 0;

    while (i < count) {
        char *trimmed = g_strdup(lines[i]);
        g_strstrip(trimmed);

        if (trimmed[0] == '\0') {
            g_free(trimmed);
            i++;
            continue;
        }

        // Fenced code block.
        if (g_str_has_prefix(trimmed, "```") ||
            g_str_has_prefix(trimmed, "~~~")) {
            const char *fence = g_str_has_prefix(trimmed, "```") ? "```" : "~~~";
            GString *code = g_string_new(NULL);
            g_free(trimmed);
            i++;

            while (i < count) {
                char *check = g_strdup(lines[i]);
                g_strstrip(check);
                gboolean closing = g_str_has_prefix(check, fence);
                g_free(check);
                if (closing) {
                    i++;
                    break;
                }
                if (code->len > 0)
                    g_string_append_c(code, '\n');
                g_string_append(code, lines[i]);
                i++;
            }

            GtkWidget *label = gtk_label_new(code->str);
            gtk_widget_add_css_class(label, "nf-code");
            gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
            gtk_label_set_selectable(GTK_LABEL(label), TRUE);
            gtk_label_set_wrap(GTK_LABEL(label), TRUE);
            gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_CHAR);
            gtk_widget_set_hexpand(label, TRUE);
            gtk_box_append(GTK_BOX(box), label);
            g_string_free(code, TRUE);
            continue;
        }

        // Heading.
        guint level = heading_level(lines[i]);
        if (level > 0) {
            const char *content = lines[i] + level;
            while (*content == ' ')
                content++;
            char *markup = inline_markup(content);
            GtkWidget *label = make_label(markup, ctx);
            char *css = g_strdup_printf("nf-h%u", level);
            gtk_widget_add_css_class(label, css);
            g_free(css);
            gtk_widget_set_margin_top(label, level <= 2 ? 8 : 4);
            gtk_box_append(GTK_BOX(box), label);
            g_free(markup);
            g_free(trimmed);
            i++;
            continue;
        }

        // Block quote.
        if (trimmed[0] == '>') {
            const char *content = trimmed + 1;
            while (*content == ' ')
                content++;
            char *markup = inline_markup(content);
            GtkWidget *label = make_label(markup, ctx);
            gtk_widget_add_css_class(label, "nf-quote");
            gtk_box_append(GTK_BOX(box), label);
            g_free(markup);
            g_free(trimmed);
            i++;
            continue;
        }

        // Horizontal rule.
        if (is_rule(trimmed)) {
            GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
            gtk_widget_set_margin_top(separator, 6);
            gtk_widget_set_margin_bottom(separator, 6);
            gtk_box_append(GTK_BOX(box), separator);
            g_free(trimmed);
            i++;
            continue;
        }

        // List item.
        gsize marker_len = 0;
        if (list_marker(trimmed, &marker_len)) {
            const char *content = trimmed + marker_len;
            char *markup = inline_markup(content);
            char *prefixed = g_strconcat("•  ", markup, NULL);
            GtkWidget *label = make_label(prefixed, ctx);
            gtk_widget_add_css_class(label, "nf-list");
            gtk_widget_set_margin_start(label, 14);
            gtk_box_append(GTK_BOX(box), label);
            g_free(prefixed);
            g_free(markup);
            g_free(trimmed);
            i++;
            continue;
        }

        // Paragraph: join following ordinary lines.
        GString *paragraph = g_string_new(NULL);
        g_free(trimmed);

        while (i < count) {
            char *check = g_strdup(lines[i]);
            g_strstrip(check);
            if (check[0] == '\0' || heading_level(lines[i]) > 0 ||
                check[0] == '>' || is_rule(check) ||
                g_str_has_prefix(check, "```") ||
                g_str_has_prefix(check, "~~~") || list_marker(check, &marker_len)) {
                g_free(check);
                break;
            }
            g_free(check);

            if (paragraph->len > 0)
                g_string_append_c(paragraph, '\n');
            g_string_append(paragraph, lines[i]);
            i++;
        }

        if (paragraph->len > 0) {
            char *markup = inline_markup(paragraph->str);
            GtkWidget *label = make_label(markup, ctx);
            gtk_box_append(GTK_BOX(box), label);
            g_free(markup);
        }
        g_string_free(paragraph, TRUE);
    }

    g_strfreev(lines);
    return box;
}
