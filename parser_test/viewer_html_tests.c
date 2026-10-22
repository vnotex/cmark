#include <cmark.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_utils.h"

static cmark_html_options viewer_options(void) {
  cmark_html_options options = {0};
  options.viewer = 1;
  options.render_raw_html = 1;
  options.allow_file_links = 1;
  options.allow_svg_data_images = 1;
  return options;
}

static char *render(const char *markdown, int parse_options,
                    const cmark_html_options *options) {
  cmark_node *doc =
      cmark_parse_document(markdown, strlen(markdown), parse_options);
  char *html = cmark_render_html_with_options(doc, options);
  cmark_node_free(doc);
  return html;
}

static int contains(const char *html, const char *part) {
  if (!strstr(html, part)) {
    fprintf(stderr, "Missing HTML fragment:\n%s\nActual:\n%s\n", part, html);
    return 0;
  }
  return 1;
}

static int excludes(const char *html, const char *part) {
  if (strstr(html, part)) {
    fprintf(stderr, "Unexpected HTML fragment:\n%s\nActual:\n%s\n", part, html);
    return 0;
  }
  return 1;
}

static int occurs(const char *html, const char *part, size_t expected) {
  const char *at = html;
  size_t count = 0;
  while ((at = strstr(at, part)) != NULL) {
    ++count;
    at += strlen(part);
  }
  if (count != expected) {
    fprintf(stderr, "Expected %lu occurrences, got %lu: %s\nActual:\n%s\n",
            (unsigned long)expected, (unsigned long)count, part, html);
    return 0;
  }
  return 1;
}

static int precedes(const char *html, const char *first, const char *second) {
  const char *a = strstr(html, first);
  const char *b = strstr(html, second);
  if (!a || !b || a >= b) {
    fprintf(stderr, "Expected %s before %s\nActual:\n%s\n", first, second,
            html);
    return 0;
  }
  return 1;
}

static int generic_contract(void) {
  const char *source = "# Heading\n\n```cpp\n<&\n```\n\n"
                       "[bad](javascript:evil) [[javascript:evil|wiki]]\n\n"
                       "<script>evil()</script>\n";
  cmark_node *doc =
      cmark_parse_document(source, strlen(source), CMARK_OPT_DEFAULT);
  cmark_html_options options = {0};
  char *legacy;
  char *configured;
  int ok = 1;
  const int flags[] = {CMARK_OPT_DEFAULT, CMARK_OPT_SOURCEPOS, CMARK_OPT_UNSAFE,
                       CMARK_OPT_SOURCEPOS | CMARK_OPT_UNSAFE};
  size_t i;
  for (i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
    options.options = flags[i];
    legacy = cmark_render_html(doc, flags[i]);
    configured = cmark_render_html_with_options(doc, &options);
    ok &= strcmp(legacy, configured) == 0;
    ok &= contains(configured, "class=\"language-cpp\"");
    ok &= contains(configured,
                   "<a class=\"wikilink\" href=\"javascript:evil\">wiki</a>");
    ok &= excludes(configured, "source-line");
    ok &= excludes(configured, "vx-header-anchor");
    if (flags[i] & CMARK_OPT_UNSAFE) {
      ok &= contains(configured, "<a href=\"javascript:evil\">bad</a>");
      ok &= contains(configured, "<script>evil()</script>");
    } else {
      ok &= contains(configured, "<a href=\"\">bad</a>");
      ok &= contains(configured, "<!-- raw HTML omitted -->");
    }
    if (flags[i] & CMARK_OPT_SOURCEPOS)
      ok &= contains(configured, "data-sourcepos=\"1:1-1:9\"");
    free(configured);
    free(legacy);
  }
  cmark_node_free(doc);
  return ok && test_html("---\nkey: value\n---\n\n![alt](image.png =12x34)\n",
                         "<p><img src=\"image.png\" alt=\"alt\" /></p>\n",
                         CMARK_OPT_DEFAULT);
}

static int source_mapping_and_tasks(void) {
  const char *source = "# Heading\r\n\r\nparagraph\r\n\r\n"
                       "```cpp extra\r\ncode\r\n```\r\n\r\n"
                       "    indented\r\n\r\n"
                       "- [X] checked\r\n  - [ ] nested\r\n\r\n"
                       "| a | b |\r\n| - | - |\r\n| c | d |\r\n";
  const char *ids[] = {"heading-native"};
  cmark_html_options options = viewer_options();
  char *html;
  int ok;
  options.heading_ids = ids;
  options.heading_count = 1;
  options.options = CMARK_OPT_SOURCEPOS;
  html = render(source, CMARK_OPT_VIEWER, &options);
  ok = contains(html, "<h1 id=\"heading-native\" class=\"source-line\" "
                      "data-source-line=\"0\"") &&
       contains(html, "<p class=\"source-line\" data-source-line=\"2\"") &&
       contains(
           html,
           "<code class=\"source-line lang-cpp\" data-source-line=\"4\">") &&
       contains(html, "<pre class=\"source-line\" data-source-line=\"8\"") &&
       contains(html, "<li class=\"task-list-item enabled source-line\" "
                      "data-source-line=\"10\"") &&
       contains(html, "<li class=\"task-list-item enabled source-line\" "
                      "data-source-line=\"11\"") &&
       contains(html, "<table class=\"source-line\" data-source-line=\"13\"") &&
       contains(html, "data-sourcepos=\"1:1-1:9\"") &&
       occurs(html, "class=\"contains-task-list\"", 2) &&
       occurs(html, "class=\"task-list-item-checkbox\"", 2) &&
       occurs(html, "checked=\"\"", 1) && excludes(html, "disabled=") &&
       excludes(html, "[X]");
  free(html);
  html = render("- [x] first\n\n  second paragraph\n\n- ordinary\n",
                CMARK_OPT_VIEWER, &options);
  ok &= contains(html, "<p class=\"source-line\" data-source-line=\"0\"") &&
        occurs(html, "task-list-item-checkbox", 1) &&
        contains(html, "second paragraph</p>") &&
        contains(html, "ordinary</p>");
  free(html);
  return ok;
}

static int url_policy_and_raw_html(void) {
  const char *source =
      "[normal](javascript:evil) [[javascript:evil|wiki]] "
      "[vb](vbscript:evil) [[vbscript:evil|wvb]] "
      "[data](data:text/html;base64,PHNjcmlwdD4=) "
      "[[data:text/html;base64,PHNjcmlwdD4=|wdata]] "
      "[entity](jav&#x61;script:evil) "
      "[file](file:///tmp/note.md) [[file:///tmp/note.md|wfile]] "
      "[relative](../note.md#part) [[../note.md#part|wrelative]] "
      "[fragment](#heading) [web](https://example.com) "
      "![svg](data:image/svg+xml;base64,PHN2Zy8+) "
      "![png](data:image/png;base64,AAAA) "
      "![bad](data:text/html;base64,PHNjcmlwdD4=)\n\n"
      "<script>raw()</script>\n";
  cmark_html_options options = viewer_options();
  char *html;
  int ok;
  options.render_raw_html = 0;
  options.options = CMARK_OPT_UNSAFE;
  html = render(source, CMARK_OPT_VIEWER, &options);
  ok = contains(html, "<a href=\"\">normal</a>") &&
       contains(html, "<a class=\"wikilink\" href=\"\">wiki</a>") &&
       contains(html, "<a href=\"\">vb</a>") &&
       contains(html, "<a class=\"wikilink\" href=\"\">wvb</a>") &&
       contains(html, "<a href=\"\">data</a>") &&
       contains(html, "<a class=\"wikilink\" href=\"\">wdata</a>") &&
       contains(html, "<a href=\"\">entity</a>") &&
       occurs(html, "href=\"file:///tmp/note.md\"", 2) &&
       occurs(html, "href=\"../note.md#part\"", 2) &&
       contains(html, "href=\"#heading\"") &&
       contains(html, "href=\"https://example.com\"") &&
       contains(html, "src=\"data:image/svg+xml;base64,PHN2Zy8+\"") &&
       contains(html, "src=\"data:image/png;base64,AAAA\"") &&
       contains(html, "<img src=\"\" alt=\"bad\"") &&
       contains(html, "<!-- raw HTML omitted -->") &&
       excludes(html, "<script>");
  free(html);
  options.render_raw_html = 1;
  options.allow_file_links = 0;
  options.allow_svg_data_images = 0;
  html = render(source, CMARK_OPT_VIEWER, &options);
  ok &= contains(html, "<script>raw()</script>") &&
        excludes(html, "href=\"file:") &&
        excludes(html, "src=\"data:image/svg+xml") &&
        excludes(html, "href=\"javascript:") &&
        excludes(html, "href=\"vbscript:");
  free(html);
  return ok;
}

static int url_controls_and_case(void) {
  const char *targets[] = {" JaVaScRiPt:evil",     "java\tscript:evil",
                           "java\nscript:evil",    "\r\nVBSCRIPT:evil",
                           " DATA:text/html,evil", "data:image/svg+xmlx;evil"};
  const cmark_node_type types[] = {CMARK_NODE_LINK, CMARK_NODE_WIKILINK};
  cmark_html_options options = viewer_options();
  size_t i, j;
  int ok = 1;
  for (i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
    for (j = 0; j < sizeof(targets) / sizeof(targets[0]); ++j) {
      cmark_node *doc = cmark_node_new(CMARK_NODE_DOCUMENT);
      cmark_node *paragraph = cmark_node_new(CMARK_NODE_PARAGRAPH);
      cmark_node *link = cmark_node_new(types[i]);
      cmark_node *label = cmark_node_new(CMARK_NODE_TEXT);
      char *html;
      ok &= cmark_node_set_url(link, targets[j]);
      ok &= cmark_node_set_literal(label, "label");
      ok &= cmark_node_append_child(link, label);
      ok &= cmark_node_append_child(paragraph, link);
      ok &= cmark_node_append_child(doc, paragraph);
      html = cmark_render_html_with_options(doc, &options);
      ok &= contains(html, "href=\"\">label</a>");
      free(html);
      cmark_node_free(doc);
    }
  }
  return ok;
}

static int headings_toc_and_read_only_rendering(void) {
  const char *source = "[toc]\n\n# A $x$ &amp; y\n\n### Child\n\n## Peer\n";
  const char *ids[] = {"borrowed-a", "borrowed-child", "borrowed-peer"};
  const char *replacement[] = {"second-a", "second-child", "second-peer"};
  cmark_html_options options = viewer_options();
  cmark_node *doc =
      cmark_parse_document(source, strlen(source), CMARK_OPT_VIEWER);
  char *before = cmark_render_xml(doc, CMARK_OPT_SOURCEPOS);
  char *html;
  char *again;
  char *after;
  int ok;
  options.heading_ids = ids;
  options.heading_count = 3;
  html = cmark_render_html_with_options(doc, &options);
  ok = contains(html, "<nav class=\"vx-table-of-contents\"><ol>") &&
       contains(html, "<a href=\"#borrowed-a\">A x &amp; y</a>") &&
       contains(html, "<ol><li><a href=\"#borrowed-child\">Child</a></li>"
                      "<li><a href=\"#borrowed-peer\">Peer</a></li></ol>") &&
       contains(html, "<h1 id=\"borrowed-a\"") &&
       contains(html, "<eq class=\"tex-to-render\">$x$</eq>") &&
       contains(html, "<a class=\"vx-header-anchor\" href=\"#borrowed-a\" "
                      "vx-data-anchor-icon=\"\xc2\xb6\"></a>") &&
       excludes(html, "href=\"#a-x");
  again = cmark_render_html_with_options(doc, &options);
  ok &= strcmp(html, again) == 0;
  free(again);
  options.heading_ids = replacement;
  again = cmark_render_html_with_options(doc, &options);
  ok &= contains(again, "href=\"#second-a\"") && excludes(again, "borrowed-a");
  after = cmark_render_xml(doc, CMARK_OPT_SOURCEPOS);
  ok &= strcmp(before, after) == 0;
  free(before);
  free(after);
  free(html);
  free(again);
  cmark_node_free(doc);
  return ok;
}

static int toc_options_and_escaping(void) {
  const char *source = "$<toc{\"level\":[2,3],\"listType\":\"ul\","
                       "\"containerId\":\"toc-id\",\"listClass\":\"list\","
                       "\"itemClass\":\"entry\",\"linkClass\":\"l\\u0069nk\","
                       "\"containerClass\":\"quote\\\"<>&\","
                       "\"unused\":[true,false,null,1]}>\n\n"
                       "# Root\n\n## Child\n\n### Grandchild\n";
  const char *ids[] = {"root", "child", "grandchild"};
  cmark_html_options options = viewer_options();
  char *html;
  int ok;
  options.heading_ids = ids;
  options.heading_count = 3;
  html = render(source, CMARK_OPT_VIEWER, &options);
  ok = contains(html,
                "<nav id=\"toc-id\" class=\"quote&quot;&lt;&gt;&amp;\">") &&
       contains(html, "<ul class=\"list\"><li class=\"entry\">"
                      "<a class=\"link\" href=\"#child\">Child</a>") &&
       contains(html, "href=\"#grandchild\">Grandchild</a>") &&
       excludes(html, "href=\"#root\">Root</a>");
  free(html);
  return ok;
}

static int frontmatter_figures_and_plaintext(void) {
  const char *source =
      "---\ntitle: <script>\" & value\n---\n\n"
      "![A *caption* $x < y$](photo.png \"title\" =120x80)\n\n"
      "before ![A $x < y$ &amp; \"quote\"](icon.png =50%x25%) after\n\n"
      "[![Linked **caption**](linked.png =x42)](target.md)\n";
  cmark_html_options options = viewer_options();
  char *html = render(source, CMARK_OPT_VIEWER, &options);
  int ok =
      contains(html,
               "<details "
               "class=\"vx-frontmatter\"><summary>Metadata</summary><pre>") &&
      contains(html, "title: &lt;script&gt;&quot; &amp; value") &&
      excludes(html, "<script>") &&
      contains(html, "<figure><img src=\"photo.png\" alt=\"\" title=\"title\" "
                     "width=\"120\" height=\"80\" />") &&
      contains(html, "<figcaption>A <em>caption</em> <eq "
                     "class=\"tex-to-render\">$x &lt; y$</eq></figcaption>") &&
      contains(html, "alt=\"A x &lt; y &amp; &quot;quote&quot;\" width=\"50%\" "
                     "height=\"25%\"") &&
      contains(html, "<figure><a href=\"target.md\"><img src=\"linked.png\" "
                     "alt=\"\" height=\"42\" /></a>") &&
      contains(html,
               "<figcaption>Linked <strong>caption</strong></figcaption>");
  free(html);
  return ok;
}

static int alerts_math_and_new_generic_nodes(void) {
  const char *source = "::: alert-warning\nH~2~O x^2^ ==marked==\n:::\n\n"
                       "inline $x < y$ and $$z$$.\n\n"
                       "$$a < b$$ (A&1)\n\n"
                       "\\begin{align}\nx &= y\n\\end{align}\n";
  cmark_html_options options = viewer_options();
  char *html = render(source, CMARK_OPT_VIEWER, &options);
  int ok =
      contains(html, "<div class=\"vx-alert alert-warning\" role=\"alert\">") &&
      contains(html, "H<sub>2</sub>O x<sup>2</sup> <mark>marked</mark>") &&
      contains(html, "<eq class=\"tex-to-render\">$x &lt; y$</eq>") &&
      contains(html,
               "<section><eqn class=\"tex-to-render\">$$z$$</eqn></section>") &&
      contains(html, "<section class=\"eqno\"><eqn class=\"tex-to-render\">") &&
      contains(html, "<span>(A&amp;1)</span>") &&
      contains(html, "\\begin{align}") && contains(html, "\\end{align}") &&
      contains(html, "x &amp;= y");
  free(html);
  options.viewer = 0;
  html = render("::: alert-info\nH~2~O x^2^\n:::\n\n- [x] checked\n",
                CMARK_OPT_VIEWER, &options);
  ok &= contains(html, "<div class=\"alert alert-info\" role=\"alert\">") &&
        contains(html, "H<sub>2</sub>O x<sup>2</sup>") &&
        contains(html, "disabled=\"\" checked=\"\"") &&
        excludes(html, "source-line");
  free(html);
  return ok;
}

static int footnote_identities_and_order(void) {
  const char *source = "B[^b] then ^[inline *note*] and A[^a], B again[^b].\n\n"
                       "[^a]: Alpha\n\n[^b]: Beta\n\n[^unused]: Hidden\n";
  cmark_html_options options = viewer_options();
  cmark_node *doc =
      cmark_parse_document(source, strlen(source), CMARK_OPT_VIEWER);
  char *html = cmark_render_html_with_options(doc, &options);
  char *again = cmark_render_html_with_options(doc, &options);
  int ok = contains(html, "<sup class=\"footnote-ref\"><a href=\"#fn1\" "
                          "id=\"fnref1\">[1]</a></sup>") &&
           contains(html, "href=\"#fn1\" id=\"fnref1:1\">[1:1]</a>") &&
           contains(html, "href=\"#fn2\" id=\"fnref2\">[2]</a>") &&
           contains(html, "href=\"#fn3\" id=\"fnref3\">[3]</a>") &&
           contains(html, "<hr class=\"footnotes-sep\" />") &&
           contains(html, "<ol class=\"footnotes-list\">") &&
           occurs(html, "id=\"fn1\"", 1) && occurs(html, "id=\"fn2\"", 1) &&
           occurs(html, "id=\"fn3\"", 1) &&
           occurs(html, "class=\"footnote-backref\"", 4) &&
           contains(html, "href=\"#fnref1:1\" class=\"footnote-backref\"") &&
           contains(html, "inline <em>note</em>") && excludes(html, "Hidden") &&
           precedes(html, "<li id=\"fn1\"", "<li id=\"fn2\"") &&
           precedes(html, "<li id=\"fn2\"", "<li id=\"fn3\"") &&
           strcmp(html, again) == 0;
  free(html);
  free(again);
  cmark_node_free(doc);
  return ok;
}

static int breaks_html_text_and_empty(void) {
  cmark_html_options options = viewer_options();
  char *html;
  int ok;
  options.options = CMARK_OPT_HARDBREAKS;
  options.render_raw_html = 0;
  html = render("a\nb\n\n<b>&amp;</b>\n",
                CMARK_OPT_VIEWER | CMARK_OPT_HTML_AS_TEXT, &options);
  ok = contains(html, "a<br />\nb") &&
       contains(html, "&lt;b&gt;&amp;&lt;/b&gt;") &&
       excludes(html, "<!-- raw HTML omitted -->");
  free(html);
  html = render("", CMARK_OPT_VIEWER, &options);
  ok &= strcmp(html, "") == 0;
  free(html);
  return ok;
}

static int heading_ids_in_deferred_footnotes(void) {
  const char *source = "[^note]: # Same\n\n# Same\n\nText[^note]\n";
  /* IDs are assigned in rendered order, then supplied in source order. */
  const char *ids[] = {"same-1", "same"};
  cmark_html_options options = viewer_options();
  cmark_node *doc =
      cmark_parse_document(source, strlen(source), CMARK_OPT_VIEWER);
  cmark_node *definition = cmark_node_first_child(doc);
  char *html;
  char *relocated;
  int ok;
  options.heading_ids = ids;
  options.heading_count = 2;
  html = cmark_render_html_with_options(doc, &options);
  ok = contains(html, "<h1 id=\"same\"") &&
       contains(html, "<h1 id=\"same-1\"") &&
       precedes(html, "<h1 id=\"same\"", "<h1 id=\"same-1\"");
  while (definition &&
         cmark_node_get_type(definition) != CMARK_NODE_FOOTNOTE_DEFINITION)
    definition = cmark_node_next(definition);
  if (!definition) {
    free(html);
    cmark_node_free(doc);
    return 0;
  }
  cmark_node_unlink(definition);
  ok &= cmark_node_append_child(doc, definition);
  relocated = cmark_render_html_with_options(doc, &options);
  ok &= strcmp(html, relocated) == 0;
  free(relocated);
  free(html);
  cmark_node_free(doc);
  return ok;
}

static int optional_heading_id_tables(void) {
  const char *source = "# First\n\n## Second\n\nUse[^note]\n\n"
                       "[^note]: Footnote body\n";
  const char *ids[] = {"first"};
  cmark_html_options options = viewer_options();
  char *without_ids;
  char *html;
  int ok;

  options.heading_count = 2;
  without_ids = render(source, CMARK_OPT_VIEWER, &options);
  ok = contains(without_ids,
                "<h1 class=\"source-line\" data-source-line=\"0\">First</h1>") &&
       contains(without_ids,
                "<h2 class=\"source-line\" data-source-line=\"2\">Second</h2>") &&
       excludes(without_ids, "vx-header-anchor") &&
       contains(without_ids, "href=\"#fn1\"") &&
       contains(without_ids, "Footnote body");

  options.heading_ids = ids;
  options.heading_count = 0;
  html = render(source, CMARK_OPT_VIEWER, &options);
  ok &= strcmp(html, without_ids) == 0;
  free(html);
  free(without_ids);

  options.heading_count = 1;
  html = render(source, CMARK_OPT_VIEWER, &options);
  ok &= contains(html, "<h1 id=\"first\"") &&
        contains(html, "class=\"vx-header-anchor\" href=\"#first\"") &&
        contains(html,
                 "<h2 class=\"source-line\" data-source-line=\"2\">Second</h2>") &&
        contains(html, "href=\"#fn1\"") && contains(html, "Footnote body");
  free(html);

  html = render("", CMARK_OPT_VIEWER, &options);
  ok &= strcmp(html, "") == 0;
  free(html);
  return ok;
}

static int heading_attribute_escaping_and_empty_id(void) {
  const char *ids[] = {"\"<&", ""};
  cmark_html_options options = viewer_options();
  char *html;
  int ok;
  options.heading_ids = ids;
  options.heading_count = 2;
  html = render("[toc]\n\n# [[target.md|First]]\n\n# Empty\n", CMARK_OPT_VIEWER,
                &options);
  ok = contains(html, "<h1 id=\"&quot;&lt;&amp;\"") &&
       contains(html, "<h1 id=\"\"") &&
       contains(html, "href=\"#\">Empty</a>") &&
       contains(html, "<a class=\"wikilink\" href=\"target.md\">First</a>") &&
       occurs(html, "vx-data-anchor-icon=", 2) && excludes(html, "id=\"\"<&");
  free(html);
  return ok;
}

static int task_image_remains_an_inline_label(void) {
  cmark_html_options options = viewer_options();
  char *html =
      render("- [x] ![task image](task.png)\n", CMARK_OPT_VIEWER, &options);
  int ok = contains(html, "task-list-item-checkbox") &&
           contains(html, "alt=\"task image\"") && excludes(html, "<figure>") &&
           excludes(html, "<figcaption>");
  free(html);
  return ok;
}

static int visible_lines_after_reference_definitions(void) {
  cmark_html_options options = viewer_options();
  char *html = render("[target]: target.md\nParagraph [target]\n",
                      CMARK_OPT_VIEWER, &options);
  int ok = contains(html, "<p class=\"source-line\" data-source-line=\"1\">") &&
           contains(html, "href=\"target.md\">target</a>");
  free(html);
  return ok;
}

static int display_math_keeps_inline_source_nodes(void) {
  const struct {
    const char *source;
    const char *fragment;
    int standalone;
  } cases[] = {
      {"$$x^2$$\n",
       "<section><eqn class=\"tex-to-render\">$$x^2$$</eqn></section>", 1},
      {"Inline $$x^2$$ in a paragraph.\n",
       "Inline <section><eqn class=\"tex-to-render\">$$x^2$$</eqn></section> "
       "in a paragraph.",
       0},
      {"$$x^2$$ (tag)\n",
       "<section class=\"eqno\"><eqn "
       "class=\"tex-to-render\">$$x^2$$</eqn><span>(tag)</span></section>",
       1},
      {"- $$x^2$$\n",
       "<section><eqn class=\"tex-to-render\">$$x^2$$</eqn></section>", 1},
      {"- $$x^2$$ (tag)\n",
       "<section class=\"eqno\"><eqn "
       "class=\"tex-to-render\">$$x^2$$</eqn><span>(tag)</span></section>",
       1},
      {"| Math |\n| --- |\n| $$x^2$$ |\n",
       "<td><section><eqn class=\"tex-to-render\">$$x^2$$</eqn></section>", 0},
      {"# $$x^2$$ (tag)\n",
       "<section><eqn class=\"tex-to-render\">$$x^2$$</eqn></section> (tag)",
       0},
      {"Inline $$x^2$$ (tag)\n",
       "Inline <section><eqn class=\"tex-to-render\">$$x^2$$</eqn></section> "
       "(tag)",
       0},
      {"- [x] $$x^2$$\n",
       "checked=\"\" /> <section><eqn "
       "class=\"tex-to-render\">$$x^2$$</eqn></section>",
       0}};
  cmark_html_options options = viewer_options();
  size_t i;
  int ok = 1;
  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    cmark_node *doc = cmark_parse_document(
        cases[i].source, strlen(cases[i].source), CMARK_OPT_VIEWER);
    cmark_iter *iter = cmark_iter_new(doc);
    cmark_event_type event;
    int inline_count = 0;
    char *before = cmark_render_xml(doc, CMARK_OPT_SOURCEPOS);
    char *html;
    char *after;
    while ((event = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
      cmark_node *node = cmark_iter_get_node(iter);
      if (event != CMARK_EVENT_ENTER)
        continue;
      ok &= cmark_node_get_type(node) != CMARK_NODE_FORMULA_BLOCK;
      if (cmark_node_get_type(node) == CMARK_NODE_FORMULA_INLINE) {
        ++inline_count;
        ok &= cmark_node_get_formula_display(node) != 0;
        ok &= strcmp(cmark_node_get_literal(node), "x^2") == 0;
        ok &= cmark_node_get_start_line(node) == cmark_node_get_end_line(node);
        ok &= cmark_node_get_end_column(node) -
                  cmark_node_get_start_column(node) ==
              2;
      }
    }
    cmark_iter_free(iter);
    ok &= inline_count == 1;
    html = cmark_render_html_with_options(doc, &options);
    after = cmark_render_xml(doc, CMARK_OPT_SOURCEPOS);
    ok &= contains(html, cases[i].fragment) && strcmp(before, after) == 0;
    if (cases[i].standalone)
      ok &= excludes(html, "<p");
    free(before);
    free(after);
    free(html);
    cmark_node_free(doc);
  }
  return ok && test_html("$$x^2$$\n",
                         "<p><eq class=\"tex-to-render\">$$x^2$$</eq></p>\n",
                         CMARK_OPT_DEFAULT);
}

static int display_math_paragraph_boundaries(void) {
  cmark_html_options options = viewer_options();
  char *html = render("$$a$$\n$$b$$\ntext\n", CMARK_OPT_VIEWER, &options);
  int ok =
      contains(html,
               "<section><eqn class=\"tex-to-render\">$$a$$</eqn></section>") &&
      contains(html,
               "<section><eqn class=\"tex-to-render\">$$b$$</eqn></section>") &&
      contains(html,
               "<p class=\"source-line\" data-source-line=\"2\">text</p>") &&
      occurs(html, "<p", 1) && excludes(html, "<br");
  free(html);
  html = render("text\n$$x$$\n", CMARK_OPT_VIEWER, &options);
  ok &= contains(
      html, "<p class=\"source-line\" data-source-line=\"0\">text\n"
            "<section><eqn class=\"tex-to-render\">$$x$$</eqn></section></p>");
  free(html);
  html = render("$a\\$b$\n", CMARK_OPT_VIEWER, &options);
  ok &= contains(html, "<eq class=\"tex-to-render\">$a\\$b$</eq>") &&
        excludes(html, "a\\\\$b");
  free(html);
  return ok;
}

static int toc_matches_visible_heading_order(void) {
  const char *source = "[^unused]: # Hidden heading\n\n"
                       "[^b]: # Same\n\n[^a]: # Same\n\n"
                       "[toc]\n\n# Same\n\nUse[^a] then[^b]\n";
  const char *ids[] = {"", "same-2", "same-1", "same"};
  cmark_html_options options = viewer_options();
  cmark_node *doc =
      cmark_parse_document(source, strlen(source), CMARK_OPT_VIEWER);
  char *before = cmark_render_xml(doc, CMARK_OPT_SOURCEPOS);
  char *html;
  char *after;
  int ok;
  options.heading_ids = ids;
  options.heading_count = 4;
  html = cmark_render_html_with_options(doc, &options);
  after = cmark_render_xml(doc, CMARK_OPT_SOURCEPOS);
  ok = contains(html, "<nav class=\"vx-table-of-contents\"><ol>"
                      "<li><a href=\"#same\">Same</a></li>"
                      "<li><a href=\"#same-1\">Same</a></li>"
                      "<li><a href=\"#same-2\">Same</a></li></ol></nav>") &&
       excludes(html, "Hidden heading") &&
       precedes(html, "<h1 id=\"same\"", "<h1 id=\"same-1\"") &&
       precedes(html, "<h1 id=\"same-1\"", "<h1 id=\"same-2\"") &&
       strcmp(before, after) == 0;
  free(before);
  free(after);
  free(html);
  cmark_node_free(doc);
  return ok;
}

int main(void) {
  CASE(generic_contract);
  CASE(source_mapping_and_tasks);
  CASE(url_policy_and_raw_html);
  CASE(url_controls_and_case);
  CASE(headings_toc_and_read_only_rendering);
  CASE(toc_options_and_escaping);
  CASE(frontmatter_figures_and_plaintext);
  CASE(alerts_math_and_new_generic_nodes);
  CASE(footnote_identities_and_order);
  CASE(breaks_html_text_and_empty);
  CASE(heading_ids_in_deferred_footnotes);
  CASE(optional_heading_id_tables);
  CASE(toc_matches_visible_heading_order);
  CASE(heading_attribute_escaping_and_empty_id);
  CASE(task_image_remains_an_inline_label);
  CASE(visible_lines_after_reference_definitions);
  CASE(display_math_keeps_inline_source_nodes);
  CASE(display_math_paragraph_boundaries);
  return 0;
}
