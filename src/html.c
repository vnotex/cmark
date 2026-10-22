#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmark_ctype.h"
#include "cmark.h"
#include "node.h"
#include "buffer.h"
#include "houdini.h"
#include "scanners.h"
#include "utf8.h"

#define BUFFER_SIZE 100

// Functions to convert cmark_nodes to HTML strings.

static void escape_html(cmark_strbuf *dest, const unsigned char *source,
                        bufsize_t length) {
  houdini_escape_html(dest, source, length, 0);
}

static void escape_inline_formula(cmark_strbuf *dest,
                                  const unsigned char *source,
                                  bufsize_t length) {
  bufsize_t i, start = 0;

  if (length == 0)
    return;

  for (i = 0; i < length; ++i) {
    if (source[i] == '$') {
      escape_html(dest, source + start, i - start);
      // Restore the backslash removed by handle_dollar().
      cmark_strbuf_puts(dest, "\\$");
      start = i + 1;
    }
  }
  escape_html(dest, source + start, length - start);
}

static inline void cr(cmark_strbuf *html) {
  if (html->size && html->ptr[html->size - 1] != '\n')
    cmark_strbuf_putc(html, '\n');
}

struct footnote_entry {
  cmark_node *node;
  unsigned int references;
  unsigned int rendered;
};

struct heading_entry {
  cmark_node *node;
  const char *id;
  size_t traversal_order;
};

struct render_state {
  cmark_strbuf *html;
  cmark_node *plain;
  bool plain_heading;
  cmark_strbuf *footnotes_html;
  bool in_footnote;
  const cmark_html_options *config;
  cmark_node *root;
  cmark_node *active_footnote;
  cmark_node *display_math_paragraph;
  cmark_node *paragraph_remainder;
  struct footnote_entry *footnotes;
  size_t footnote_capacity;
  struct heading_entry *headings;
  size_t heading_count;
};

static void S_render_tree(cmark_node *root, struct render_state *state);
static void S_render_children(cmark_node *node, struct render_state *state);

static cmark_strbuf *S_html(struct render_state *state) {
  return state->in_footnote ? state->footnotes_html : state->html;
}

/* URL permissions are independent of raw HTML and CMARK_OPT_UNSAFE. */
static bool S_prefix(const unsigned char *text, const char *prefix) {
  while (*prefix) {
    unsigned char ch = *text++;
    if (ch >= 'A' && ch <= 'Z')
      ch += 'a' - 'A';
    if (ch != (unsigned char)*prefix++)
      return false;
  }
  return true;
}

static bool S_viewer_url(const unsigned char *url,
                         const cmark_html_options *config) {
  const unsigned char *p;
  while (*url && *url <= ' ')
    ++url;
  /* Browsers discard embedded ASCII tabs/newlines before parsing a scheme. */
  for (p = url; *p; ++p) {
    if (*p < ' ' || *p == 0x7f)
      return false;
  }
  if (S_prefix(url, "javascript:") || S_prefix(url, "vbscript:"))
    return false;
  if (S_prefix(url, "file:"))
    return config->allow_file_links != 0;
  if (S_prefix(url, "data:")) {
    if (config->allow_svg_data_images && S_prefix(url, "data:image/svg+xml") &&
        (url[18] == ';' || url[18] == ','))
      return true;
    return S_prefix(url, "data:image/gif;") ||
           S_prefix(url, "data:image/png;") ||
           S_prefix(url, "data:image/jpeg;") ||
           S_prefix(url, "data:image/webp;");
  }
  return true;
}

static bool S_url_allowed(cmark_node *node, struct render_state *state) {
  const unsigned char *url = node->as.link.url;
  if (!url)
    return false;
  if (state->config->viewer)
    return S_viewer_url(url, state->config);
  return (state->config->options & CMARK_OPT_UNSAFE) ||
         !_scan_dangerous_url(url);
}

static void S_source_line(cmark_node *node, cmark_strbuf *html,
                          const char *classes) {
  char buffer[BUFFER_SIZE];
  int line = cmark_node_get_start_line(node);
  if ((node->type == CMARK_NODE_PARAGRAPH ||
       node->type == CMARK_NODE_HEADING) &&
      node->content_start_line > 0)
    line = node->content_start_line;
  if (classes || line > 0) {
    cmark_strbuf_puts(html, " class=\"");
    if (classes)
      cmark_strbuf_puts(html, classes);
    if (line > 0) {
      if (classes && *classes)
        cmark_strbuf_putc(html, ' ');
      cmark_strbuf_puts(html, "source-line");
    }
    cmark_strbuf_putc(html, '"');
  }
  if (line > 0) {
    snprintf(buffer, sizeof(buffer), " data-source-line=\"%d\"", line - 1);
    cmark_strbuf_puts(html, buffer);
  }
}

static int S_heading_compare(const void *a, const void *b) {
  uintptr_t left = (uintptr_t)((const struct heading_entry *)a)->node;
  uintptr_t right = (uintptr_t)((const struct heading_entry *)b)->node;
  return (left > right) - (left < right);
}

static int S_heading_source_compare(const void *a, const void *b) {
  const struct heading_entry *left = (const struct heading_entry *)a;
  const struct heading_entry *right = (const struct heading_entry *)b;
  if (left->node->start_line != right->node->start_line)
    return (left->node->start_line > right->node->start_line) ? 1 : -1;
  if (left->node->start_column != right->node->start_column)
    return (left->node->start_column > right->node->start_column) ? 1 : -1;
  return (left->traversal_order > right->traversal_order) -
         (left->traversal_order < right->traversal_order);
}

static const char *S_heading_id(struct render_state *state, cmark_node *node) {
  struct heading_entry key = {node, NULL, 0};
  struct heading_entry *entry;
  if (!state->heading_count)
    return NULL;
  entry = (struct heading_entry *)bsearch(
      &key, state->headings, state->heading_count, sizeof(*state->headings),
      S_heading_compare);
  return entry ? entry->id : NULL;
}

static void S_heading_link(cmark_strbuf *html, const char *id) {
  cmark_strbuf_puts(html, "#");
  houdini_escape_href(html, (const unsigned char *)id, (bufsize_t)strlen(id));
}

static cmark_node *S_figure_image(cmark_node *paragraph) {
  cmark_node *child = paragraph->first_child;
  if (!child || child->next ||
      (paragraph->parent && paragraph == paragraph->parent->first_child &&
       cmark_node_get_task_checked(paragraph->parent) >= 0))
    return NULL;
  if (child->type == CMARK_NODE_LINK || child->type == CMARK_NODE_WIKILINK) {
    child = child->first_child;
    if (!child || child->next)
      return NULL;
  }
  return child->type == CMARK_NODE_IMAGE ? child : NULL;
}

static bool S_is_figure_image(cmark_node *image) {
  cmark_node *paragraph = image->parent;
  if (paragraph && (paragraph->type == CMARK_NODE_LINK ||
                    paragraph->type == CMARK_NODE_WIKILINK))
    paragraph = paragraph->parent;
  return paragraph && paragraph->type == CMARK_NODE_PARAGRAPH &&
         S_figure_image(paragraph) == image;
}

/* Source-inline display formulas can occupy complete leading paragraph lines.
 * Preserve their AST placement while giving those lines standalone HTML. */
static cmark_node *S_display_math_remainder(cmark_node *paragraph) {
  cmark_node *node = paragraph->first_child;
  while (node && node->type == CMARK_NODE_FORMULA_INLINE &&
         cmark_node_get_formula_display(node) &&
         node->start_line == node->end_line) {
    cmark_node *next = node->next;
    if (next && next->type == CMARK_NODE_TEXT) {
      bufsize_t i;
      for (i = 0;
           i < next->len && (next->data[i] == ' ' || next->data[i] == '\t');
           ++i) {
      }
      if (i != next->len)
        break;
      next = next->next;
    }
    if (!next)
      return NULL;
    if (next->type != CMARK_NODE_SOFTBREAK &&
        next->type != CMARK_NODE_LINEBREAK)
      break;
    node = next->next;
  }
  return node;
}

static void S_task_checkbox(cmark_strbuf *html, int checked, bool viewer) {
  cmark_strbuf_puts(
      html, "<input class=\"task-list-item-checkbox\" type=\"checkbox\"");
  if (!viewer)
    cmark_strbuf_puts(html, " disabled=\"\"");
  if (checked)
    cmark_strbuf_puts(html, " checked=\"\"");
  cmark_strbuf_puts(html, " /> ");
}

static bool S_task_list(cmark_node *list) {
  cmark_node *child;
  for (child = list->first_child; child; child = child->next) {
    if (cmark_node_get_task_checked(child) >= 0)
      return true;
  }
  return false;
}

static struct footnote_entry *S_footnote(struct render_state *state,
                                         int number) {
  size_t capacity;
  if (number <= 0)
    return NULL;
  if ((size_t)number >= state->footnote_capacity) {
    capacity = state->footnote_capacity ? state->footnote_capacity : 8;
    while (capacity <= (size_t)number)
      capacity *= 2;
    state->footnotes = (struct footnote_entry *)state->root->mem->realloc(
        state->footnotes, capacity * sizeof(*state->footnotes));
    memset(state->footnotes + state->footnote_capacity, 0,
           (capacity - state->footnote_capacity) * sizeof(*state->footnotes));
    state->footnote_capacity = capacity;
  }
  return &state->footnotes[number];
}

/* Only per-call metadata is retained; the published tree is never modified. */
static void S_collect_metadata(struct render_state *state) {
  cmark_iter *iter = cmark_iter_new(state->root);
  cmark_event_type ev;
  cmark_node *enclosing_definition = NULL;
  size_t heading_capacity = 0;
  size_t i;
  if (state->config->heading_ids && state->config->heading_count) {
    heading_capacity = state->config->heading_count;
    state->headings = (struct heading_entry *)state->root->mem->calloc(
        heading_capacity, sizeof(*state->headings));
  }
  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *node = cmark_iter_get_node(iter);
    struct footnote_entry *entry;
    if (node->type == CMARK_NODE_IMAGE && !S_is_figure_image(node)) {
      cmark_iter_reset(iter, node, CMARK_EVENT_EXIT);
      continue;
    }
    if (state->headings && node->type == CMARK_NODE_HEADING &&
        ev == CMARK_EVENT_ENTER) {
      if (state->heading_count == heading_capacity) {
        heading_capacity *= 2;
        state->headings = (struct heading_entry *)state->root->mem->realloc(
            state->headings, heading_capacity * sizeof(*state->headings));
      }
      state->headings[state->heading_count].node = node;
      state->headings[state->heading_count].id = NULL;
      state->headings[state->heading_count].traversal_order =
          state->heading_count;
      ++state->heading_count;
    }
    if (node->type == CMARK_NODE_FOOTNOTE_DEFINITION) {
      if (ev == CMARK_EVENT_ENTER) {
        enclosing_definition = node;
        entry = S_footnote(state, node->as.footnote_def.number);
        if (entry)
          entry->node = node;
      } else {
        enclosing_definition = node->parent;
        while (enclosing_definition &&
               enclosing_definition->type != CMARK_NODE_FOOTNOTE_DEFINITION)
          enclosing_definition = enclosing_definition->parent;
      }
    } else if ((!enclosing_definition ||
                enclosing_definition->as.footnote_def.number > 0) &&
               ev == CMARK_EVENT_ENTER &&
               (node->type == CMARK_NODE_FOOTNOTE_REFERENCE ||
                node->type == CMARK_NODE_INLINE_FOOTNOTE)) {
      entry = S_footnote(state, node->as.footnote_ref.number);
      if (entry) {
        ++entry->references;
        if (node->type == CMARK_NODE_INLINE_FOOTNOTE)
          entry->node = node;
      }
    }
  }
  cmark_iter_free(iter);
  if (!state->headings || !state->config->heading_ids)
    return;
  /* Native heading projections are source-ordered, even when definitions
   * have been relocated in the tree or are emitted after the main body. */
  if (state->heading_count > 1)
    qsort(state->headings, state->heading_count, sizeof(*state->headings),
          S_heading_source_compare);
  for (i = 0; i < state->heading_count && i < state->config->heading_count; ++i)
    state->headings[i].id = state->config->heading_ids[i];
  if (state->heading_count > 1)
    qsort(state->headings, state->heading_count, sizeof(*state->headings),
          S_heading_compare);
}

static void S_footnote_reference(cmark_node *node, struct render_state *state) {
  char number_text[BUFFER_SIZE];
  int number = node->as.footnote_ref.number;
  struct footnote_entry *entry = S_footnote(state, number);
  unsigned int occurrence = entry ? entry->rendered++ : 0;
  cmark_strbuf *html = S_html(state);
  snprintf(number_text, sizeof(number_text),
           "<sup class=\"footnote-ref\"><a href=\"#fn%d\" id=\"fnref%d", number,
           number);
  cmark_strbuf_puts(html, number_text);
  if (occurrence) {
    snprintf(number_text, sizeof(number_text), ":%u", occurrence);
    cmark_strbuf_puts(html, number_text);
  }
  snprintf(number_text, sizeof(number_text), "\">[%d", number);
  cmark_strbuf_puts(html, number_text);
  if (occurrence) {
    snprintf(number_text, sizeof(number_text), ":%u", occurrence);
    cmark_strbuf_puts(html, number_text);
  }
  cmark_strbuf_puts(html, "]</a></sup>");
}

static void S_footnote_backlinks(struct render_state *state, int number) {
  struct footnote_entry *entry = S_footnote(state, number);
  unsigned int i;
  char buffer[BUFFER_SIZE];
  for (i = 0; entry && i < entry->references; ++i) {
    snprintf(buffer, sizeof(buffer), " <a href=\"#fnref%d", number);
    cmark_strbuf_puts(S_html(state), buffer);
    if (i) {
      snprintf(buffer, sizeof(buffer), ":%u", i);
      cmark_strbuf_puts(S_html(state), buffer);
    }
    cmark_strbuf_puts(
        S_html(state),
        "\" class=\"footnote-backref\">\xe2\x86\xa9\xef\xb8\x8e</a>");
  }
}

static void S_render_sourcepos(cmark_node *node, cmark_strbuf *html,
                               int options) {
  char buffer[BUFFER_SIZE];
  if (CMARK_OPT_SOURCEPOS & options) {
    snprintf(buffer, BUFFER_SIZE, " data-sourcepos=\"%d:%d-%d:%d\"",
             cmark_node_get_start_line(node), cmark_node_get_start_column(node),
             cmark_node_get_end_line(node), cmark_node_get_end_column(node));
    cmark_strbuf_puts(html, buffer);
  }
}

enum toc_attribute {
  TOC_CONTAINER_CLASS,
  TOC_CONTAINER_ID,
  TOC_LIST_CLASS,
  TOC_ITEM_CLASS,
  TOC_LINK_CLASS,
  TOC_ATTRIBUTE_COUNT
};

struct toc_options {
  unsigned int levels;
  bool unordered;
  cmark_strbuf attrs[TOC_ATTRIBUTE_COUNT];
};

static void S_json_space(const unsigned char **cursor) {
  while (**cursor == ' ' || **cursor == '\t' || **cursor == '\n' ||
         **cursor == '\r')
    ++*cursor;
}

static int S_json_hex(const unsigned char **cursor) {
  unsigned int value = 0;
  int i;
  for (i = 0; i < 4; ++i) {
    unsigned char ch = *(*cursor)++;
    value <<= 4;
    if (ch >= '0' && ch <= '9')
      value |= ch - '0';
    else if (ch >= 'a' && ch <= 'f')
      value |= ch - 'a' + 10;
    else if (ch >= 'A' && ch <= 'F')
      value |= ch - 'A' + 10;
    else
      return -1;
  }
  return (int)value;
}

static bool S_json_string(const unsigned char **cursor, cmark_strbuf *value) {
  if (**cursor != '"')
    return false;
  ++*cursor;
  if (value)
    cmark_strbuf_clear(value);
  while (**cursor && **cursor != '"') {
    unsigned char ch = *(*cursor)++;
    if (ch < ' ')
      return false;
    if (ch == '\\') {
      ch = *(*cursor)++;
      switch (ch) {
      case '"':
      case '\\':
      case '/':
        break;
      case 'b':
        ch = '\b';
        break;
      case 'f':
        ch = '\f';
        break;
      case 'n':
        ch = '\n';
        break;
      case 'r':
        ch = '\r';
        break;
      case 't':
        ch = '\t';
        break;
      case 'u': {
        int codepoint = S_json_hex(cursor);
        if (codepoint < 0)
          return false;
        if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
          int low;
          if ((*cursor)[0] != '\\' || (*cursor)[1] != 'u')
            return false;
          *cursor += 2;
          low = S_json_hex(cursor);
          if (low < 0xdc00 || low > 0xdfff)
            return false;
          codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + low - 0xdc00;
        } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
          return false;
        }
        if (value)
          cmark_utf8proc_encode_char(codepoint, value);
        continue;
      }
      default:
        return false;
      }
    }
    if (value)
      cmark_strbuf_putc(value, ch);
  }
  if (**cursor != '"')
    return false;
  ++*cursor;
  return true;
}

static bool S_json_number(const unsigned char **cursor, double *value) {
  const unsigned char *start = *cursor;
  const unsigned char *p = start;
  if (*p == '-')
    ++p;
  if (*p == '0')
    ++p;
  else if (*p >= '1' && *p <= '9') {
    do {
      ++p;
    } while (*p >= '0' && *p <= '9');
  } else {
    return false;
  }
  if (*p == '.') {
    ++p;
    if (*p < '0' || *p > '9')
      return false;
    do {
      ++p;
    } while (*p >= '0' && *p <= '9');
  }
  if (*p == 'e' || *p == 'E') {
    ++p;
    if (*p == '+' || *p == '-')
      ++p;
    if (*p < '0' || *p > '9')
      return false;
    do {
      ++p;
    } while (*p >= '0' && *p <= '9');
  }
  if (value) {
    const unsigned char *digit = start;
    double result = 0, fraction = 0.1;
    int exponent = 0;
    bool negative = *digit == '-';
    bool negative_exponent = false;
    if (negative)
      ++digit;
    while (*digit >= '0' && *digit <= '9')
      result = result * 10 + (*digit++ - '0');
    if (*digit == '.') {
      ++digit;
      while (*digit >= '0' && *digit <= '9') {
        result += (*digit++ - '0') * fraction;
        fraction *= 0.1;
      }
    }
    if (*digit == 'e' || *digit == 'E') {
      ++digit;
      negative_exponent = *digit == '-';
      if (*digit == '+' || *digit == '-')
        ++digit;
      while (*digit >= '0' && *digit <= '9') {
        if (exponent < 400)
          exponent = exponent * 10 + (*digit - '0');
        ++digit;
      }
      if (exponent > 400)
        exponent = 400;
      while (exponent--)
        result *= negative_exponent ? 0.1 : 10;
    }
    *value = negative ? -result : result;
  }
  *cursor = p;
  return true;
}

/* Unknown JSON options are consumed iteratively, including nested objects.
 * The stack is per-call and grows with the input, never with the C stack. */
static bool S_json_skip(const unsigned char **cursor, cmark_mem *mem) {
  cmark_strbuf stack = CMARK_BUF_INIT(mem);
  bool valid = true;
  bool need_value = true;
  do {
    S_json_space(cursor);
    if (need_value) {
      if (**cursor == '{' || **cursor == '[') {
        unsigned char open = *(*cursor)++;
        cmark_strbuf_putc(&stack, open);
        S_json_space(cursor);
        if (**cursor == (open == '{' ? '}' : ']')) {
          ++*cursor;
          cmark_strbuf_truncate(&stack, stack.size - 1);
        } else {
          if (open == '{') {
            if (!S_json_string(cursor, NULL)) {
              valid = false;
              break;
            }
            S_json_space(cursor);
            if (*(*cursor)++ != ':') {
              valid = false;
              break;
            }
          }
          continue;
        }
      } else if (**cursor == '"') {
        if (!S_json_string(cursor, NULL)) {
          valid = false;
          break;
        }
      } else if (strncmp((const char *)*cursor, "true", 4) == 0) {
        *cursor += 4;
      } else if (strncmp((const char *)*cursor, "false", 5) == 0) {
        *cursor += 5;
      } else if (strncmp((const char *)*cursor, "null", 4) == 0) {
        *cursor += 4;
      } else if (!S_json_number(cursor, NULL)) {
        valid = false;
        break;
      }
      need_value = false;
    }
    if (stack.size) {
      unsigned char open = stack.ptr[stack.size - 1];
      S_json_space(cursor);
      if (**cursor == (open == '{' ? '}' : ']')) {
        ++*cursor;
        cmark_strbuf_truncate(&stack, stack.size - 1);
      } else if (**cursor == ',') {
        ++*cursor;
        S_json_space(cursor);
        if (open == '{') {
          if (!S_json_string(cursor, NULL)) {
            valid = false;
            break;
          }
          S_json_space(cursor);
          if (*(*cursor)++ != ':') {
            valid = false;
            break;
          }
        }
        need_value = true;
      } else {
        valid = false;
        break;
      }
    }
  } while (stack.size || need_value);
  cmark_strbuf_free(&stack);
  return valid;
}

static bool S_toc_levels(const unsigned char **cursor, unsigned int *levels) {
  bool array = **cursor == '[';
  double level;
  int i;
  *levels = 0;
  if (array) {
    ++*cursor;
    S_json_space(cursor);
    if (**cursor == ']') {
      ++*cursor;
      return true;
    }
  }
  do {
    S_json_space(cursor);
    if (!S_json_number(cursor, &level))
      return false;
    for (i = 1; i <= 6; ++i) {
      if (array ? level == i : i >= level)
        *levels |= 1u << i;
    }
    S_json_space(cursor);
    if (!array)
      return true;
    if (**cursor == ']') {
      ++*cursor;
      return true;
    }
    if (*(*cursor)++ != ',')
      return false;
  } while (true);
}

static void S_toc_options(cmark_node *node, struct toc_options *options,
                          bool viewer) {
  const unsigned char *p =
      (const unsigned char *)cmark_node_get_toc_options(node);
  cmark_strbuf key = CMARK_BUF_INIT(node->mem);
  cmark_strbuf value = CMARK_BUF_INIT(node->mem);
  bool valid = true;
  int i;
  options->levels = 0x7e;
  options->unordered = false;
  for (i = 0; i < TOC_ATTRIBUTE_COUNT; ++i)
    cmark_strbuf_init(node->mem, &options->attrs[i], 0);
  cmark_strbuf_puts(&options->attrs[TOC_CONTAINER_CLASS],
                    viewer ? "vx-table-of-contents" : "table-of-contents");
  if (!*p)
    return;
  S_json_space(&p);
  if (*p++ != '{') {
    valid = false;
  } else {
    S_json_space(&p);
    while (*p && *p != '}') {
      int attr = -1;
      if (!S_json_string(&p, &key)) {
        valid = false;
        break;
      }
      S_json_space(&p);
      if (*p++ != ':') {
        valid = false;
        break;
      }
      S_json_space(&p);
      if (strcmp((const char *)key.ptr, "containerClass") == 0)
        attr = TOC_CONTAINER_CLASS;
      else if (strcmp((const char *)key.ptr, "containerId") == 0)
        attr = TOC_CONTAINER_ID;
      else if (strcmp((const char *)key.ptr, "listClass") == 0)
        attr = TOC_LIST_CLASS;
      else if (strcmp((const char *)key.ptr, "itemClass") == 0)
        attr = TOC_ITEM_CLASS;
      else if (strcmp((const char *)key.ptr, "linkClass") == 0)
        attr = TOC_LINK_CLASS;
      if (attr >= 0 && *p == '"') {
        valid = S_json_string(&p, &options->attrs[attr]);
      } else if (strcmp((const char *)key.ptr, "listType") == 0 && *p == '"') {
        valid = S_json_string(&p, &value);
        options->unordered = strcmp((const char *)value.ptr, "ul") == 0;
      } else if (strcmp((const char *)key.ptr, "level") == 0) {
        valid = S_toc_levels(&p, &options->levels);
      } else {
        valid = S_json_skip(&p, node->mem);
      }
      if (!valid)
        break;
      S_json_space(&p);
      if (*p == '}')
        break;
      if (*p++ != ',') {
        valid = false;
        break;
      }
      S_json_space(&p);
      if (*p == '}') {
        valid = false;
        break;
      }
    }
    if (valid) {
      if (*p != '}') {
        valid = false;
      } else {
        ++p;
        S_json_space(&p);
        valid = !*p;
      }
    }
  }
  if (!valid) {
    options->levels = 0x7e;
    options->unordered = false;
    for (i = 0; i < TOC_ATTRIBUTE_COUNT; ++i)
      cmark_strbuf_clear(&options->attrs[i]);
    cmark_strbuf_puts(&options->attrs[TOC_CONTAINER_CLASS],
                      viewer ? "vx-table-of-contents" : "table-of-contents");
  }
  cmark_strbuf_free(&key);
  cmark_strbuf_free(&value);
}

static void S_toc_attribute(cmark_strbuf *html, const char *name,
                            const cmark_strbuf *value) {
  if (!value->size)
    return;
  cmark_strbuf_putc(html, ' ');
  cmark_strbuf_puts(html, name);
  cmark_strbuf_puts(html, "=\"");
  escape_html(html, value->ptr, value->size);
  cmark_strbuf_putc(html, '"');
}

static void S_render_toc(cmark_node *node, struct render_state *state) {
  struct toc_options options;
  cmark_strbuf *html = S_html(state);
  cmark_iter *iter = cmark_iter_new(state->root);
  cmark_event_type ev;
  size_t footnote_index = 0;
  int ancestors[6];
  int ancestor_count = 0, depth = 0, i;
  S_toc_options(node, &options, state->config->viewer != 0);
  cr(html);
  cmark_strbuf_puts(html, "<nav");
  S_toc_attribute(html, "id", &options.attrs[TOC_CONTAINER_ID]);
  S_toc_attribute(html, "class", &options.attrs[TOC_CONTAINER_CLASS]);
  cmark_strbuf_putc(html, '>');
  while (true) {
    cmark_node *heading;
    int level;
    const char *id;
    ev = cmark_iter_next(iter);
    if (ev == CMARK_EVENT_DONE) {
      if (!state->config->viewer)
        break;
      /* Match emission: main body, then referenced notes in number order.
       * Empty heading IDs are valid and never indicate hidden content. */
      do {
        ++footnote_index;
      } while (footnote_index < state->footnote_capacity &&
               (!state->footnotes[footnote_index].node ||
                !state->footnotes[footnote_index].references));
      if (footnote_index >= state->footnote_capacity)
        break;
      cmark_iter_free(iter);
      iter = cmark_iter_new(state->footnotes[footnote_index].node);
      continue;
    }
    heading = cmark_iter_get_node(iter);
    if (state->config->viewer && ev == CMARK_EVENT_ENTER &&
        heading->type == CMARK_NODE_FOOTNOTE_DEFINITION &&
        heading != cmark_iter_get_root(iter)) {
      cmark_iter_reset(iter, heading, CMARK_EVENT_EXIT);
      continue;
    }
    if (ev != CMARK_EVENT_ENTER || heading->type != CMARK_NODE_HEADING)
      continue;
    level = heading->as.heading.level;
    while (ancestor_count && ancestors[ancestor_count - 1] >= level)
      --ancestor_count;
    if (!(options.levels & (1u << level)))
      continue;
    while (depth > ancestor_count + 1) {
      cmark_strbuf_puts(html, options.unordered ? "</li></ul>" : "</li></ol>");
      --depth;
    }
    if (depth == ancestor_count + 1) {
      cmark_strbuf_puts(html, "</li>");
    } else {
      cmark_strbuf_puts(html, options.unordered ? "<ul" : "<ol");
      S_toc_attribute(html, "class", &options.attrs[TOC_LIST_CLASS]);
      cmark_strbuf_putc(html, '>');
      ++depth;
    }
    ancestors[ancestor_count++] = level;
    cmark_strbuf_puts(html, "<li");
    S_toc_attribute(html, "class", &options.attrs[TOC_ITEM_CLASS]);
    cmark_strbuf_putc(html, '>');
    id = S_heading_id(state, heading);
    if (id) {
      cmark_strbuf_puts(html, "<a");
      S_toc_attribute(html, "class", &options.attrs[TOC_LINK_CLASS]);
      cmark_strbuf_puts(html, " href=\"");
      S_heading_link(html, id);
      cmark_strbuf_puts(html, "\">");
    }
    state->plain = heading;
    state->plain_heading = true;
    S_render_children(heading, state);
    state->plain = NULL;
    state->plain_heading = false;
    if (id)
      cmark_strbuf_puts(html, "</a>");
  }
  while (depth--) {
    cmark_strbuf_puts(html, options.unordered ? "</li></ul>" : "</li></ol>");
  }
  cmark_strbuf_puts(html, "</nav>\n");
  cmark_iter_free(iter);
  for (i = 0; i < TOC_ATTRIBUTE_COUNT; ++i)
    cmark_strbuf_free(&options.attrs[i]);
}

static int S_render_node(cmark_node *node, cmark_event_type ev_type,
                         struct render_state *state, int options) {
  cmark_node *parent;
  cmark_node *grandparent;
  cmark_strbuf *html = state->in_footnote ? state->footnotes_html : state->html;
  char start_heading[] = "<h0";
  char end_heading[] = "</h0";
  bool tight;
  char buffer[BUFFER_SIZE];

  bool entering = (ev_type == CMARK_EVENT_ENTER);
  bool viewer = state->config->viewer != 0;

  if (state->plain == node) { // back at original node
    state->plain = NULL;
  }

  if (state->plain != NULL) {
    switch (node->type) {
    case CMARK_NODE_TEXT:
    case CMARK_NODE_CODE:
      escape_html(html, node->data, node->len);
      break;

    case CMARK_NODE_HTML_INLINE:
      if (!state->plain_heading)
        escape_html(html, node->data, node->len);
      break;

    case CMARK_NODE_IMAGE:
      if (state->plain_heading)
        return 0;
      break;

    case CMARK_NODE_FORMULA_INLINE:
      if (viewer || state->plain_heading)
        escape_html(html, node->data, node->len);
      break;

    case CMARK_NODE_FOOTNOTE_REFERENCE:
    case CMARK_NODE_INLINE_FOOTNOTE:
      if (viewer || state->plain_heading)
        return 0;
      break;

    case CMARK_NODE_LINEBREAK:
    case CMARK_NODE_SOFTBREAK:
      cmark_strbuf_putc(html, ' ');
      break;

    default:
      break;
    }
    return 1;
  }

  if (state->display_math_paragraph &&
      node->parent == state->display_math_paragraph) {
    if (node == state->paragraph_remainder && entering) {
      cmark_node *paragraph = state->display_math_paragraph;
      parent = paragraph->parent;
      grandparent = parent ? parent->parent : NULL;
      tight = grandparent && grandparent->type == CMARK_NODE_LIST &&
              grandparent->as.list.tight;
      state->display_math_paragraph = NULL;
      state->paragraph_remainder = NULL;
      if (!tight) {
        cr(html);
        cmark_strbuf_puts(html, "<p");
        S_source_line(node, html, NULL);
        S_render_sourcepos(paragraph, html, options);
        cmark_strbuf_putc(html, '>');
      }
    } else if (node->type != CMARK_NODE_FORMULA_INLINE) {
      return 1; /* Whitespace separating the standalone display lines. */
    }
  }

  switch (node->type) {
  case CMARK_NODE_DOCUMENT:
    break;

  case CMARK_NODE_BLOCK_QUOTE:
    if (entering) {
      cr(html);
      cmark_strbuf_puts(html, "<blockquote");
      S_render_sourcepos(node, html, options);
      cmark_strbuf_puts(html, ">\n");
    } else {
      cr(html);
      cmark_strbuf_puts(html, "</blockquote>\n");
    }
    break;

  case CMARK_NODE_LIST: {
    cmark_list_type list_type = (cmark_list_type)node->as.list.list_type;
    int start = node->as.list.start;

    if (entering) {
      cr(html);
      if (list_type == CMARK_BULLET_LIST) {
        cmark_strbuf_puts(html, "<ul");
        if (S_task_list(node))
          cmark_strbuf_puts(html, " class=\"contains-task-list\"");
        S_render_sourcepos(node, html, options);
        cmark_strbuf_puts(html, ">\n");
      } else if (start == 1) {
        cmark_strbuf_puts(html, "<ol");
        if (S_task_list(node))
          cmark_strbuf_puts(html, " class=\"contains-task-list\"");
        S_render_sourcepos(node, html, options);
        cmark_strbuf_puts(html, ">\n");
      } else {
        snprintf(buffer, BUFFER_SIZE, "<ol start=\"%d\"", start);
        cmark_strbuf_puts(html, buffer);
        if (S_task_list(node))
          cmark_strbuf_puts(html, " class=\"contains-task-list\"");
        S_render_sourcepos(node, html, options);
        cmark_strbuf_puts(html, ">\n");
      }
    } else {
      cmark_strbuf_puts(html,
                        list_type == CMARK_BULLET_LIST ? "</ul>\n" : "</ol>\n");
    }
    break;
  }

  case CMARK_NODE_ITEM:
    if (entering) {
      cr(html);
      cmark_strbuf_puts(html, "<li");
      if (viewer)
        S_source_line(node, html,
                      cmark_node_get_task_checked(node) >= 0
                          ? "task-list-item enabled"
                          : NULL);
      else if (cmark_node_get_task_checked(node) >= 0)
        cmark_strbuf_puts(html, " class=\"task-list-item\"");
      S_render_sourcepos(node, html, options);
      cmark_strbuf_putc(html, '>');
    } else {
      cmark_strbuf_puts(html, "</li>\n");
    }
    break;

  case CMARK_NODE_HEADING:
    if (entering) {
      cr(html);
      start_heading[2] = (char)('0' + node->as.heading.level);
      cmark_strbuf_puts(html, start_heading);
      if (viewer) {
        const char *id = S_heading_id(state, node);
        if (id) {
          cmark_strbuf_puts(html, " id=\"");
          escape_html(html, (const unsigned char *)id, (bufsize_t)strlen(id));
          cmark_strbuf_putc(html, '"');
        }
        S_source_line(node, html, NULL);
      }
      S_render_sourcepos(node, html, options);
      cmark_strbuf_putc(html, '>');
    } else {
      const char *id = viewer ? S_heading_id(state, node) : NULL;
      if (id) {
        cmark_strbuf_puts(html, "<a class=\"vx-header-anchor\" href=\"");
        S_heading_link(html, id);
        cmark_strbuf_puts(html, "\" vx-data-anchor-icon=\"\xc2\xb6\"></a>");
      }
      end_heading[3] = (char)('0' + node->as.heading.level);
      cmark_strbuf_puts(html, end_heading);
      cmark_strbuf_puts(html, ">\n");
    }
    break;

  case CMARK_NODE_CODE_BLOCK:
    cr(html);

    if (viewer) {
      const unsigned char *info = node->as.code.info;
      bufsize_t length = 0;
      cmark_strbuf_puts(html, "<pre");
      if (!node->as.code.fenced)
        S_source_line(node, html, NULL);
      S_render_sourcepos(node, html, options);
      cmark_strbuf_puts(html, "><code");
      if (node->as.code.fenced) {
        if (info) {
          while (info[length] && !cmark_isspace(info[length]))
            ++length;
        }
        cmark_strbuf_puts(html, " class=\"source-line");
        if (length) {
          cmark_strbuf_puts(html, " lang-");
          escape_html(html, info, length);
        }
        snprintf(buffer, sizeof(buffer), "\" data-source-line=\"%d\"",
                 node->start_line > 0 ? node->start_line - 1 : 0);
        cmark_strbuf_puts(html, buffer);
      }
      cmark_strbuf_putc(html, '>');
      escape_html(html, node->data, node->len);
      cmark_strbuf_puts(html, "</code></pre>\n");
      break;
    }

    if (node->as.code.info == NULL || node->as.code.info[0] == 0) {
      cmark_strbuf_puts(html, "<pre");
      S_render_sourcepos(node, html, options);
      cmark_strbuf_puts(html, "><code>");
    } else {
      bufsize_t first_tag = 0;
      while (node->as.code.info[first_tag] &&
             !cmark_isspace(node->as.code.info[first_tag])) {
        first_tag += 1;
      }

      cmark_strbuf_puts(html, "<pre");
      S_render_sourcepos(node, html, options);
      cmark_strbuf_puts(html, "><code class=\"");
      if (strncmp((char *)node->as.code.info, "language-", 9) != 0) {
        cmark_strbuf_puts(html, "language-");
      }
      escape_html(html, node->as.code.info, first_tag);
      cmark_strbuf_puts(html, "\">");
    }

    escape_html(html, node->data, node->len);
    cmark_strbuf_puts(html, "</code></pre>\n");
    break;

  case CMARK_NODE_HTML_BLOCK:
    cr(html);
    if (viewer ? !state->config->render_raw_html
               : !(options & CMARK_OPT_UNSAFE)) {
      cmark_strbuf_puts(html, "<!-- raw HTML omitted -->");
    } else {
      cmark_strbuf_put(html, node->data, node->len);
    }
    cr(html);
    break;

  case CMARK_NODE_CUSTOM_BLOCK: {
    unsigned char *block = entering ? node->as.custom.on_enter :
                                      node->as.custom.on_exit;
    cr(html);
    if (block) {
      cmark_strbuf_puts(html, (char *)block);
    }
    cr(html);
    break;
  }

  case CMARK_NODE_THEMATIC_BREAK:
    cr(html);
    cmark_strbuf_puts(html, "<hr");
    S_render_sourcepos(node, html, options);
    cmark_strbuf_puts(html, " />\n");
    break;

  case CMARK_NODE_PARAGRAPH: {
    cmark_node *figure = viewer ? S_figure_image(node) : NULL;
    parent = cmark_node_parent(node);
    grandparent = cmark_node_parent(parent);
    tight = grandparent && grandparent->type == CMARK_NODE_LIST &&
            grandparent->as.list.tight;
    if (viewer && entering && node->first_child &&
        !(parent && node == parent->first_child &&
          cmark_node_get_task_checked(parent) >= 0)) {
      cmark_node *remainder = S_display_math_remainder(node);
      if (remainder != node->first_child) {
        state->display_math_paragraph = node;
        state->paragraph_remainder = remainder;
        cr(html);
        break;
      }
    } else if (!entering && state->display_math_paragraph == node) {
      if (parent == state->active_footnote && !node->next)
        S_footnote_backlinks(state, parent->as.footnote_def.number);
      state->display_math_paragraph = NULL;
      state->paragraph_remainder = NULL;
      cr(html);
      break;
    }
    if (figure) {
      if (entering) {
        cr(html);
        cmark_strbuf_puts(html, "<figure>");
      } else {
        if (figure->first_child) {
          cmark_strbuf_puts(html, "<figcaption>");
          S_render_children(figure, state);
          cmark_strbuf_puts(html, "</figcaption>");
        }
        cmark_strbuf_puts(html, "</figure>\n");
      }
    } else if (!tight) {
      if (entering) {
        cr(html);
        cmark_strbuf_puts(html, "<p");
        if (viewer)
          S_source_line(node, html, NULL);
        S_render_sourcepos(node, html, options);
        cmark_strbuf_putc(html, '>');
      } else {
        if (viewer && parent == state->active_footnote && !node->next)
          S_footnote_backlinks(state, parent->as.footnote_def.number);
        cmark_strbuf_puts(html, "</p>\n");
      }
    }
    if (entering && parent && node == parent->first_child &&
        cmark_node_get_task_checked(parent) >= 0)
      S_task_checkbox(html, cmark_node_get_task_checked(parent), viewer);
    break;
  }

  case CMARK_NODE_TEXT:
    escape_html(html, node->data, node->len);
    break;

  case CMARK_NODE_LINEBREAK:
    cmark_strbuf_puts(html, "<br />\n");
    break;

  case CMARK_NODE_SOFTBREAK:
    if (options & CMARK_OPT_HARDBREAKS) {
      cmark_strbuf_puts(html, "<br />\n");
    } else if (options & CMARK_OPT_NOBREAKS) {
      cmark_strbuf_putc(html, ' ');
    } else {
      cmark_strbuf_putc(html, '\n');
    }
    break;

  case CMARK_NODE_CODE:
    cmark_strbuf_puts(html, "<code>");
    escape_html(html, node->data, node->len);
    cmark_strbuf_puts(html, "</code>");
    break;

  case CMARK_NODE_HTML_INLINE:
    if (viewer ? !state->config->render_raw_html
               : !(options & CMARK_OPT_UNSAFE)) {
      cmark_strbuf_puts(html, "<!-- raw HTML omitted -->");
    } else {
      cmark_strbuf_put(html, node->data, node->len);
    }
    break;

  case CMARK_NODE_CUSTOM_INLINE: {
    unsigned char *block = entering ? node->as.custom.on_enter :
                                      node->as.custom.on_exit;
    if (block) {
      cmark_strbuf_puts(html, (char *)block);
    }
    break;
  }

  case CMARK_NODE_STRONG:
    if (entering) {
      cmark_strbuf_puts(html, "<strong>");
    } else {
      cmark_strbuf_puts(html, "</strong>");
    }
    break;

  case CMARK_NODE_STRIKETHROUGH:
    if (entering) {
      cmark_strbuf_puts(html, "<s>");
    } else {
      cmark_strbuf_puts(html, "</s>");
    }
    break;

  case CMARK_NODE_MARK:
    if (entering) {
      cmark_strbuf_puts(html, "<mark>");
    } else {
      cmark_strbuf_puts(html, "</mark>");
    }
    break;

  case CMARK_NODE_SUBSCRIPT:
    cmark_strbuf_puts(html, entering ? "<sub>" : "</sub>");
    break;

  case CMARK_NODE_SUPERSCRIPT:
    cmark_strbuf_puts(html, entering ? "<sup>" : "</sup>");
    break;

  case CMARK_NODE_ALERT:
    if (entering) {
      const char *type = cmark_node_get_alert_type(node);
      cr(html);
      cmark_strbuf_puts(html, viewer ? "<div class=\"vx-alert "
                                     : "<div class=\"alert ");
      escape_html(html, (const unsigned char *)type, (bufsize_t)strlen(type));
      cmark_strbuf_puts(html, "\" role=\"alert\">");
    } else {
      cmark_strbuf_puts(html, "</div>\n");
    }
    break;

  case CMARK_NODE_TOC:
    S_render_toc(node, state);
    break;

  case CMARK_NODE_WIKILINK:
    if (entering) {
      cmark_strbuf_puts(html, "<a class=\"wikilink\" href=\"");
      if (node->as.link.url && (!viewer || S_url_allowed(node, state))) {
        if (viewer)
          houdini_escape_href(html, node->as.link.url,
                              (bufsize_t)strlen((char *)node->as.link.url));
        else
          escape_html(html, node->as.link.url,
                      (bufsize_t)strlen((char *)node->as.link.url));
      }
      cmark_strbuf_puts(html, "\">");
    } else {
      cmark_strbuf_puts(html, "</a>");
    }
    break;

  case CMARK_NODE_EMPH:
    if (entering) {
      cmark_strbuf_puts(html, "<em>");
    } else {
      cmark_strbuf_puts(html, "</em>");
    }
    break;

  case CMARK_NODE_LINK:
    if (entering) {
      cmark_strbuf_puts(html, "<a href=\"");
      if (S_url_allowed(node, state)) {
        houdini_escape_href(html, node->as.link.url,
                            (bufsize_t)strlen((char *)node->as.link.url));
      }
      if (node->as.link.title) {
        cmark_strbuf_puts(html, "\" title=\"");
        escape_html(html, node->as.link.title,
                    (bufsize_t)strlen((char *)node->as.link.title));
      }
      cmark_strbuf_puts(html, "\">");
    } else {
      cmark_strbuf_puts(html, "</a>");
    }
    break;

  case CMARK_NODE_IMAGE:
    if (entering) {
      cmark_strbuf_puts(html, "<img src=\"");
      if (S_url_allowed(node, state)) {
        houdini_escape_href(html, node->as.link.url,
                            (bufsize_t)strlen((char *)node->as.link.url));
      }
      cmark_strbuf_puts(html, "\" alt=\"");
      if (viewer && S_is_figure_image(node)) {
        /* Caption children are rendered at paragraph exit, not as alt text. */
        S_render_node(node, CMARK_EVENT_EXIT, state, options);
        return 0;
      }
      state->plain = node;
    } else {
      if (node->as.link.title) {
        cmark_strbuf_puts(html, "\" title=\"");
        escape_html(html, node->as.link.title,
                    (bufsize_t)strlen((char *)node->as.link.title));
      }

      cmark_strbuf_putc(html, '"');
      if (viewer) {
        const char *width = cmark_node_get_image_width_text(node);
        const char *height = cmark_node_get_image_height_text(node);
        if (*width) {
          cmark_strbuf_puts(html, " width=\"");
          escape_html(html, (const unsigned char *)width,
                      (bufsize_t)strlen(width));
          cmark_strbuf_putc(html, '"');
        }
        if (*height) {
          cmark_strbuf_puts(html, " height=\"");
          escape_html(html, (const unsigned char *)height,
                      (bufsize_t)strlen(height));
          cmark_strbuf_putc(html, '"');
        }
      }
      cmark_strbuf_puts(html, " />");
    }
    break;

  case CMARK_NODE_FRONTMATTER:
    if (viewer && node->len) {
      cr(html);
      cmark_strbuf_puts(html, "<details class=\"vx-frontmatter\">"
                              "<summary>Metadata</summary><pre>");
      escape_html(html, node->data, node->len);
      cmark_strbuf_puts(html, "</pre></details>\n");
    }
    break;

  case CMARK_NODE_FOOTNOTE_REFERENCE: {
    if (viewer) {
      S_footnote_reference(node, state);
      break;
    }
    int number = node->as.footnote_ref.number;
    snprintf(buffer, BUFFER_SIZE,
             "<sup class=\"footnote-ref\" id=\"fnref-%d\">"
             "<a href=\"#fn-%d\">%d</a></sup>",
             number, number, number);
    cmark_strbuf_puts(html, buffer);
    break;
  }

  case CMARK_NODE_FOOTNOTE_DEFINITION: {
    int number = node->as.footnote_def.number;
    if (number == 0)
      return 0;
    if (viewer) {
      if (node != state->active_footnote)
        return 0;
      if (entering) {
        snprintf(buffer, sizeof(buffer),
                 "<li id=\"fn%d\" class=\"footnote-item\">", number);
        cmark_strbuf_puts(html, buffer);
      } else {
        if (!node->last_child ||
            node->last_child->type != CMARK_NODE_PARAGRAPH ||
            S_figure_image(node->last_child))
          S_footnote_backlinks(state, number);
        cmark_strbuf_puts(html, "</li>\n");
      }
      break;
    }
    if (entering) {
      snprintf(buffer, BUFFER_SIZE, "<li id=\"fn-%d\" value=\"%d\">\n", number, number);
      cmark_strbuf_puts(state->footnotes_html, buffer);
      state->in_footnote = true;
    } else {
      cmark_strbuf *fn = state->footnotes_html;
      snprintf(buffer, BUFFER_SIZE,
               " <a href=\"#fnref-%d\" class=\"footnote-backref\">"
               "\xe2\x86\xa9</a>",
               number);
      if (fn->size >= 5 &&
          memcmp(fn->ptr + fn->size - 5, "</p>\n", 4) == 0) {
        fn->size -= 5;
        fn->ptr[fn->size] = 0;
        cmark_strbuf_puts(fn, buffer);
        cmark_strbuf_puts(fn, "</p>\n");
      } else {
        cmark_strbuf_puts(fn, buffer);
        cmark_strbuf_putc(fn, '\n');
      }
      cmark_strbuf_puts(fn, "</li>\n");
      state->in_footnote = false;
    }
    break;
  }

  case CMARK_NODE_INLINE_FOOTNOTE: {
    int number = node->as.footnote_ref.number;
    if (viewer) {
      if (node != state->active_footnote) {
        S_footnote_reference(node, state);
        return 0;
      }
      if (entering) {
        snprintf(buffer, sizeof(buffer),
                 "<li id=\"fn%d\" class=\"footnote-item\"><p>", number);
        cmark_strbuf_puts(html, buffer);
      } else {
        S_footnote_backlinks(state, number);
        cmark_strbuf_puts(html, "</p></li>\n");
      }
      break;
    }
    if (entering) {
      snprintf(buffer, BUFFER_SIZE,
               "<sup class=\"footnote-ref\" id=\"fnref-%d\">"
               "<a href=\"#fn-%d\">%d</a></sup>",
               number, number, number);
      cmark_strbuf_puts(state->html, buffer);
      snprintf(buffer, BUFFER_SIZE, "<li id=\"fn-%d\" value=\"%d\">\n<p>", number, number);
      cmark_strbuf_puts(state->footnotes_html, buffer);
      state->in_footnote = true;
    } else {
      snprintf(buffer, BUFFER_SIZE,
               " <a href=\"#fnref-%d\" class=\"footnote-backref\">"
               "\xe2\x86\xa9</a></p>\n</li>\n",
               number);
      cmark_strbuf_puts(state->footnotes_html, buffer);
      state->in_footnote = false;
    }
    break;
  }

  case CMARK_NODE_TABLE:
    if (entering) {
      cr(html);
      cmark_strbuf_puts(html, "<table");
      if (viewer)
        S_source_line(node, html, NULL);
      S_render_sourcepos(node, html, options);
      cmark_strbuf_puts(html, ">\n");
    } else {
      if (node->last_child &&
          node->last_child->as.table_row.type == CMARK_TABLE_ROW_TYPE_DATA) {
        cmark_strbuf_puts(html, "</tbody>\n");
      }
      cmark_strbuf_puts(html, "</table>\n");
    }
    break;

  case CMARK_NODE_TABLE_ROW:
    switch (node->as.table_row.type) {
    case CMARK_TABLE_ROW_TYPE_HEADER:
      if (entering) {
        cmark_strbuf_puts(html, "<thead>\n<tr");
        S_render_sourcepos(node, html, options);
        cmark_strbuf_puts(html, ">\n");
      } else {
        cmark_strbuf_puts(html, "</tr>\n</thead>\n");
      }
      break;
    case CMARK_TABLE_ROW_TYPE_DELIMITER:
      if (entering) {
        return 0;
      }
      break;
    case CMARK_TABLE_ROW_TYPE_DATA:
      if (entering) {
        if (node->prev &&
            node->prev->as.table_row.type != CMARK_TABLE_ROW_TYPE_DATA) {
          cmark_strbuf_puts(html, "<tbody>\n");
        }
        cmark_strbuf_puts(html, "<tr");
        S_render_sourcepos(node, html, options);
        cmark_strbuf_puts(html, ">\n");
      } else {
        cmark_strbuf_puts(html, "</tr>\n");
      }
      break;
    }
    break;

  case CMARK_NODE_TABLE_CELL: {
    const char *tag = (node->parent->as.table_row.type ==
                       CMARK_TABLE_ROW_TYPE_HEADER) ? "th" : "td";
    if (entering) {
      cr(html);
      cmark_strbuf_puts(html, "<");
      cmark_strbuf_puts(html, tag);
      if (node->parent && node->parent->parent &&
          node->parent->parent->type == CMARK_NODE_TABLE) {
        int col = node->as.table_cell.idx;
        if (col < node->parent->parent->as.table.columns_cnt) {
          switch (node->parent->parent->as.table.alignments[col]) {
          case CMARK_TABLE_ALIGN_LEFT:
            cmark_strbuf_puts(html, " style=\"text-align: left\"");
            break;
          case CMARK_TABLE_ALIGN_CENTER:
            cmark_strbuf_puts(html, " style=\"text-align: center\"");
            break;
          case CMARK_TABLE_ALIGN_RIGHT:
            cmark_strbuf_puts(html, " style=\"text-align: right\"");
            break;
          case CMARK_TABLE_ALIGN_NONE:
            break;
          }
        }
      }
      S_render_sourcepos(node, html, options);
      cmark_strbuf_putc(html, '>');
    } else {
      cmark_strbuf_puts(html, "</");
      cmark_strbuf_puts(html, tag);
      cmark_strbuf_puts(html, ">\n");
    }
    break;
  }

  case CMARK_NODE_FORMULA_INLINE: {
    bool display = cmark_node_get_formula_display(node);
    const char *label =
        viewer && display ? cmark_node_get_formula_label(node) : "";
    if (viewer && display) {
      cmark_strbuf_puts(html,
                        *label ? "<section class=\"eqno\">" : "<section>");
      cmark_strbuf_puts(html, "<eqn class=\"tex-to-render\">$");
    } else {
      cmark_strbuf_puts(html, "<eq class=\"tex-to-render\">$");
    }
    if (display)
      cmark_strbuf_putc(html, '$');
    if (node->as.formula.viewer)
      escape_html(html, node->data, node->len);
    else
      escape_inline_formula(html, node->data, node->len);
    if (display)
      cmark_strbuf_putc(html, '$');
    if (viewer && display) {
      cmark_strbuf_puts(html, "$</eqn>");
      if (*label) {
        cmark_strbuf_puts(html, "<span>(");
        escape_html(html, (const unsigned char *)label,
                    (bufsize_t)strlen(label));
        cmark_strbuf_puts(html, ")</span>");
      }
      cmark_strbuf_puts(html, "</section>");
    } else {
      cmark_strbuf_puts(html, "$</eq>");
    }
    break;
  }

  case CMARK_NODE_FORMULA_BLOCK: {
    const char *label = viewer ? cmark_node_get_formula_label(node) : "";
    cr(html);
    cmark_strbuf_puts(html, *label ? "<section class=\"eqno\">" : "<section>");
    cmark_strbuf_puts(html, "<eqn class=\"tex-to-render\"");
    S_render_sourcepos(node, html, options);
    cmark_strbuf_puts(html, ">$$");
    escape_html(html, node->data, node->len);
    cmark_strbuf_puts(html, "$$</eqn>");
    if (*label) {
      cmark_strbuf_puts(html, "<span>(");
      escape_html(html, (const unsigned char *)label, (bufsize_t)strlen(label));
      cmark_strbuf_puts(html, ")</span>");
    }
    cmark_strbuf_puts(html, "</section>\n");
    break;
  }

  default:
    assert(false);
    break;
  }

  // cmark_strbuf_putc(html, 'x');
  return 1;
}

static void S_render_tree(cmark_node *root, struct render_state *state) {
  cmark_iter *iter = cmark_iter_new(root);
  cmark_event_type ev_type;
  while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *cur = cmark_iter_get_node(iter);
    if (!S_render_node(cur, ev_type, state, state->config->options))
      cmark_iter_reset(iter, cur, CMARK_EVENT_EXIT);
  }
  cmark_iter_free(iter);
}

static void S_render_children(cmark_node *node, struct render_state *state) {
  cmark_iter *iter = cmark_iter_new(node);
  cmark_event_type ev_type;
  cmark_iter_next(iter); /* The enclosing element is already being emitted. */
  while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *cur = cmark_iter_get_node(iter);
    if (cur == node)
      break;
    if (!S_render_node(cur, ev_type, state, state->config->options))
      cmark_iter_reset(iter, cur, CMARK_EVENT_EXIT);
  }
  cmark_iter_free(iter);
}

char *cmark_render_html_with_options(cmark_node *root,
                                     const cmark_html_options *options) {
  const cmark_html_options defaults = {0};
  struct render_state state = {0};
  cmark_strbuf html;
  cmark_strbuf footnotes_buf;
  size_t i;
  char *result;
  if (!root)
    return NULL;
  cmark_strbuf_init(root->mem, &html, 0);
  cmark_strbuf_init(root->mem, &footnotes_buf, 0);
  state.html = &html;
  state.footnotes_html = &footnotes_buf;
  state.root = root;
  state.config = options ? options : &defaults;
  if (state.config->viewer)
    S_collect_metadata(&state);
  S_render_tree(root, &state);

  if (state.config->viewer) {
    state.in_footnote = true;
    for (i = 1; i < state.footnote_capacity; ++i) {
      if (state.footnotes[i].node && state.footnotes[i].references) {
        state.active_footnote = state.footnotes[i].node;
        S_render_tree(state.active_footnote, &state);
      }
    }
    if (footnotes_buf.size) {
      cmark_strbuf_puts(&html, "<hr class=\"footnotes-sep\" />\n"
                               "<section class=\"footnotes\">\n"
                               "<ol class=\"footnotes-list\">\n");
      cmark_strbuf_put(&html, footnotes_buf.ptr, footnotes_buf.size);
      cmark_strbuf_puts(&html, "</ol>\n</section>\n");
    }
  } else if (footnotes_buf.size) {
    cmark_strbuf_puts(&html,
                      "<section class=\"footnotes\" data-footnotes>\n"
                      "<hr />\n<ol>\n");
    cmark_strbuf_put(&html, footnotes_buf.ptr, footnotes_buf.size);
    cmark_strbuf_puts(&html, "</ol>\n</section>\n");
  }
  root->mem->free(state.footnotes);
  root->mem->free(state.headings);
  cmark_strbuf_free(&footnotes_buf);
  result = (char *)cmark_strbuf_detach(&html);
  return result;
}

char *cmark_render_html(cmark_node *root, int options) {
  cmark_html_options config = {0};
  config.options = options;
  return cmark_render_html_with_options(root, &config);
}
