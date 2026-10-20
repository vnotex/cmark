#include <cmark.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_utils.h"

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      return 0;                                                                \
    }                                                                          \
  } while (0)

static cmark_node *parse(const char *source, int options) {
  return cmark_parse_document(source, strlen(source), options);
}

static cmark_node *nth(cmark_node *root, cmark_node_type type, unsigned index) {
  cmark_iter *iter = cmark_iter_new(root);
  cmark_node *result = NULL;
  cmark_event_type event;
  while ((event = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter);
    if (event == CMARK_EVENT_ENTER && cmark_node_get_type(node) == type &&
        !index--) {
      result = node;
      break;
    }
  }
  cmark_iter_free(iter);
  return result;
}

static unsigned count(cmark_node *root, cmark_node_type type) {
  cmark_iter *iter = cmark_iter_new(root);
  unsigned result = 0;
  cmark_event_type event;
  while ((event = cmark_iter_next(iter)) != CMARK_EVENT_DONE)
    if (event == CMARK_EVENT_ENTER &&
        cmark_node_get_type(cmark_iter_get_node(iter)) == type)
      ++result;
  cmark_iter_free(iter);
  return result;
}

static int option_isolation(void) {
  const char *source =
      "::: alert-info\n\n- [X] H~2~O x^2^ example.com :smile:\n\n:::";
  cmark_node *doc = parse(source, CMARK_OPT_DEFAULT);
  CHECK(count(doc, CMARK_NODE_ALERT) == 0);
  CHECK(count(doc, CMARK_NODE_SUBSCRIPT) == 0);
  CHECK(count(doc, CMARK_NODE_SUPERSCRIPT) == 0);
  CHECK(count(doc, CMARK_NODE_LINK) == 0);
  CHECK(cmark_node_get_task_checked(nth(doc, CMARK_NODE_ITEM, 0)) == -1);
  cmark_node_free(doc);
  doc = parse(source,
              CMARK_OPT_VIEWER | CMARK_OPT_LINKIFY | CMARK_OPT_NO_EXTENSIONS);
  CHECK(count(doc, CMARK_NODE_ALERT) == 0);
  CHECK(count(doc, CMARK_NODE_SUBSCRIPT) == 0);
  CHECK(count(doc, CMARK_NODE_SUPERSCRIPT) == 0);
  CHECK(count(doc, CMARK_NODE_LINK) == 0);
  CHECK(cmark_node_get_task_checked(nth(doc, CMARK_NODE_ITEM, 0)) == -1);
  cmark_node_free(doc);
  doc = parse(source, CMARK_OPT_VIEWER | CMARK_OPT_LINKIFY);
  CHECK(count(doc, CMARK_NODE_ALERT) == 1);
  CHECK(count(doc, CMARK_NODE_SUBSCRIPT) == 1);
  CHECK(count(doc, CMARK_NODE_SUPERSCRIPT) == 1);
  CHECK(count(doc, CMARK_NODE_LINK) == 1);
  CHECK(cmark_node_get_task_checked(nth(doc, CMARK_NODE_ITEM, 0)) == 1);
  char *xml = cmark_render_xml(doc, 0);
  CHECK(strstr(xml, ":smile:") != NULL);
  free(xml);
  cmark_node_free(doc);
  return 1;
}

static int html_policy(void) {
  const char *source =
      "<div>\n*text*\n</div>\n\n<b>inline</b> <https://example.com>";
  cmark_node *doc =
      parse(source, CMARK_OPT_HTML_AS_TEXT | CMARK_OPT_NO_EXTENSIONS);
  CHECK(count(doc, CMARK_NODE_HTML_BLOCK) == 0);
  CHECK(count(doc, CMARK_NODE_HTML_INLINE) == 0);
  CHECK(count(doc, CMARK_NODE_EMPH) == 1);
  CHECK(count(doc, CMARK_NODE_LINK) == 1);
  char *html = cmark_render_html(doc, CMARK_OPT_UNSAFE);
  CHECK(strstr(html, "&lt;div&gt;") != NULL);
  CHECK(strstr(html, "<b>") == NULL);
  free(html);
  cmark_node_free(doc);
  doc = parse(source, CMARK_OPT_DEFAULT);
  CHECK(count(doc, CMARK_NODE_HTML_BLOCK) == 1);
  CHECK(count(doc, CMARK_NODE_HTML_INLINE) == 2);
  cmark_node_free(doc);
  return 1;
}

static int scripts_and_tasks(void) {
  cmark_node *doc =
      parse("H~2~O x^2^ ~~strike~~ ~a\\ b~ ^a_b^ ~bad space~ ^open\n",
            CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_SUBSCRIPT) == 2);
  CHECK(count(doc, CMARK_NODE_SUPERSCRIPT) == 2);
  CHECK(count(doc, CMARK_NODE_STRIKETHROUGH) == 1);
  CHECK(strcmp(cmark_node_get_literal(
                   cmark_node_first_child(nth(doc, CMARK_NODE_SUBSCRIPT, 1))),
               "a b") == 0);
  CHECK(strcmp(cmark_node_get_literal(
                   cmark_node_first_child(nth(doc, CMARK_NODE_SUPERSCRIPT, 1))),
               "a_b") == 0);
  CHECK(cmark_node_get_start_column(nth(doc, CMARK_NODE_SUBSCRIPT, 0)) == 2);
  CHECK(cmark_node_get_end_column(nth(doc, CMARK_NODE_SUBSCRIPT, 0)) == 4);
  char *markdown = cmark_render_commonmark(doc, 0, 0);
  CHECK(strstr(markdown, "~a\\ b~") != NULL);
  free(markdown);
  cmark_node_free(doc);

  doc = parse("- [X] outer\r\n  - [ ] inner\r\n\r\n- ordinary\r\n\r\n  [x] "
              "not-first\r\n- [x]not-task\r\n",
              CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_ITEM) == 4);
  CHECK(cmark_node_get_task_checked(nth(doc, CMARK_NODE_ITEM, 0)) == 1);
  CHECK(cmark_node_get_task_checked(nth(doc, CMARK_NODE_ITEM, 1)) == 0);
  CHECK(cmark_node_get_task_checked(nth(doc, CMARK_NODE_ITEM, 2)) == -1);
  CHECK(cmark_node_get_task_checked(nth(doc, CMARK_NODE_ITEM, 3)) == -1);
  CHECK(cmark_node_get_start_line(nth(doc, CMARK_NODE_ITEM, 1)) == 2);
  cmark_node *first_text = cmark_node_first_child(
      cmark_node_first_child(nth(doc, CMARK_NODE_ITEM, 0)));
  CHECK(strcmp(cmark_node_get_literal(first_text), "outer") == 0);
  CHECK(cmark_node_get_start_column(first_text) == 7);
  cmark_node_free(doc);
  return 1;
}

static int alerts_and_directives(void) {
  const char *source =
      ":::: alert-warning\nouter\n\n::: alert-info\ninner\n:::\n\n"
      "```text\n::::\n```\n::::\n\n# after\n";
  cmark_node *doc = parse(source, CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_ALERT) == 2);
  CHECK(count(doc, CMARK_NODE_CODE_BLOCK) == 1);
  CHECK(strcmp(cmark_node_get_alert_type(nth(doc, CMARK_NODE_ALERT, 0)),
               "alert-warning") == 0);
  CHECK(strcmp(cmark_node_get_alert_type(nth(doc, CMARK_NODE_ALERT, 1)),
               "alert-info") == 0);
  CHECK(cmark_node_parent(nth(doc, CMARK_NODE_HEADING, 0)) == doc);
  cmark_node_free(doc);
  return 1;
}

static int toc_forms(void) {
  const char *source = "[toc]\n${TOC}\n[[_toc_]]\n$<toc{\"level\":[2,3],"
                       "\"listType\":\"ul\"}>\n\n"
                       "toc\n\n[not-toc]\n\n::: alert-info extra\n";
  cmark_node *doc = parse(source, CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_TOC) == 4);
  CHECK(strcmp(cmark_node_get_toc_options(nth(doc, CMARK_NODE_TOC, 3)),
               "{\"level\":[2,3],\"listType\":\"ul\"}") == 0);
  CHECK(count(doc, CMARK_NODE_ALERT) == 0);
  cmark_node *toc = nth(doc, CMARK_NODE_TOC, 0);
  CHECK(cmark_node_is_leaf(toc));
  cmark_node *text = cmark_node_new(CMARK_NODE_TEXT);
  CHECK(!cmark_node_append_child(toc, text));
  cmark_node_free(text);
  cmark_node_free(doc);
  return 1;
}

static int links_and_positions(void) {
  const char *source =
      "See https://example.org/a_(b). www.example.org, person+tag@example.com; "
      "https://\xe4\xbe\x8b\xe5\xad\x90.\xe6\xb5\x8b\xe8\xaf\x95/path "
      "https://example.org/\xc3\xa9\n";
  const char *urls[] = {"https://example.org/a_(b)", "http://www.example.org",
                        "mailto:person+tag@example.com",
                        "https://xn--fsqu00a.xn--0zwm56d/path",
                        "https://example.org/%C3%A9"};
  cmark_node *doc = parse(source, CMARK_OPT_VIEWER | CMARK_OPT_LINKIFY);
  CHECK(count(doc, CMARK_NODE_LINK) == 5);
  for (unsigned i = 0; i < 5; ++i) {
    cmark_node *link = nth(doc, CMARK_NODE_LINK, i);
    CHECK(strcmp(cmark_node_get_url(link), urls[i]) == 0);
    CHECK(cmark_node_get_start_line(link) == 1);
    CHECK(cmark_node_get_end_line(link) == 1);
    CHECK(cmark_node_get_url_start_column(link) ==
          cmark_node_get_start_column(link));
    CHECK(cmark_node_get_url_end_column(link) ==
          cmark_node_get_end_column(link));
    const char *text = cmark_node_get_literal(cmark_node_first_child(link));
    CHECK(cmark_node_get_end_column(link) - cmark_node_get_start_column(link) +
              1 ==
          (int)strlen(text));
    CHECK(strncmp(source + cmark_node_get_start_column(link) - 1, text,
                  strlen(text)) == 0);
  }
  cmark_node_free(doc);
  doc = parse(
      "[www.example.com](target) [[www.example.com]] ![www.example.com](image) "
      "`www.example.com` $www.example.com$ <a "
      "href='target'>www.example.com</a> "
      "example.txt 127.0.0.1 http\\://example.com\n",
      CMARK_OPT_VIEWER | CMARK_OPT_LINKIFY);
  CHECK(count(doc, CMARK_NODE_LINK) == 1);
  CHECK(count(doc, CMARK_NODE_WIKILINK) == 1);
  cmark_node_free(doc);
  doc = parse("\xef\xbc\x88www.example.com\xef\xbc\x89  \n", CMARK_OPT_LINKIFY);
  CHECK(count(doc, CMARK_NODE_LINK) == 1);
  CHECK(cmark_node_get_start_column(nth(doc, CMARK_NODE_LINK, 0)) == 4);
  CHECK(cmark_node_get_end_column(nth(doc, CMARK_NODE_LINK, 0)) == 18);
  cmark_node_free(doc);
  doc = parse("https://example.com/a?one=1&amp;two=2\nnext www.example.org\n",
              CMARK_OPT_LINKIFY);
  CHECK(count(doc, CMARK_NODE_LINK) == 2);
  CHECK(strcmp(cmark_node_get_url(nth(doc, CMARK_NODE_LINK, 0)),
               "https://example.com/a?one=1&two=2") == 0);
  CHECK(cmark_node_get_start_line(nth(doc, CMARK_NODE_LINK, 1)) == 2);
  cmark_node_free(doc);
  return 1;
}

static int linkify_after_separators(void) {
  const char *sources[] = {"word example.com\n",   "word   example.com\n",
                           "word (example.com)\n", "**bold** example.com\n",
                           "`code` example.com\n", "x^2^ example.com\n"};
  for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
    cmark_node *doc = parse(sources[i], CMARK_OPT_VIEWER | CMARK_OPT_LINKIFY);
    CHECK(count(doc, CMARK_NODE_LINK) == 1);
    CHECK(strcmp(cmark_node_get_url(nth(doc, CMARK_NODE_LINK, 0)),
                 "http://example.com") == 0);
    cmark_node_free(doc);
  }
  return 1;
}

static int tables_and_dimensions(void) {
  const char *source = "a | b\n:-- | --:\n1 | 2 | ignored\nshort\n# after\n";
  cmark_node *doc = parse(source, CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_TABLE) == 1);
  CHECK(count(doc, CMARK_NODE_TABLE_ROW) == 4);
  CHECK(count(doc, CMARK_NODE_TABLE_CELL) == 8);
  CHECK(count(doc, CMARK_NODE_HEADING) == 1);
  CHECK(cmark_node_get_start_line(nth(doc, CMARK_NODE_HEADING, 0)) == 5);
  CHECK(cmark_node_get_start_column(nth(doc, CMARK_NODE_TABLE_ROW, 0)) == 1);
  cmark_node_free(doc);
  doc = parse(source, CMARK_OPT_DEFAULT);
  CHECK(count(doc, CMARK_NODE_TABLE) == 0);
  cmark_node_free(doc);
  doc =
      parse("| one |\n---\n| $x$ and a\\|b and `c\\|d` |\n", CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_TABLE) == 1);
  CHECK(count(doc, CMARK_NODE_FORMULA_INLINE) == 1);
  CHECK(strcmp(cmark_node_get_literal(nth(doc, CMARK_NODE_CODE, 0)), "c|d") ==
        0);
  cmark_node_free(doc);
  doc = parse("![a](image =50%x25%) ![b](image =x200) ![c](image =100x) "
              "![d](<> =0x0) ![bad](image =50%px20)\n",
              CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_IMAGE) == 4);
  CHECK(strcmp(cmark_node_get_image_width_text(nth(doc, CMARK_NODE_IMAGE, 0)),
               "50%") == 0);
  CHECK(strcmp(cmark_node_get_image_height_text(nth(doc, CMARK_NODE_IMAGE, 0)),
               "25%") == 0);
  CHECK(cmark_node_get_image_width(nth(doc, CMARK_NODE_IMAGE, 0)) == 0);
  CHECK(cmark_node_get_image_height(nth(doc, CMARK_NODE_IMAGE, 1)) == 200);
  CHECK(strcmp(cmark_node_get_image_width_text(nth(doc, CMARK_NODE_IMAGE, 1)),
               "") == 0);
  CHECK(strcmp(cmark_node_get_image_height_text(nth(doc, CMARK_NODE_IMAGE, 2)),
               "") == 0);
  CHECK(strcmp(cmark_node_get_image_width_text(nth(doc, CMARK_NODE_IMAGE, 3)),
               "0") == 0);
  cmark_node_free(doc);
  return 1;
}

static int math_boundaries(void) {
  cmark_node *doc = parse(
      "$$x+y$$ (1)\n\n\\begin{align}\na &= b\\\\\n\n c &= d\n\\end{align}\n\n"
      "inline $$a\nb$$ after\n",
      CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_FORMULA_BLOCK) == 1);
  CHECK(strcmp(cmark_node_get_formula_label(
                   nth(doc, CMARK_NODE_FORMULA_INLINE, 0)),
               "1") == 0);
  CHECK(strcmp(cmark_node_get_literal(nth(doc, CMARK_NODE_FORMULA_INLINE, 0)),
               "x+y") == 0);
  CHECK(strstr(cmark_node_get_literal(nth(doc, CMARK_NODE_FORMULA_BLOCK, 0)),
               "\\begin{align}") != NULL);
  CHECK(strstr(cmark_node_get_literal(nth(doc, CMARK_NODE_FORMULA_BLOCK, 0)),
               "\\end{align}") != NULL);
  CHECK(count(doc, CMARK_NODE_FORMULA_INLINE) == 1);
  CHECK(cmark_node_get_formula_display(nth(doc, CMARK_NODE_FORMULA_INLINE, 0)));
  CHECK(cmark_node_get_start_line(nth(doc, CMARK_NODE_FORMULA_INLINE, 0)) == 1);
  CHECK(cmark_node_get_end_line(nth(doc, CMARK_NODE_FORMULA_INLINE, 0)) == 1);
  cmark_node_free(doc);
  doc = parse("\\begin \n{align}x\\end{align}\n", CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_FORMULA_BLOCK) == 1);
  CHECK(strstr(cmark_node_get_literal(nth(doc, CMARK_NODE_FORMULA_BLOCK, 0)),
               "{align}x\\end{align}") != NULL);
  cmark_node_free(doc);
  doc = parse("\\begin{align}\ntext\n\\end{other}\n\n# surviving heading\n",
              CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_FORMULA_BLOCK) == 0);
  CHECK(count(doc, CMARK_NODE_HEADING) == 1);
  CHECK(cmark_node_get_start_line(nth(doc, CMARK_NODE_HEADING, 0)) == 5);
  cmark_node_free(doc);
  doc = parse("$$unclosed\n\n# surviving heading\n", CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_FORMULA_BLOCK) == 0);
  CHECK(count(doc, CMARK_NODE_HEADING) == 1);
  cmark_node_free(doc);
  doc = parse("$$bad$$ tail\n\n$$good$$\n", CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_FORMULA_BLOCK) == 0);
  CHECK(count(doc, CMARK_NODE_FORMULA_INLINE) == 2);
  CHECK(strcmp(cmark_node_get_literal(nth(doc, CMARK_NODE_FORMULA_INLINE, 1)),
               "good") == 0);
  cmark_node_free(doc);
  // The original texmath rule skips whitespace-rejected dollar candidates;
  // adjacent malformed samples can therefore form valid cross-sample math.
  doc = parse("$ spaced$ $x $ 2$x$ $x$2 \\(not math\\) `$x$`\n",
              CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_FORMULA_INLINE) == 2);
  CHECK(strcmp(cmark_node_get_literal(nth(doc, CMARK_NODE_FORMULA_INLINE, 0)),
               "x $ 2") == 0);
  CHECK(strcmp(cmark_node_get_literal(nth(doc, CMARK_NODE_FORMULA_INLINE, 1)),
               "2 \\(not math\\) `") == 0);
  cmark_node_free(doc);
  const char *literal_cases[] = {"$ spaced$\n", "$x $\n",           "2$x$\n",
                                 "$x$2\n",      "\\(not math\\)\n", "`$x$`\n"};
  for (size_t i = 0; i < sizeof(literal_cases) / sizeof(literal_cases[0]);
       ++i) {
    doc = parse(literal_cases[i], CMARK_OPT_VIEWER);
    CHECK(count(doc, CMARK_NODE_FORMULA_INLINE) == 0);
    cmark_node_free(doc);
  }
  return 1;
}

static int display_math_source_placement(void) {
  const struct {
    const char *source;
    cmark_node_type parent;
    int line;
    int column;
  } cases[] = {
      {"$$x^2$$\n", CMARK_NODE_PARAGRAPH, 1, 3},
      {"before $$x^2$$ after\n", CMARK_NODE_PARAGRAPH, 1, 10},
      {"- $$x^2$$\n", CMARK_NODE_PARAGRAPH, 1, 5},
      {"| math |\n| --- |\n| $$x^2$$ |\n", CMARK_NODE_TABLE_CELL, 3, 5},
      {"# $$x^2$$ (tag)\n", CMARK_NODE_HEADING, 1, 5}};
  const int profiles[] = {CMARK_OPT_DEFAULT, CMARK_OPT_VIEWER};
  for (size_t profile = 0; profile < sizeof(profiles) / sizeof(profiles[0]);
       ++profile) {
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      cmark_node *doc = parse(cases[i].source, profiles[profile]);
      CHECK(count(doc, CMARK_NODE_FORMULA_BLOCK) == 0);
      CHECK(count(doc, CMARK_NODE_FORMULA_INLINE) == 1);
      cmark_node *formula = nth(doc, CMARK_NODE_FORMULA_INLINE, 0);
      CHECK(cmark_node_get_type(cmark_node_parent(formula)) == cases[i].parent);
      CHECK(cmark_node_get_formula_display(formula));
      CHECK(strcmp(cmark_node_get_literal(formula), "x^2") == 0);
      CHECK(strcmp(cmark_node_get_formula_label(formula), "") == 0);
      CHECK(cmark_node_get_start_line(formula) == cases[i].line);
      CHECK(cmark_node_get_end_line(formula) == cases[i].line);
      CHECK(cmark_node_get_start_column(formula) == cases[i].column);
      CHECK(cmark_node_get_end_column(formula) == cases[i].column + 2);
      cmark_node_free(doc);
    }
    cmark_node *doc = parse("$$\nx^2\n$$\n", profiles[profile]);
    CHECK(count(doc, CMARK_NODE_FORMULA_BLOCK) == 1);
    CHECK(count(doc, CMARK_NODE_FORMULA_INLINE) == 0);
    cmark_node_free(doc);
    doc = parse("inline $$a\nb$$ after\n", profiles[profile]);
    CHECK(count(doc, CMARK_NODE_FORMULA_INLINE) == 0);
    CHECK(count(doc, CMARK_NODE_FORMULA_BLOCK) == 0);
    cmark_node_free(doc);
  }
  cmark_node *doc =
      parse("$$x^2$$ (tag)\n\n- $$y^2$$ (other)\n", CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_FORMULA_BLOCK) == 0);
  CHECK(count(doc, CMARK_NODE_FORMULA_INLINE) == 2);
  cmark_node *formula = nth(doc, CMARK_NODE_FORMULA_INLINE, 0);
  CHECK(strcmp(cmark_node_get_formula_label(formula), "tag") == 0);
  CHECK(cmark_node_get_start_column(formula) == 3);
  CHECK(cmark_node_get_end_column(formula) == 5);
  CHECK(cmark_node_next(formula) == NULL);
  formula = nth(doc, CMARK_NODE_FORMULA_INLINE, 1);
  CHECK(strcmp(cmark_node_get_formula_label(formula), "other") == 0);
  CHECK(cmark_node_get_start_column(formula) == 5);
  CHECK(cmark_node_get_end_column(formula) == 7);
  CHECK(cmark_node_next(formula) == NULL);
  cmark_node_free(doc);
  doc = parse(
      "before $$x$$ (not-a-tag)\n\n| math |\n| --- |\n| $$x$$ (not-a-tag) |\n",
      CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_FORMULA_INLINE) == 2);
  for (unsigned i = 0; i < 2; ++i) {
    formula = nth(doc, CMARK_NODE_FORMULA_INLINE, i);
    CHECK(strcmp(cmark_node_get_formula_label(formula), "") == 0);
    CHECK(cmark_node_get_type(cmark_node_next(formula)) == CMARK_NODE_TEXT);
    CHECK(strstr(cmark_node_get_literal(cmark_node_next(formula)),
                 "(not-a-tag)") != NULL);
  }
  cmark_node_free(doc);
  return 1;
}

static int dollar_rule_precedence(void) {
  const char *sources[] = {"$$$x$\n", "$$x$y$\n", "text $$$$$\n", "$$x$$2\n",
                           "2$$x$\n"};
  const char *contents[] = {"$", "$x", "$", "$x", "x"};
  for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
    cmark_node *doc = parse(sources[i], CMARK_OPT_VIEWER);
    CHECK(count(doc, CMARK_NODE_FORMULA_INLINE) == 1);
    cmark_node *formula = nth(doc, CMARK_NODE_FORMULA_INLINE, 0);
    CHECK(strcmp(cmark_node_get_literal(formula), contents[i]) == 0);
    CHECK(cmark_node_get_formula_display(formula) == (i == 2));
    cmark_node_free(doc);
  }
  return 1;
}

static int footnote_reference_order(void) {
  const char *source = "[^unused]: ^[hidden]\n\n[^a]: first[^b]\n\n"
                       "[^b]: second\n\nbody[^a] ^[visible]\n";
  cmark_node *doc = parse(source, CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_INLINE_FOOTNOTE) == 1);
  char *xml = cmark_render_xml(doc, 0);
  CHECK(strstr(xml, "label=\"a\" number=\"1\"") != NULL);
  CHECK(strstr(xml, "inline_footnote number=\"2\"") != NULL);
  CHECK(strstr(xml, "label=\"b\" number=\"3\"") != NULL);
  free(xml);
  cmark_node_free(doc);
  return 1;
}

static int footnote_continuation_indent(void) {
  const char *source = "reference[^longlabel]\n\n  [^longlabel]: intro\n\n"
                       "    ## Nested heading\n\n        nested code\n";
  cmark_node *doc = parse(source, CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_HEADING) == 1);
  CHECK(count(doc, CMARK_NODE_CODE_BLOCK) == 1);
  cmark_node *heading = nth(doc, CMARK_NODE_HEADING, 0);
  CHECK(cmark_node_get_type(cmark_node_parent(heading)) ==
        CMARK_NODE_FOOTNOTE_DEFINITION);
  CHECK(cmark_node_get_start_line(heading) == 5);
  CHECK(cmark_node_get_start_column(heading) == 5);
  cmark_node *code = nth(doc, CMARK_NODE_CODE_BLOCK, 0);
  CHECK(cmark_node_parent(code) == cmark_node_parent(heading));
  CHECK(strcmp(cmark_node_get_literal(code), "nested code\n") == 0);
  cmark_node_free(doc);

  doc = parse(source, CMARK_OPT_DEFAULT);
  CHECK(count(doc, CMARK_NODE_HEADING) == 0);
  code = nth(doc, CMARK_NODE_CODE_BLOCK, 0);
  CHECK(code != NULL);
  CHECK(cmark_node_get_type(cmark_node_parent(code)) == CMARK_NODE_DOCUMENT);
  CHECK(strstr(cmark_node_get_literal(code), "## Nested heading") != NULL);
  cmark_node_free(doc);
  return 1;
}

static int formula_labels_in_all_renderers(void) {
  const char *source =
      "$$x^2$$ (tag&1)\n\n$$\ny^2\n$$ (block_2)\n\ninline $z$\n";
  cmark_node *doc = parse(source, CMARK_OPT_VIEWER);
  CHECK(count(doc, CMARK_NODE_FORMULA_INLINE) == 2);
  CHECK(count(doc, CMARK_NODE_FORMULA_BLOCK) == 1);
  char *markdown = cmark_render_commonmark(doc, 0, 0);
  char *man = cmark_render_man(doc, 0, 0);
  char *latex = cmark_render_latex(doc, 0, 0);
  char *xml = cmark_render_xml(doc, 0);
  CHECK(strstr(markdown, "$$x^2$$ (tag&1)") != NULL);
  CHECK(strstr(markdown, "$$ (block_2)") != NULL);
  CHECK(strstr(markdown, "$z$") != NULL);
  CHECK(strstr(man, "x^2 (tag&1)") != NULL);
  CHECK(strstr(man, "y^2") != NULL);
  CHECK(strstr(man, "(block_2)") != NULL);
  CHECK(strstr(latex, "\\[x^2\\quad\\mbox{(tag\\&1)}\\]") != NULL);
  CHECK(strstr(latex, "y^2") != NULL);
  CHECK(strstr(latex, "\\quad\\mbox{(block\\_2)}\\]") != NULL);
  CHECK(strstr(latex, "\\(z\\)") != NULL);
  CHECK(strstr(xml, "<formula_inline display=\"true\" label=\"tag&amp;1\"") !=
        NULL);
  CHECK(strstr(xml, "<formula_block label=\"block_2\"") != NULL);
  cmark_node *roundtrip = parse(markdown, CMARK_OPT_VIEWER);
  CHECK(count(roundtrip, CMARK_NODE_FORMULA_INLINE) == 2);
  CHECK(count(roundtrip, CMARK_NODE_FORMULA_BLOCK) == 1);
  CHECK(strcmp(cmark_node_get_formula_label(
                   nth(roundtrip, CMARK_NODE_FORMULA_INLINE, 0)),
               "tag&1") == 0);
  CHECK(strcmp(cmark_node_get_formula_label(
                   nth(roundtrip, CMARK_NODE_FORMULA_BLOCK, 0)),
               "block_2") == 0);
  cmark_node_free(roundtrip);
  free(markdown);
  free(man);
  free(latex);
  free(xml);
  cmark_node_free(doc);

  // Default callers keep the previous non-HTML formula behavior. Text on
  // either side survives while unlabeled legacy formula nodes emit nothing.
  doc = parse("before $legacy$ after\n", CMARK_OPT_DEFAULT);
  markdown = cmark_render_commonmark(doc, 0, 0);
  man = cmark_render_man(doc, 0, 0);
  latex = cmark_render_latex(doc, 0, 0);
  CHECK(strstr(markdown, "before  after") != NULL &&
        strstr(markdown, "legacy") == NULL);
  CHECK(strstr(man, "before  after") != NULL && strstr(man, "legacy") == NULL);
  CHECK(strstr(latex, "before  after") != NULL &&
        strstr(latex, "legacy") == NULL);
  free(markdown);
  free(man);
  free(latex);
  cmark_node_free(doc);
  return 1;
}

static int streaming_and_renderers(void) {
  const char *source =
      "::: alert-info\r\n\r\n- [x] ~a\\ b~ ^2^ www.example.com\r\n\r\n"
      "[toc]\r\n\r\n# title\r\n\r\n[^n]: note\r\n\r\n"
      "reference[^n] again[^n] ^[inline]\r\n\r\n:::\r\n";
  int options = CMARK_OPT_VIEWER | CMARK_OPT_LINKIFY;
  cmark_node *doc = parse(source, options);
  CHECK(count(doc, CMARK_NODE_FOOTNOTE_REFERENCE) == 2);
  CHECK(count(doc, CMARK_NODE_INLINE_FOOTNOTE) == 1);
  cmark_parser *parser = cmark_parser_new(options);
  for (size_t i = 0; i < strlen(source); ++i)
    cmark_parser_feed(parser, source + i, 1);
  cmark_node *streamed = cmark_parser_finish(parser);
  cmark_parser_free(parser);
  char *xml = cmark_render_xml(doc, CMARK_OPT_SOURCEPOS);
  char *other = cmark_render_xml(streamed, CMARK_OPT_SOURCEPOS);
  CHECK(strcmp(xml, other) == 0);
  free(xml);
  free(other);
  char *markdown = cmark_render_commonmark(doc, 0, 0);
  char *man = cmark_render_man(doc, 0, 0);
  char *latex = cmark_render_latex(doc, 0, 0);
  CHECK(strstr(markdown, "alert-info") != NULL);
  CHECK(strstr(markdown, "[x]") != NULL);
  CHECK(strstr(man, "a b") != NULL);
  CHECK(strstr(latex, "\\textsubscript{") != NULL);
  CHECK(strstr(latex, "\\tableofcontents") != NULL);
  free(markdown);
  free(man);
  free(latex);
  cmark_node_free(streamed);
  cmark_node_free(doc);
  CHECK(cmark_node_get_task_checked(NULL) == -1);
  CHECK(strcmp(cmark_node_get_alert_type(NULL), "") == 0);
  CHECK(cmark_node_get_content_start_line(NULL) == 0);
  return 1;
}

int main(void) {
  CASE(option_isolation);
  CASE(html_policy);
  CASE(scripts_and_tasks);
  CASE(alerts_and_directives);
  CASE(toc_forms);
  CASE(links_and_positions);
  CASE(linkify_after_separators);
  CASE(tables_and_dimensions);
  CASE(math_boundaries);
  CASE(display_math_source_placement);
  CASE(dollar_rule_precedence);
  CASE(footnote_reference_order);
  CASE(footnote_continuation_indent);
  CASE(formula_labels_in_all_renderers);
  CASE(streaming_and_renderers);
  return 0;
}
