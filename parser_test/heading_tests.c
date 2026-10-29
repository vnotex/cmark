#include "cmark.h"
#include "test_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_basic_heading() {
    const char *markdown = "# Heading 1\n";
    const char *expected = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<!DOCTYPE document SYSTEM \"CommonMark.dtd\">\n"
                          "<document xmlns=\"http://commonmark.org/xml/1.0\">\n"
                          "  <heading level=\"1\">\n"
                          "    <text xml:space=\"preserve\">Heading 1</text>\n"
                          "  </heading>\n"
                          "</document>\n";
    return test_xml(markdown, expected, CMARK_OPT_DEFAULT);
}

static int test_multiple_heading_levels() {
    const char *markdown = "# Heading 1\n## Heading 2\n### Heading 3\n";
    const char *expected = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<!DOCTYPE document SYSTEM \"CommonMark.dtd\">\n"
                          "<document xmlns=\"http://commonmark.org/xml/1.0\">\n"
                          "  <heading level=\"1\">\n"
                          "    <text xml:space=\"preserve\">Heading 1</text>\n"
                          "  </heading>\n"
                          "  <heading level=\"2\">\n"
                          "    <text xml:space=\"preserve\">Heading 2</text>\n"
                          "  </heading>\n"
                          "  <heading level=\"3\">\n"
                          "    <text xml:space=\"preserve\">Heading 3</text>\n"
                          "  </heading>\n"
                          "</document>\n";
    return test_xml(markdown, expected, CMARK_OPT_DEFAULT);
}

static int test_heading_source_positions() {
    const char *markdown = "# Heading 1\n";
    const char *expected = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<!DOCTYPE document SYSTEM \"CommonMark.dtd\">\n"
                          "<document sourcepos=\"1:1-1:11\" xmlns=\"http://commonmark.org/xml/1.0\">\n"
                          "  <heading sourcepos=\"1:1-1:11\" level=\"1\">\n"
                          "    <text sourcepos=\"1:3-1:11\" xml:space=\"preserve\">Heading 1</text>\n"
                          "  </heading>\n"
                          "</document>\n";
    return test_xml(markdown, expected, CMARK_OPT_SOURCEPOS);
}

static int test_heading_with_emphasis() {
    const char *markdown = "# Heading with *emphasis*\n";
    const char *expected = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<!DOCTYPE document SYSTEM \"CommonMark.dtd\">\n"
                          "<document xmlns=\"http://commonmark.org/xml/1.0\">\n"
                          "  <heading level=\"1\">\n"
                          "    <text xml:space=\"preserve\">Heading with </text>\n"
                          "    <emph>\n"
                          "      <text xml:space=\"preserve\">emphasis</text>\n"
                          "    </emph>\n"
                          "  </heading>\n"
                          "</document>\n";
    return test_xml(markdown, expected, CMARK_OPT_DEFAULT);
}

static int test_setext_headings() {
    const char *markdown = "Heading 1\n=========\nHeading 2\n---------\n";
    const char *expected = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<!DOCTYPE document SYSTEM \"CommonMark.dtd\">\n"
                          "<document xmlns=\"http://commonmark.org/xml/1.0\">\n"
                          "  <heading level=\"1\">\n"
                          "    <text xml:space=\"preserve\">Heading 1</text>\n"
                          "  </heading>\n"
                          "  <heading level=\"2\">\n"
                          "    <text xml:space=\"preserve\">Heading 2</text>\n"
                          "  </heading>\n"
                          "</document>\n";
    return test_xml(markdown, expected, CMARK_OPT_DEFAULT);
}

static int test_atx_source_extent() {
    const struct {
        const char *markdown;
        int start_column;
        const char *title;
        int title_end_column;
        int followed_by_paragraph;
    } cases[] = {
        {"## Source-preserving concealment ##\n", 1, "Source-preserving concealment", 32, 0},
        {"## Title ##", 1, "Title", 8, 0},
        {"## Title ###\r\n", 1, "Title", 8, 0},
        {"## Title ##\r", 1, "Title", 8, 0},
        {"## Title ## \t\n", 1, "Title", 8, 0},
        {"## Title \t\n", 1, "Title", 8, 0},
        {"  ## Title ##\n", 3, "Title", 10, 0},
        {"> ## Title ##\n", 3, "Title", 10, 0},
        {"- ## Title ##\n", 3, "Title", 10, 0},
        {">\t##\tTitle\t##\n", 3, "Title", 10, 0},
        {"## \xe6\xa0\x87\xe9\xa2\x98 ##\n", 1, "\xe6\xa0\x87\xe9\xa2\x98", 9, 0},
        {"## ###\n", 1, "", 0, 0},
        {"## \t\n", 1, "", 0, 0},
        {"## Title##\n", 1, "Title##", 10, 0},
        {"## Title \\##\n", 1, "Title ##", 12, 0},
        {"## Title ##\nparagraph\n", 1, "Title", 8, 1}
    };
    const int options[] = {CMARK_OPT_DEFAULT, CMARK_OPT_SOURCEPOS,
                           CMARK_OPT_SOURCE_SYNTAX, CMARK_OPT_NO_EXTENSIONS};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        for (size_t j = 0; j < sizeof(options) / sizeof(options[0]); ++j) {
            cmark_node *doc = cmark_parse_document(cases[i].markdown,
                                                   strlen(cases[i].markdown), options[j]);
            cmark_iter *iter = cmark_iter_new(doc);
            cmark_node *heading = NULL;
            while (cmark_iter_next(iter) != CMARK_EVENT_DONE) {
                cmark_node *node = cmark_iter_get_node(iter);
                if (cmark_node_get_type(node) == CMARK_NODE_HEADING) {
                    heading = node;
                    break;
                }
            }
            const int end_column = (int)strcspn(cases[i].markdown, "\r\n");
            int ok = heading && cmark_node_get_start_line(heading) == 1 &&
                     cmark_node_get_start_column(heading) == cases[i].start_column &&
                     cmark_node_get_end_line(heading) == 1 &&
                     cmark_node_get_end_column(heading) == end_column;
            if (heading) {
                cmark_node *text = cmark_node_first_child(heading);
                if (cases[i].title_end_column) {
                    ok = ok && text && cmark_node_get_type(text) == CMARK_NODE_TEXT &&
                         strcmp(cmark_node_get_literal(text), cases[i].title) == 0 &&
                         cmark_node_get_end_column(text) == cases[i].title_end_column;
                } else {
                    ok = ok && text == NULL;
                }
                for (cmark_node *parent = cmark_node_parent(heading); parent;
                     parent = cmark_node_parent(parent)) {
                    const int end_line = cases[i].followed_by_paragraph ? 2 : 1;
                    const int parent_end = cases[i].followed_by_paragraph ? 9 : end_column;
                    ok = ok && cmark_node_get_end_line(parent) == end_line &&
                         cmark_node_get_end_column(parent) == parent_end;
                }
                char expected_html[128];
                snprintf(expected_html, sizeof(expected_html), "<h2>%s</h2>\n", cases[i].title);
                char *html = cmark_render_html(heading, CMARK_OPT_DEFAULT);
                ok = ok && html && strcmp(html, expected_html) == 0;
                free(html);
            }
            if (!ok) {
                fprintf(stderr, "ATX extent/title mismatch (case %zu, options %d): %s\n"
                                "Expected heading 1:%d-1:%d, got %d:%d-%d:%d\n",
                        i, options[j], cases[i].markdown, cases[i].start_column, end_column,
                        cmark_node_get_start_line(heading), cmark_node_get_start_column(heading),
                        cmark_node_get_end_line(heading), cmark_node_get_end_column(heading));
            }
            cmark_iter_free(iter);
            cmark_node_free(doc);
            if (!ok) {
                return 0;
            }
        }
    }
    return 1;
}

int main() {
    CASE(test_basic_heading);
    CASE(test_multiple_heading_levels);
    CASE(test_heading_source_positions);
    CASE(test_atx_source_extent);
    CASE(test_heading_with_emphasis);
    CASE(test_setext_headings);
    return 0;
}
