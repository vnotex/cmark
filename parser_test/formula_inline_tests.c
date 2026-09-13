#include <cmark.h>

#include "test_utils.h"

int test_formula_inline_simple() {
  return test_xml("$E=mc^2$",
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE document SYSTEM \"CommonMark.dtd\">\n"
      "<document xmlns=\"http://commonmark.org/xml/1.0\">\n"
      "  <paragraph>\n"
      "    <formula_inline xml:space=\"preserve\">E=mc^2</formula_inline>\n"
      "  </paragraph>\n"
      "</document>\n",
      CMARK_OPT_DEFAULT);
}

int test_formula_inline_multiple() {
  return test_xml("$a+b$ and $c+d$",
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE document SYSTEM \"CommonMark.dtd\">\n"
      "<document xmlns=\"http://commonmark.org/xml/1.0\">\n"
      "  <paragraph>\n"
      "    <formula_inline xml:space=\"preserve\">a+b</formula_inline>\n"
      "    <text xml:space=\"preserve\"> and </text>\n"
      "    <formula_inline xml:space=\"preserve\">c+d</formula_inline>\n"
      "  </paragraph>\n"
      "</document>\n",
      CMARK_OPT_DEFAULT);
}

int test_formula_inline_with_escape() {
  return test_xml("$a\\$b$",
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE document SYSTEM \"CommonMark.dtd\">\n"
      "<document xmlns=\"http://commonmark.org/xml/1.0\">\n"
      "  <paragraph>\n"
      "    <formula_inline xml:space=\"preserve\">a$b</formula_inline>\n"
      "  </paragraph>\n"
      "</document>\n",
      CMARK_OPT_DEFAULT);
}

int test_formula_inline_not_closed() {
  return test_xml("$formula",
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE document SYSTEM \"CommonMark.dtd\">\n"
      "<document xmlns=\"http://commonmark.org/xml/1.0\">\n"
      "  <paragraph>\n"
      "    <text xml:space=\"preserve\">$formula</text>\n"
      "  </paragraph>\n"
      "</document>\n",
      CMARK_OPT_DEFAULT);
}

int test_formula_inline_empty() {
  return test_xml("abc $$",
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE document SYSTEM \"CommonMark.dtd\">\n"
      "<document xmlns=\"http://commonmark.org/xml/1.0\">\n"
      "  <paragraph>\n"
      "    <text xml:space=\"preserve\">abc $$</text>\n"
      "  </paragraph>\n"
      "</document>\n",
      CMARK_OPT_DEFAULT);
}

int test_formula_inline_simple_html() {
  return test_html("$E=mc^2$",
      "<p><eq class=\"tex-to-render\">$E=mc^2$</eq></p>\n",
      CMARK_OPT_DEFAULT);
}

int test_formula_inline_multiple_html() {
  return test_html("$a+b$ and $c+d$",
      "<p><eq class=\"tex-to-render\">$a+b$</eq> and "
      "<eq class=\"tex-to-render\">$c+d$</eq></p>\n",
      CMARK_OPT_DEFAULT);
}

int test_formula_inline_special_chars_html() {
  return test_html("$a < b$",
      "<p><eq class=\"tex-to-render\">$a &lt; b$</eq></p>\n",
      CMARK_OPT_DEFAULT);
}

int test_formula_inline_escaped_dollar_html() {
  return test_html("$a\\$b$ and $a" "\\\\" "\\$" "b$",
      "<p><eq class=\"tex-to-render\">$a\\$b$</eq> and "
      "<eq class=\"tex-to-render\">$a" "\\\\" "\\$" "b$</eq></p>\n",
      CMARK_OPT_DEFAULT);
}

int test_formula_inline_html_metacharacters() {
  return test_html("$a < b & c > d$ and $\\text{</eq><img>}$ and $&lt;$",
      "<p><eq class=\"tex-to-render\">$a &lt; b &amp; c &gt; d$</eq> and "
      "<eq class=\"tex-to-render\">$\\text{&lt;/eq&gt;&lt;img&gt;}$</eq> and "
      "<eq class=\"tex-to-render\">$&amp;lt;$</eq></p>\n",
      CMARK_OPT_DEFAULT);
}

int test_formula_inline_literal_contexts_html() {
  return test_html("\\$p_1\\$ and `$p_1$`\n\n$$p_1$$\n\n$formula",
      "<p>$p_1$ and <code>$p_1$</code></p>\n"
      "<p>$$p_1$$</p>\n"
      "<p>$formula</p>\n",
      CMARK_OPT_DEFAULT);
}

int main() {
  CASE(test_formula_inline_simple);
  CASE(test_formula_inline_multiple);
  CASE(test_formula_inline_with_escape);
  CASE(test_formula_inline_not_closed);
  CASE(test_formula_inline_empty);
  CASE(test_formula_inline_simple_html);
  CASE(test_formula_inline_multiple_html);
  CASE(test_formula_inline_special_chars_html);
  CASE(test_formula_inline_escaped_dollar_html);
  CASE(test_formula_inline_html_metacharacters);
  CASE(test_formula_inline_literal_contexts_html);
  return 0;
}
