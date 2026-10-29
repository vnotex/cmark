#include <stdbool.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmark_ctype.h"
#include "node.h"
#include "parser.h"
#include "references.h"
#include "footnotes.h"
#include "cmark.h"
#include "houdini.h"
#include "utf8.h"
#include "scanners.h"
#include "inlines.h"

static const char *EMDASH = "\xE2\x80\x94";
static const char *ENDASH = "\xE2\x80\x93";
static const char *ELLIPSES = "\xE2\x80\xA6";
static const char *LEFTDOUBLEQUOTE = "\xE2\x80\x9C";
static const char *RIGHTDOUBLEQUOTE = "\xE2\x80\x9D";
static const char *LEFTSINGLEQUOTE = "\xE2\x80\x98";
static const char *RIGHTSINGLEQUOTE = "\xE2\x80\x99";

// Macros for creating various kinds of simple.
#define make_linebreak(mem) make_simple(mem, CMARK_NODE_LINEBREAK)
#define make_softbreak(mem) make_simple(mem, CMARK_NODE_SOFTBREAK)
#define make_emph(mem) make_simple(mem, CMARK_NODE_EMPH)
#define make_strong(mem) make_simple(mem, CMARK_NODE_STRONG)

#define MAXBACKTICKS 1000

typedef struct delimiter {
  struct delimiter *previous;
  struct delimiter *next;
  cmark_node *inl_text;
  bufsize_t position;
  bufsize_t length;
  unsigned char delim_char;
  bool can_open;
  bool can_close;
} delimiter;

typedef struct bracket {
  struct bracket *previous;
  cmark_node *inl_text;
  bufsize_t position;
  bool image;
  bool active;
  bool bracket_after;
  bool inline_note;
} bracket;

#define FLAG_SKIP_HTML_CDATA        (1u << 0)
#define FLAG_SKIP_HTML_DECLARATION  (1u << 1)
#define FLAG_SKIP_HTML_PI           (1u << 2)
#define FLAG_SKIP_HTML_COMMENT      (1u << 3)

typedef struct {
  cmark_mem *mem;
  cmark_chunk input;
  unsigned flags;
  int line;
  bufsize_t pos;
  int block_offset;
  cmark_node *source;
  int column_offset;
  cmark_reference_map *refmap;
  cmark_footnote_map *footnote_map;
  delimiter *last_delim;
  bracket *last_bracket;
  bufsize_t backticks[MAXBACKTICKS + 1];
  bool scanned_for_backticks;
  bool no_link_openers;
  bool unescape_table_pipes;
  bufsize_t dollar_scan_end;
  bool dollar_scan_has_closer;
} subject;

static inline bool S_is_line_end_char(char c) {
  return (c == '\n' || c == '\r');
}

static delimiter *S_insert_emph_like_inline(subject *subj, delimiter *opener,
                                            delimiter *closer);

static int parse_inline(subject *subj, cmark_node *parent, int options);

static void subject_from_buf(cmark_mem *mem, int line_number, int block_offset, subject *e,
                             cmark_chunk *chunk, cmark_reference_map *refmap,
                             cmark_footnote_map *footnote_map);
static bufsize_t subject_find_special_char(subject *subj, int options);
static void subj_line_column_at(subject *subj, bufsize_t from, bufsize_t off,
                                int *p_line, int *p_column);

// Create an inline with a literal string value.
static inline cmark_node *make_literal(subject *subj, cmark_node_type t,
                                       int start_column, int end_column) {
  cmark_node *e = (cmark_node *)subj->mem->calloc(1, sizeof(*e));
  e->mem = subj->mem;
  e->type = (uint16_t)t;
  e->start_line = e->end_line = subj->line;
  // columns are 1 based.
  e->start_column = start_column + 1 + subj->column_offset + subj->block_offset;
  e->end_column = end_column + 1 + subj->column_offset + subj->block_offset;
  return e;
}

// Create an inline with no value.
static inline cmark_node *make_simple(cmark_mem *mem, cmark_node_type t) {
  cmark_node *e = (cmark_node *)mem->calloc(1, sizeof(*e));
  e->mem = mem;
  e->type = t;
  return e;
}

static cmark_node *make_str(subject *subj, int sc, int ec, cmark_chunk s) {
  cmark_node *e = make_literal(subj, CMARK_NODE_TEXT, sc, ec);
  e->data = (unsigned char *)subj->mem->realloc(NULL, s.len + 1);
  if (s.data != NULL) {
    memcpy(e->data, s.data, s.len);
  }
  e->data[s.len] = 0;
  e->len = s.len;
  return e;
}

static cmark_node *make_str_from_buf(subject *subj, int sc, int ec,
                                     cmark_strbuf *buf) {
  cmark_node *e = make_literal(subj, CMARK_NODE_TEXT, sc, ec);
  e->len = buf->size;
  e->data = cmark_strbuf_detach(buf);
  return e;
}

// Like make_str, but parses entities.
static cmark_node *make_str_with_entities(subject *subj,
                                          int start_column, int end_column,
                                          cmark_chunk *content) {
  cmark_strbuf unescaped = CMARK_BUF_INIT(subj->mem);

  if (houdini_unescape_html(&unescaped, content->data, content->len)) {
    return make_str_from_buf(subj, start_column, end_column, &unescaped);
  } else {
    return make_str(subj, start_column, end_column, *content);
  }
}

// Like cmark_node_append_child but without costly sanity checks.
// Assumes that child was newly created.
static void append_child(cmark_node *node, cmark_node *child) {
  cmark_node *old_last_child = node->last_child;

  child->next = NULL;
  child->prev = old_last_child;
  child->parent = node;
  node->last_child = child;

  if (old_last_child) {
    old_last_child->next = child;
  } else {
    // Also set first_child if node previously had no children.
    node->first_child = child;
  }
}

// Duplicate a chunk by creating a copy of the buffer not by reusing the
// buffer like cmark_chunk_dup does.
static unsigned char *cmark_strdup(cmark_mem *mem, unsigned char *src) {
  if (src == NULL) {
    return NULL;
  }
  size_t len = strlen((char *)src);
  unsigned char *data = (unsigned char *)mem->realloc(NULL, len + 1);
  memcpy(data, src, len + 1);
  return data;
}

static unsigned char *cmark_clean_autolink(cmark_mem *mem, cmark_chunk *url,
                                           int is_email) {
  cmark_strbuf buf = CMARK_BUF_INIT(mem);

  cmark_chunk_trim(url);

  if (is_email)
    cmark_strbuf_puts(&buf, "mailto:");

  houdini_unescape_html_f(&buf, url->data, url->len);
  return cmark_strbuf_detach(&buf);
}

static inline cmark_node *make_autolink(subject *subj, int start_column,
                                        int end_column, cmark_chunk url,
                                        int is_email) {
  cmark_node *link = make_simple(subj->mem, CMARK_NODE_LINK);
  link->as.link.url = cmark_clean_autolink(subj->mem, &url, is_email);
  link->as.link.title = NULL;
  link->as.link.width = 0;
  link->as.link.height = 0;
  subj_line_column_at(subj, start_column, start_column,
                      &link->start_line, &link->start_column);
  subj_line_column_at(subj, start_column, end_column,
                      &link->end_line, &link->end_column);
  subj_line_column_at(subj, start_column, start_column + 1,
                      &link->as.link.url_start_line, &link->as.link.url_start_column);
  subj_line_column_at(subj, start_column, end_column - 1,
                      &link->as.link.url_end_line, &link->as.link.url_end_column);
  cmark_node_set_syntax(link, subj->source, subj->input.data, subj->input.len,
                        start_column, start_column + 1, end_column, end_column + 1);
  append_child(link, make_str_with_entities(subj, start_column + 1, end_column - 1, &url));
  return link;
}

static void subject_from_buf(cmark_mem *mem, int line_number, int block_offset, subject *e,
                             cmark_chunk *chunk, cmark_reference_map *refmap,
                             cmark_footnote_map *footnote_map) {
  int i;
  e->mem = mem;
  e->input = *chunk;
  e->flags = 0;
  e->line = line_number;
  e->pos = 0;
  e->block_offset = block_offset;
  e->source = NULL;
  e->column_offset = 0;
  e->refmap = refmap;
  e->footnote_map = footnote_map;
  e->last_delim = NULL;
  e->last_bracket = NULL;
  for (i = 0; i <= MAXBACKTICKS; i++) {
    e->backticks[i] = 0;
  }
  e->scanned_for_backticks = false;
  e->no_link_openers = true;
  e->unescape_table_pipes = false;
  e->dollar_scan_end = 0;
  e->dollar_scan_has_closer = false;
}

static inline int isbacktick(int c) { return (c == '`'); }

static inline unsigned char peek_char(subject *subj) {
  // NULL bytes should have been stripped out by now.  If they're
  // present, it's a programming error:
  assert(!(subj->pos < subj->input.len && subj->input.data[subj->pos] == 0));
  return (subj->pos < subj->input.len) ? subj->input.data[subj->pos] : 0;
}

static inline unsigned char peek_at(subject *subj, bufsize_t pos) {
  return subj->input.data[pos];
}

// Return true if there are more characters in the subject.
static inline int is_eof(subject *subj) {
  return (subj->pos >= subj->input.len);
}

// Advance the subject.  Doesn't check for eof.
#define advance(subj) (subj)->pos += 1

static inline bool skip_spaces(subject *subj) {
  bool skipped = false;
  while (peek_char(subj) == ' ' || peek_char(subj) == '\t') {
    advance(subj);
    skipped = true;
  }
  return skipped;
}

static inline bool skip_line_end(subject *subj) {
  bool seen_line_end_char = false;
  if (peek_char(subj) == '\r') {
    advance(subj);
    seen_line_end_char = true;
  }
  if (peek_char(subj) == '\n') {
    advance(subj);
    seen_line_end_char = true;
  }
  return seen_line_end_char || is_eof(subj);
}

// Take characters while a predicate holds, and return a string.
static inline cmark_chunk take_while(subject *subj, int (*f)(int)) {
  unsigned char c;
  bufsize_t startpos = subj->pos;
  bufsize_t len = 0;

  while ((c = peek_char(subj)) && (*f)(c)) {
    advance(subj);
    len++;
  }

  return cmark_chunk_dup(&subj->input, startpos, len);
}

// Return the number of newlines in a given span of text in a subject.  If
// the number is greater than zero, also return the number of characters
// between the last newline and the end of the span in `since_newline`.
static int count_newlines(subject *subj, bufsize_t from, bufsize_t len, int *since_newline) {
  int nls = 0;
  int since_nl = 0;

  while (len--) {
    if (subj->input.data[from++] == '\n') {
      ++nls;
      since_nl = 0;
    } else {
      ++since_nl;
    }
  }

  if (!nls)
    return 0;

  *since_newline = since_nl;
  return nls;
}

// Adjust `node`'s `end_line`, `end_column`, and `subj`'s `line` and
// `column_offset` according to the number of newlines in a just-matched span
// of text in `subj`.
//
// `matchlen` bytes starting at `subj->pos - matchlen - extra` are scanned for
// newlines; `extra` is the width of the CLOSING delimiter, which sits after
// that range (the closing backtick run of a code span, the `>` of an inline
// HTML tag). `since_newline` is therefore the 0-based column, within the last
// line's stripped content, of the first byte of that closing delimiter, and the
// node's inclusive end column is `since_newline + extra` -- plus `block_offset`,
// the width of the container prefix the block layer stripped, which every
// column this parser reports includes (see `subj_line_column_at()`).
//
// VNoteX: this is unconditional on purpose. It performs position bookkeeping
// only (`subj->line`, `subj->column_offset`, `node->end_line`,
// `node->end_column`); it emits nothing and takes no part in token
// recognition. Gating it on CMARK_OPT_SOURCEPOS -- a *renderer* option --
// contradicted the contract in cmark.h ("recorded during parsing regardless of
// CMARK_OPT_SOURCEPOS") and left `subj->line` stale for every inline node
// following a multiline code span or a multiline inline HTML tag.
static void adjust_subj_node_newlines(subject *subj, cmark_node *node, int matchlen, int extra) {
  int since_newline;
  int newlines = count_newlines(subj, subj->pos - matchlen - extra, matchlen, &since_newline);
  if (newlines) {
    subj->line += newlines;
    node->end_line += newlines;
    node->end_column = since_newline + extra + subj->block_offset;
    subj->column_offset = -subj->pos + since_newline + extra;
  }
}

// Compute the 1-based source line and column of the byte at `off` in
// `subj->input`, given that `subj->line` and `subj->column_offset` are accurate
// for byte offset `from` (with `from <= off`).
//
// When the span crosses one or more newlines the byte lies on a continuation
// line, whose column is measured from the start of that line's content plus
// `block_offset` -- the width of the container prefix the block layer stripped.
// That matches what the enclosing paragraph reports for the same position, and
// what a consumer mapping these coordinates back onto the original text needs.
static void subj_line_column_at(subject *subj, bufsize_t from, bufsize_t off,
                                int *p_line, int *p_column) {
  int since_newline = 0;
  // `off` may sit just before `from` (a shortcut reference link rewinds
  // `subj->pos` to the byte after `]`); such a byte is necessarily on the
  // reference line, so the plain formula applies.
  int newlines = off > from
                     ? count_newlines(subj, from, off - from, &since_newline)
                     : 0;
  if (newlines) {
    *p_line = subj->line + newlines;
    *p_column = since_newline + 1 + subj->block_offset;
  } else {
    *p_line = subj->line;
    *p_column = (int)off + 1 + subj->column_offset + subj->block_offset;
  }
}

// Bring `subj`'s `line` and `column_offset` up to date after a span of input in
// [`from`, `subj->pos`) was consumed by a scanner rather than by
// `parse_inline()` (which is what maintains them for ordinary text).
static void subj_catch_up_newlines(subject *subj, bufsize_t from) {
  int since_newline = 0;
  int newlines = subj->pos > from
                     ? count_newlines(subj, from, subj->pos - from, &since_newline)
                     : 0;
  if (newlines) {
    subj->line += newlines;
    subj->column_offset = -subj->pos + since_newline;
  }
}

// Try to process a backtick code span that began with a
// span of ticks of length openticklength length (already
// parsed).  Return 0 if you don't find matching closing
// backticks, otherwise return the position in the subject
// after the closing backticks.
static bufsize_t scan_to_closing_backticks(subject *subj,
                                           bufsize_t openticklength) {

  bool found = false;
  if (openticklength > MAXBACKTICKS) {
    // we limit backtick string length because of the array subj->backticks:
    return 0;
  }
  if (subj->scanned_for_backticks &&
      subj->backticks[openticklength] <= subj->pos) {
    // return if we already know there's no closer
    return 0;
  }
  while (!found) {
    // read non backticks
    unsigned char c;
    while ((c = peek_char(subj)) && c != '`') {
      advance(subj);
    }
    if (is_eof(subj)) {
      break;
    }
    bufsize_t numticks = 0;
    while (peek_char(subj) == '`') {
      advance(subj);
      numticks++;
    }
    // store position of ender
    if (numticks <= MAXBACKTICKS) {
      subj->backticks[numticks] = subj->pos - numticks;
    }
    if (numticks == openticklength) {
      return (subj->pos);
    }
  }
  // got through whole input without finding closer
  subj->scanned_for_backticks = true;
  return 0;
}

static bool math_space_at(subject *subj, bufsize_t pos) {
  if (pos >= subj->input.len)
    return false;
  unsigned char c = subj->input.data[pos];
  if (c < 0x80)
    return cmark_isspace(c) != 0;
  int32_t cp;
  int bytes = cmark_utf8proc_iterate(subj->input.data + pos,
                                     subj->input.len - pos, &cp);
  return bytes > 0 && (cmark_utf8proc_is_space(cp) || cp == 0xfeff);
}

static bufsize_t scan_viewer_closing_dollar(subject *subj,
                                            bufsize_t fence_length) {
  bufsize_t start = subj->pos;
  const unsigned char *data = subj->input.data;
  if (fence_length == 2) {
    // The configured double-dollar expression is [^$]*?[^\\], followed by
    // $$. A third dollar may therefore be the final content character.
    bufsize_t pos = start;
    while (pos < subj->input.len && data[pos] != '$' &&
           !S_is_line_end_char(data[pos]))
      ++pos;
    if (pos == subj->input.len || S_is_line_end_char(data[pos]))
      return 0;
    if (pos > start && pos + 1 < subj->input.len && data[pos + 1] == '$' &&
        data[pos - 1] != '\\') {
      subj->pos = pos + 2;
      return subj->pos;
    }
    if (pos + 2 < subj->input.len && data[pos + 1] == '$' &&
        data[pos + 2] == '$') {
      subj->pos = pos + 3;
      return subj->pos;
    }
    return 0;
  }

  // Single-dollar content may contain rejected dollar candidates. Cache the
  // next usable closer (or line end), so repeated malformed openers and digit
  // postcondition failures never rescan the same line quadratically.
  if (start < subj->dollar_scan_end) {
    if (!subj->dollar_scan_has_closer)
      return 0;
    subj->pos = subj->dollar_scan_end + 1;
    return subj->pos;
  }
  bufsize_t pos = start + 1;
  for (; pos < subj->input.len; ++pos) {
    if (S_is_line_end_char(data[pos]) ||
        (pos + 2 < subj->input.len && data[pos] == 0xe2 &&
         data[pos + 1] == 0x80 &&
         (data[pos + 2] == 0xa8 || data[pos + 2] == 0xa9)))
      break;
    if (data[pos] != '$' || data[pos - 1] == '\\')
      continue;
    bufsize_t previous = pos - 1;
    while (previous > start && (data[previous] & 0xc0) == 0x80)
      --previous;
    if (math_space_at(subj, previous))
      continue;
    subj->dollar_scan_end = pos;
    subj->dollar_scan_has_closer = true;
    subj->pos = pos + 1;
    return subj->pos;
  }
  subj->dollar_scan_end = pos;
  subj->dollar_scan_has_closer = false;
  return 0;
}

// The default native grammar stops at the first unescaped dollar.
static bufsize_t scan_to_closing_dollar(subject *subj, bufsize_t fence_length,
                                        int options) {
  if (options & CMARK_OPT_VIEWER_COMPAT)
    return scan_viewer_closing_dollar(subj, fence_length);
  bufsize_t startpos = subj->pos;

  // read non dollar
  unsigned char c;
  size_t slash_cnt = 0;
  // Directly skip the escaped dollar.
  while ((c = peek_char(subj)) && (c != '$' || slash_cnt % 2 == 1)) {
    if (c == '\\') {
      ++slash_cnt;
    } else {
      slash_cnt = 0;
    }
    if (c == '\r' || c == '\n') {
      // Native default inline math does not cross lines.
      break;
    }
    advance(subj);
  }
  if (!is_eof(subj) && c == '$' &&
      (fence_length == 1 ||
       (subj->pos + 1 < subj->input.len &&
        peek_at(subj, subj->pos + 1) == '$'))) {
    subj->pos += fence_length;
    return subj->pos;
  } else {
    // Rewind it.
    subj->pos = startpos;
    return 0;
  }
}

// Destructively modify string, converting newlines to
// spaces, then removing a single leading + trailing space,
// unless the code span consists entirely of space characters.
static void S_normalize_code(cmark_strbuf *s) {
  bufsize_t r, w;
  bool contains_nonspace = false;

  for (r = 0, w = 0; r < s->size; ++r) {
    switch (s->ptr[r]) {
    case '\r':
      if (s->ptr[r + 1] != '\n') {
        s->ptr[w++] = ' ';
      }
      break;
    case '\n':
      s->ptr[w++] = ' ';
      break;
    default:
      s->ptr[w++] = s->ptr[r];
    }
    if (s->ptr[r] != ' ') {
      contains_nonspace = true;
    }
  }

  // begins and ends with space?
  if (contains_nonspace &&
      s->ptr[0] == ' ' && s->ptr[w - 1] == ' ') {
    cmark_strbuf_drop(s, 1);
    cmark_strbuf_truncate(s, w - 2);
  } else {
    cmark_strbuf_truncate(s, w);
  }

}


// Parse backtick code section or raw backticks, return an inline.
// Assumes that the subject has a backtick at the current position.
static cmark_node *handle_backticks(subject *subj) {
  bufsize_t initpos = subj->pos;
  cmark_chunk openticks = take_while(subj, isbacktick);
  bufsize_t startpos = subj->pos;
  bufsize_t endpos = scan_to_closing_backticks(subj, openticks.len);

  if (endpos == 0) {      // not found
    subj->pos = startpos; // rewind
    return make_str(subj, initpos, initpos + openticks.len - 1, openticks);
  } else {
    cmark_strbuf buf = CMARK_BUF_INIT(subj->mem);

    cmark_strbuf_set(&buf, subj->input.data + startpos,
                     endpos - startpos - openticks.len);
    S_normalize_code(&buf);
    if (subj->unescape_table_pipes)
      cmark_strbuf_unescape_char(&buf, '|');

    // VNoteX: let's fix it to include the ticks.
    cmark_node *node = make_literal(subj, CMARK_NODE_CODE, startpos - openticks.len,
                                    endpos - 1);
    node->len = buf.size;
    node->data = cmark_strbuf_detach(&buf);
    cmark_node_set_syntax(node, subj->source, subj->input.data, subj->input.len,
                          initpos, startpos, endpos - openticks.len, endpos);
    adjust_subj_node_newlines(subj, node, endpos - startpos, openticks.len);
    return node;
  }
}


// Parse dollar inline formula section or raw dollar, return an inline.
// Assumes that the subject has a dollar at the current position.
static cmark_node *handle_dollar(subject *subj, cmark_node *parent,
                                 int options) {
  bufsize_t initpos = subj->pos;
  // Prefer a double-dollar opener before applying the single-dollar rule.
  cmark_chunk opener =
      initpos + 1 < subj->input.len && peek_at(subj, initpos + 1) == '$'
          ? cmark_chunk_literal("$$")
          : cmark_chunk_literal("$");
  bufsize_t fence_length = opener.len;
  subj->pos += fence_length;
  bufsize_t startpos = subj->pos;

  // Pre check.
  if (initpos > 0) {
    unsigned char before_char = peek_at(subj, initpos - 1);
    if ((fence_length == 1 && before_char == '$' &&
         !(options & CMARK_OPT_VIEWER_COMPAT)) ||
        (before_char >= '0' && before_char <= '9') || before_char == '\\') {
      // The configured rules fail one character at a time, allowing a second
      // dollar to begin a single-dollar expression after a rejected opener.
      if (options & CMARK_OPT_VIEWER_COMPAT) {
        subj->pos = initpos + 1;
        return make_str(subj, initpos, initpos, cmark_chunk_literal("$"));
      }
      return make_str(subj, initpos, startpos - 1, opener);
    }
  }

try_match:
  if ((options & CMARK_OPT_VIEWER_COMPAT) && fence_length == 1 &&
      math_space_at(subj, startpos))
    return make_str(subj, initpos, startpos - 1, opener);

  bufsize_t endpos = scan_to_closing_dollar(subj, fence_length, options);
  if (endpos == 0 || endpos - startpos == fence_length)
    goto no_match;

  // Double-dollar content may end in whitespace; single-dollar rules stay
  // unchanged. Neither form accepts a digit immediately after its closer.
  unsigned char before_char = peek_at(subj, endpos - fence_length - 1);
  unsigned char after_char =
      endpos < subj->input.len ? peek_at(subj, endpos) : 0;
  if ((fence_length == 1 && (before_char == ' ' || before_char == '\t')) ||
      ((options & CMARK_OPT_VIEWER_COMPAT) && before_char == '\\') ||
      (after_char >= '0' && after_char <= '9'))
    goto no_match;

  cmark_strbuf buf = CMARK_BUF_INIT(subj->mem);

  cmark_strbuf_set(&buf, subj->input.data + startpos,
                   endpos - startpos - fence_length);
  if (!(options & CMARK_OPT_VIEWER_COMPAT))
    cmark_strbuf_unescape_char(&buf, '$');
  if (subj->unescape_table_pipes)
    cmark_strbuf_unescape_char(&buf, '|');

  cmark_node *node = make_literal(subj, CMARK_NODE_FORMULA_INLINE, startpos,
                                   endpos - fence_length - 1);
  cmark_node_set_syntax(node, subj->source, subj->input.data, subj->input.len,
                        initpos, startpos, endpos - fence_length, endpos);
  node->as.formula.fence_length = (uint8_t)fence_length;
  node->as.formula.viewer = (options & CMARK_OPT_VIEWER_COMPAT) != 0;
  node->len = buf.size;
  node->data = cmark_strbuf_detach(&buf);
  if (options & CMARK_OPT_VIEWER_COMPAT) {
    subj_line_column_at(subj, initpos, endpos - fence_length - 1,
                        &node->end_line, &node->end_column);
    subj_catch_up_newlines(subj, initpos);
    if (fence_length == 2 && parent->type == CMARK_NODE_PARAGRAPH) {
      bufsize_t line_start = initpos, tag = endpos;
      while (line_start > 0 && (subj->input.data[line_start - 1] == ' ' ||
                                subj->input.data[line_start - 1] == '\t'))
        --line_start;
      if (line_start == 0 ||
          S_is_line_end_char(subj->input.data[line_start - 1])) {
        while (tag < subj->input.len &&
               (subj->input.data[tag] == ' ' || subj->input.data[tag] == '\t'))
          ++tag;
        if (tag < subj->input.len && subj->input.data[tag] == '(') {
          bufsize_t label = ++tag;
          while (tag < subj->input.len && subj->input.data[tag] != ')' &&
                 !math_space_at(subj, tag))
            ++tag;
          if (tag > label && tag < subj->input.len &&
              subj->input.data[tag] == ')') {
            bufsize_t label_end = tag++;
            while (tag < subj->input.len && (subj->input.data[tag] == ' ' ||
                                             subj->input.data[tag] == '\t'))
              ++tag;
            if (tag == subj->input.len ||
                S_is_line_end_char(subj->input.data[tag])) {
              node->as.formula.label =
                  (unsigned char *)subj->mem->calloc(label_end - label + 1, 1);
              memcpy(node->as.formula.label, subj->input.data + label,
                     label_end - label);
              subj->pos = tag;
            }
          }
        }
      }
    }
  }
  return node;

no_match:
  if ((options & CMARK_OPT_VIEWER_COMPAT) && fence_length == 2) {
    fence_length = 1;
    opener = cmark_chunk_literal("$");
    subj->pos = startpos = initpos + 1;
    goto try_match;
  }
  subj->pos = startpos;
  return make_str(subj, initpos, startpos - 1, opener);
}

// Scan ***, **, or * and return number scanned, or 0.
// Advances position.
static int scan_delims(subject *subj, unsigned char c, bool *can_open,
                       bool *can_close) {
  int numdelims = 0;
  bufsize_t before_char_pos;
  int32_t after_char = 0;
  int32_t before_char = 0;
  int len;
  bool left_flanking, right_flanking;

  if (subj->pos == 0) {
    before_char = 10;
  } else {
    before_char_pos = subj->pos - 1;
    // walk back to the beginning of the UTF_8 sequence:
    while (peek_at(subj, before_char_pos) >> 6 == 2 && before_char_pos > 0) {
      before_char_pos -= 1;
    }
    len = cmark_utf8proc_iterate(subj->input.data + before_char_pos,
                                 subj->pos - before_char_pos, &before_char);
    if (len == -1) {
      before_char = 10;
    }
  }

  if (c == '\'' || c == '"') {
    numdelims++;
    advance(subj); // limit to 1 delim for quotes
  } else {
    while (peek_char(subj) == c) {
      numdelims++;
      advance(subj);
    }
  }

  len = cmark_utf8proc_iterate(subj->input.data + subj->pos,
                               subj->input.len - subj->pos, &after_char);
  if (len == -1) {
    after_char = 10;
  }
  left_flanking = numdelims > 0 && !cmark_utf8proc_is_space(after_char) &&
                  (!cmark_utf8proc_is_punctuation_or_symbol(after_char) ||
                   cmark_utf8proc_is_space(before_char) ||
                   cmark_utf8proc_is_punctuation_or_symbol(before_char));
  right_flanking = numdelims > 0 && !cmark_utf8proc_is_space(before_char) &&
                   (!cmark_utf8proc_is_punctuation_or_symbol(before_char) ||
                    cmark_utf8proc_is_space(after_char) ||
                    cmark_utf8proc_is_punctuation_or_symbol(after_char));
  if (c == '_') {
    *can_open = left_flanking &&
                (!right_flanking ||
                 cmark_utf8proc_is_punctuation_or_symbol(before_char));
    *can_close = right_flanking &&
                 (!left_flanking ||
                  cmark_utf8proc_is_punctuation_or_symbol(after_char));
  } else if (c == '\'' || c == '"') {
    *can_open = left_flanking &&
         (!right_flanking || before_char == '(' || before_char == '[') &&
         before_char != ']' && before_char != ')';
    *can_close = right_flanking;
  } else {
    *can_open = left_flanking;
    *can_close = right_flanking;
  }
  return numdelims;
}

/*
static void print_delimiters(subject *subj)
{
        delimiter *delim;
        delim = subj->last_delim;
        while (delim != NULL) {
                printf("Item at stack pos %p: %d %d %d next(%p) prev(%p)\n",
                       (void*)delim, delim->delim_char,
                       delim->can_open, delim->can_close,
                       (void*)delim->next, (void*)delim->previous);
                delim = delim->previous;
        }
}
*/

static void remove_delimiter(subject *subj, delimiter *delim) {
  if (delim == NULL)
    return;
  if (delim->next == NULL) {
    // end of list:
    assert(delim == subj->last_delim);
    subj->last_delim = delim->previous;
  } else {
    delim->next->previous = delim->previous;
  }
  if (delim->previous != NULL) {
    delim->previous->next = delim->next;
  }
  subj->mem->free(delim);
}

static void pop_bracket(subject *subj) {
  bracket *b;
  if (subj->last_bracket == NULL)
    return;
  b = subj->last_bracket;
  subj->last_bracket = subj->last_bracket->previous;
  subj->mem->free(b);
}

static void push_delimiter(subject *subj, unsigned char c, bool can_open,
                           bool can_close, cmark_node *inl_text) {
  delimiter *delim = (delimiter *)subj->mem->calloc(1, sizeof(delimiter));
  delim->delim_char = c;
  delim->can_open = can_open;
  delim->can_close = can_close;
  delim->inl_text = inl_text;
  delim->position = subj->pos;
  delim->length = inl_text->len;
  delim->previous = subj->last_delim;
  delim->next = NULL;
  if (delim->previous != NULL) {
    delim->previous->next = delim;
  }
  subj->last_delim = delim;
}

static void push_bracket(subject *subj, bool image, cmark_node *inl_text) {
  bracket *b = (bracket *)subj->mem->calloc(1, sizeof(bracket));
  if (subj->last_bracket != NULL) {
    subj->last_bracket->bracket_after = true;
  }
  b->image = image;
  b->active = true;
  b->inl_text = inl_text;
  b->previous = subj->last_bracket;
  b->position = subj->pos;
  b->bracket_after = false;
  b->inline_note = false;
  subj->last_bracket = b;
  if (!image) {
    subj->no_link_openers = false;
  }
}

// Assumes the subject has a c at the current position.
static cmark_node *handle_delim(subject *subj, unsigned char c, bool smart) {
  bufsize_t numdelims;
  cmark_node *inl_text;
  bool can_open, can_close;
  cmark_chunk contents;

  numdelims = scan_delims(subj, c, &can_open, &can_close);

  if (c == '\'' && smart) {
    contents = cmark_chunk_literal(RIGHTSINGLEQUOTE);
  } else if (c == '"' && smart) {
    contents =
        cmark_chunk_literal(can_close ? RIGHTDOUBLEQUOTE : LEFTDOUBLEQUOTE);
  } else {
    contents = cmark_chunk_dup(&subj->input, subj->pos - numdelims, numdelims);
  }

  inl_text = make_str(subj, subj->pos - numdelims, subj->pos - 1, contents);

  if ((can_open || can_close) && (!(c == '\'' || c == '"') || smart)) {
    push_delimiter(subj, c, can_open, can_close, inl_text);
  }

  return inl_text;
}

// Assumes we have a hyphen at the current position.
static cmark_node *handle_hyphen(subject *subj, bool smart) {
  int startpos = subj->pos;

  advance(subj);

  if (!smart || peek_char(subj) != '-') {
    return make_str(subj, subj->pos - 1, subj->pos - 1, cmark_chunk_literal("-"));
  }

  while (smart && peek_char(subj) == '-') {
    advance(subj);
  }

  int numhyphens = subj->pos - startpos;
  int en_count = 0;
  int em_count = 0;
  int i;
  cmark_strbuf buf = CMARK_BUF_INIT(subj->mem);

  if (numhyphens % 3 == 0) { // if divisible by 3, use all em dashes
    em_count = numhyphens / 3;
  } else if (numhyphens % 2 == 0) { // if divisible by 2, use all en dashes
    en_count = numhyphens / 2;
  } else if (numhyphens % 3 == 2) { // use one en dash at end
    en_count = 1;
    em_count = (numhyphens - 2) / 3;
  } else { // use two en dashes at the end
    en_count = 2;
    em_count = (numhyphens - 4) / 3;
  }

  for (i = em_count; i > 0; i--) {
    cmark_strbuf_puts(&buf, EMDASH);
  }

  for (i = en_count; i > 0; i--) {
    cmark_strbuf_puts(&buf, ENDASH);
  }

  return make_str_from_buf(subj, startpos, subj->pos - 1, &buf);
}

// Assumes we have a period at the current position.
static cmark_node *handle_period(subject *subj, bool smart) {
  advance(subj);
  if (smart && peek_char(subj) == '.') {
    advance(subj);
    if (peek_char(subj) == '.') {
      advance(subj);
      return make_str(subj, subj->pos - 3, subj->pos - 1, cmark_chunk_literal(ELLIPSES));
    } else {
      return make_str(subj, subj->pos - 2, subj->pos - 1, cmark_chunk_literal(".."));
    }
  } else {
    return make_str(subj, subj->pos - 1, subj->pos - 1, cmark_chunk_literal("."));
  }
}

static void process_emphasis(subject *subj, bufsize_t stack_bottom) {
  delimiter *candidate;
  delimiter *closer = NULL;
  delimiter *opener;
  delimiter *old_closer;
  bool opener_found;
  int openers_bottom_index = 0;
  bufsize_t openers_bottom[21] = {stack_bottom, stack_bottom, stack_bottom,
                                  stack_bottom, stack_bottom, stack_bottom,
                                  stack_bottom, stack_bottom, stack_bottom,
                                  stack_bottom, stack_bottom, stack_bottom,
                                  stack_bottom, stack_bottom, stack_bottom,
                                  stack_bottom, stack_bottom, stack_bottom,
                                  stack_bottom, stack_bottom, stack_bottom};

  // move back to first relevant delim.
  candidate = subj->last_delim;
  while (candidate != NULL && candidate->position >= stack_bottom) {
    closer = candidate;
    candidate = candidate->previous;
  }

  // now move forward, looking for closers, and handling each
  while (closer != NULL) {
    if (closer->can_close) {
      switch (closer->delim_char) {
      case '"':
        openers_bottom_index = 0;
        break;
      case '\'':
        openers_bottom_index = 1;
        break;
      case '_':
        openers_bottom_index = 2 +
                (closer->can_open ? 3 : 0) + (closer->length % 3);
        break;
      case '*':
        openers_bottom_index = 8 +
                (closer->can_open ? 3 : 0) + (closer->length % 3);
        break;
      case '~':
        openers_bottom_index = 14 +
                (closer->can_open ? 2 : 0) + (closer->length % 2);
        break;
      case '=':
        openers_bottom_index = 17 +
                (closer->can_open ? 2 : 0) + (closer->length % 2);
        break;
      default:
        assert(false);
      }

      // Now look backwards for first matching opener:
      opener = closer->previous;
      opener_found = false;
      while (opener != NULL &&
             opener->position >= openers_bottom[openers_bottom_index]) {
        if (opener->can_open && opener->delim_char == closer->delim_char) {
          // interior closer of size 2 can't match opener of size 1
          // or of size 1 can't match 2
          if (!(closer->can_open || opener->can_close) ||
              closer->length % 3 == 0 ||
              (opener->length + closer->length) % 3 != 0) {
            opener_found = true;
            break;
          }
        }
        opener = opener->previous;
      }
      old_closer = closer;
      if (closer->delim_char == '*' || closer->delim_char == '_') {
        if (opener_found) {
          closer = S_insert_emph_like_inline(subj, opener, closer);
        } else {
          closer = closer->next;
        }
      } else if (closer->delim_char == '~' || closer->delim_char == '=') {
        if (opener_found && opener->length >= 2 && closer->length >= 2) {
          closer = S_insert_emph_like_inline(subj, opener, closer);
        } else {
          closer = closer->next;
        }
      } else if (closer->delim_char == '\'' || closer->delim_char == '"') {
        if (closer->delim_char == '\'') {
          cmark_node_set_literal(closer->inl_text, RIGHTSINGLEQUOTE);
        } else {
          cmark_node_set_literal(closer->inl_text, RIGHTDOUBLEQUOTE);
        }
        closer = closer->next;
        if (opener_found) {
          if (old_closer->delim_char == '\'') {
            cmark_node_set_literal(opener->inl_text, LEFTSINGLEQUOTE);
          } else {
            cmark_node_set_literal(opener->inl_text, LEFTDOUBLEQUOTE);
          }
          remove_delimiter(subj, opener);
          remove_delimiter(subj, old_closer);
        }
      }
      if (!opener_found) {
        // set lower bound for future searches for openers
        openers_bottom[openers_bottom_index] = old_closer->position;
        if (!old_closer->can_open) {
          // we can remove a closer that can't be an
          // opener, once we've seen there's no
          // matching opener:
          remove_delimiter(subj, old_closer);
        }
      }
    } else {
      closer = closer->next;
    }
  }
  // free all delimiters in list until stack_bottom:
  while (subj->last_delim != NULL &&
         subj->last_delim->position >= stack_bottom) {
    remove_delimiter(subj, subj->last_delim);
  }
}

static delimiter *S_insert_emph_like_inline(subject *subj, delimiter *opener,
                                            delimiter *closer) {
  delimiter *delim, *tmp_delim;
  bufsize_t use_delims;
  cmark_node *opener_inl = opener->inl_text;
  cmark_node *closer_inl = closer->inl_text;
  bufsize_t opener_num_chars = opener_inl->len;
  bufsize_t closer_num_chars = closer_inl->len;
  cmark_node *tmp, *tmpnext, *emph;
  const char delim_char = closer->delim_char;

  // calculate the actual number of characters used from this closer
  use_delims = (closer_num_chars >= 2 && opener_num_chars >= 2) ? 2 : 1;

  // remove used characters from associated inlines.
  opener_num_chars -= use_delims;
  closer_num_chars -= use_delims;
  opener_inl->len = opener_num_chars;
  opener_inl->data[opener_num_chars] = 0;
  opener_inl->end_column -= use_delims;
  closer_inl->len = closer_num_chars;
  closer_inl->data[closer_num_chars] = 0;
  closer_inl->start_column += use_delims;

  // free delimiters between opener and closer
  delim = closer->previous;
  while (delim != NULL && delim != opener) {
    tmp_delim = delim->previous;
    remove_delimiter(subj, delim);
    delim = tmp_delim;
  }

  // create new emph or strong, and splice it in to our inlines
  // between the opener and closer
  switch (delim_char) {
  case '~':
    emph = make_simple(subj->mem, CMARK_NODE_STRIKETHROUGH);
    break;
  case '=':
    emph = make_simple(subj->mem, CMARK_NODE_MARK);
    break;
  default:
    emph = use_delims == 1 ? make_emph(subj->mem) : make_strong(subj->mem);
    break;
  }

  bufsize_t syntax_start = opener->position - opener->length + opener_num_chars;
  bufsize_t syntax_end = closer->position - closer_num_chars;
  cmark_node_set_syntax(emph, subj->source, subj->input.data, subj->input.len,
                        syntax_start, syntax_start + use_delims,
                        syntax_end - use_delims, syntax_end);
  tmp = opener_inl->next;
  if (tmp && tmp != closer_inl) {
    emph->first_child = tmp;
    tmp->prev = NULL;

    while (tmp && tmp != closer_inl) {
      tmpnext = tmp->next;
      tmp->parent = emph;
      if (tmpnext == closer_inl) {
        emph->last_child = tmp;
        tmp->next = NULL;
      }
      tmp = tmpnext;
    }
  }

  opener_inl->next = emph;
  closer_inl->prev = emph;
  emph->prev = opener_inl;
  emph->next = closer_inl;
  emph->parent = opener_inl->parent;

  emph->start_line = opener_inl->start_line;
  emph->end_line = closer_inl->end_line;
  emph->start_column = opener_inl->start_column + opener_inl->len;
  emph->end_column = closer_inl->end_column - closer_inl->len;

  // if opener has 0 characters, remove it and its associated inline
  if (opener_num_chars == 0) {
    cmark_node_free(opener_inl);
    remove_delimiter(subj, opener);
  }

  // if closer has 0 characters, remove it and its associated inline
  if (closer_num_chars == 0) {
    // remove empty closer inline
    cmark_node_free(closer_inl);
    // remove closer from list
    tmp_delim = closer->next;
    remove_delimiter(subj, closer);
    closer = tmp_delim;
  }

  return closer;
}

// Parse backslash-escape or just a backslash, returning an inline.
static cmark_node *handle_backslash(subject *subj) {
  advance(subj);
  unsigned char nextchar = peek_char(subj);
  if (cmark_ispunct(
          nextchar)) { // only ascii symbols and newline can be escaped
    advance(subj);
    cmark_node *node =
        make_str(subj, subj->pos - 2, subj->pos - 1,
                 cmark_chunk_dup(&subj->input, subj->pos - 1, 1));
    node->flags |= CMARK_NODE__LITERAL_ESCAPE;
    return node;
  } else if (!is_eof(subj) && skip_line_end(subj)) {
    return make_linebreak(subj->mem);
  } else {
    return make_str(subj, subj->pos - 1, subj->pos - 1, cmark_chunk_literal("\\"));
  }
}

// Parse an entity or a regular "&" string.
// Assumes the subject has an '&' character at the current position.
static cmark_node *handle_entity(subject *subj) {
  cmark_strbuf ent = CMARK_BUF_INIT(subj->mem);
  bufsize_t len;

  advance(subj);

  len = houdini_unescape_ent(&ent, subj->input.data + subj->pos,
                             subj->input.len - subj->pos);

  if (len <= 0)
    return make_str(subj, subj->pos - 1, subj->pos - 1, cmark_chunk_literal("&"));

  subj->pos += len;
  return make_str_from_buf(subj, subj->pos - 1 - len, subj->pos - 1, &ent);
}

// Clean a URL: remove surrounding whitespace, and remove \ that escape
// punctuation.
unsigned char *cmark_clean_url(cmark_mem *mem, cmark_chunk *url) {
  cmark_strbuf buf = CMARK_BUF_INIT(mem);

  cmark_chunk_trim(url);

  houdini_unescape_html_f(&buf, url->data, url->len);

  cmark_strbuf_unescape(&buf);
  return cmark_strbuf_detach(&buf);
}

unsigned char *cmark_clean_title(cmark_mem *mem, cmark_chunk *title) {
  cmark_strbuf buf = CMARK_BUF_INIT(mem);
  unsigned char first, last;

  if (title->len == 0) {
    return NULL;
  }

  first = title->data[0];
  last = title->data[title->len - 1];

  // remove surrounding quotes if any:
  if ((first == '\'' && last == '\'') || (first == '(' && last == ')') ||
      (first == '"' && last == '"')) {
    houdini_unescape_html_f(&buf, title->data + 1, title->len - 2);
  } else {
    houdini_unescape_html_f(&buf, title->data, title->len);
  }

  cmark_strbuf_unescape(&buf);
  return cmark_strbuf_detach(&buf);
}

// Parse an autolink or HTML tag.
// Assumes the subject has a '<' character at the current position.
static cmark_node *handle_pointy_brace(subject *subj, int options) {
  bufsize_t matchlen = 0;
  cmark_chunk contents;

  advance(subj); // advance past first <

  // first try to match a URL autolink
  matchlen = scan_autolink_uri(&subj->input, subj->pos);
  if (matchlen > 0) {
    contents = cmark_chunk_dup(&subj->input, subj->pos, matchlen - 1);
    subj->pos += matchlen;

    return make_autolink(subj, subj->pos - 1 - matchlen, subj->pos - 1, contents, 0);
  }

  // next try to match an email autolink
  matchlen = scan_autolink_email(&subj->input, subj->pos);
  if (matchlen > 0) {
    contents = cmark_chunk_dup(&subj->input, subj->pos, matchlen - 1);
    subj->pos += matchlen;

    return make_autolink(subj, subj->pos - 1 - matchlen, subj->pos - 1, contents, 1);
  }

  if (options & CMARK_OPT_HTML_AS_TEXT)
    return make_str(subj, subj->pos - 1, subj->pos - 1,
                    cmark_chunk_literal("<"));

  // finally, try to match an html tag
  if (subj->pos + 2 <= subj->input.len) {
    int c = subj->input.data[subj->pos];
    if (c == '!' && (subj->flags & FLAG_SKIP_HTML_COMMENT) == 0) {
      c = subj->input.data[subj->pos+1];
      if (c == '-' && subj->input.data[subj->pos+2] == '-') {
	if (subj->pos + 3 < subj->input.len &&
            subj->input.data[subj->pos+3] == '>') {
	  matchlen = 4;
	} else if (subj->pos + 4 < subj->input.len &&
                   subj->input.data[subj->pos+3] == '-' &&
                   subj->input.data[subj->pos+4] == '>') {
          matchlen = 5;
        } else {
          matchlen = scan_html_comment(&subj->input, subj->pos + 1);
          if (matchlen > 0) {
            matchlen += 1; // prefix "<"
	  } else { // no match through end of input: set a flag so
		   // we don't reparse looking for -->:
	    subj->flags |= FLAG_SKIP_HTML_COMMENT;
	  }
	}
      } else if (c == '[') {
        if ((subj->flags & FLAG_SKIP_HTML_CDATA) == 0) {
          matchlen = scan_html_cdata(&subj->input, subj->pos + 2);
          if (matchlen > 0) {
            // The regex doesn't require the final "]]>". But if we're not at
            // the end of input, it must come after the match. Otherwise,
            // disable subsequent scans to avoid quadratic behavior.
            matchlen += 5; // prefix "![", suffix "]]>"
            if (subj->pos + matchlen > subj->input.len) {
              subj->flags |= FLAG_SKIP_HTML_CDATA;
              matchlen = 0;
            }
          }
        }
      } else if ((subj->flags & FLAG_SKIP_HTML_DECLARATION) == 0) {
        matchlen = scan_html_declaration(&subj->input, subj->pos + 1);
        if (matchlen > 0) {
          matchlen += 2; // prefix "!", suffix ">"
          if (subj->pos + matchlen > subj->input.len) {
            subj->flags |= FLAG_SKIP_HTML_DECLARATION;
            matchlen = 0;
          }
        }
      }
    } else if (c == '?') {
      if ((subj->flags & FLAG_SKIP_HTML_PI) == 0) {
        // Note that we allow an empty match.
        matchlen = scan_html_pi(&subj->input, subj->pos + 1);
        matchlen += 3; // prefix "?", suffix "?>"
        if (subj->pos + matchlen > subj->input.len) {
          subj->flags |= FLAG_SKIP_HTML_PI;
          matchlen = 0;
        }
      }
    } else {
      matchlen = scan_html_tag(&subj->input, subj->pos);
    }
  }
  if (matchlen > 0) {
    const unsigned char *src = subj->input.data + subj->pos - 1;
    bufsize_t len = matchlen + 1;
    subj->pos += matchlen;
    cmark_node *node = make_literal(subj, CMARK_NODE_HTML_INLINE,
                                    subj->pos - matchlen - 1, subj->pos - 1);
    node->data = (unsigned char *)subj->mem->realloc(NULL, len + 1);
    memcpy(node->data, src, len);
    node->data[len] = 0;
    node->len = len;
    adjust_subj_node_newlines(subj, node, matchlen, 1);
    return node;
  }

  // if nothing matches, just return the opening <:
  return make_str(subj, subj->pos - 1, subj->pos - 1, cmark_chunk_literal("<"));
}

// Parse a link label.  Returns 1 if successful.
// Note:  unescaped brackets are not allowed in labels.
// The label begins with `[` and ends with the first `]` character
// encountered.  Backticks in labels do not start code spans.
static int link_label(subject *subj, cmark_chunk *raw_label) {
  bufsize_t startpos = subj->pos;
  int length = 0;
  unsigned char c;

  // advance past [
  if (peek_char(subj) == '[') {
    advance(subj);
  } else {
    return 0;
  }

  while ((c = peek_char(subj)) && c != '[' && c != ']') {
    if (c == '\\') {
      advance(subj);
      length++;
      if (cmark_ispunct(peek_char(subj))) {
        advance(subj);
        length++;
      }
    } else {
      advance(subj);
      length++;
    }
    if (length > MAX_LINK_LABEL_LENGTH) {
      goto noMatch;
    }
  }

  if (c == ']') { // match found
    *raw_label =
        cmark_chunk_dup(&subj->input, startpos + 1, subj->pos - (startpos + 1));
    cmark_chunk_trim(raw_label);
    advance(subj); // advance past ]
    return 1;
  }

noMatch:
  subj->pos = startpos; // rewind
  return 0;
}

static bufsize_t manual_scan_link_url_2(cmark_chunk *input, bufsize_t offset,
                                        cmark_chunk *output) {
  bufsize_t i = offset;
  size_t nb_p = 0;

  while (i < input->len) {
    if (input->data[i] == '\\' &&
        i + 1 < input-> len &&
        cmark_ispunct(input->data[i+1]))
      i += 2;
    else if (input->data[i] == '(') {
      ++nb_p;
      ++i;
      if (nb_p > 32)
        return -1;
    } else if (input->data[i] == ')') {
      if (nb_p == 0)
        break;
      --nb_p;
      ++i;
    } else if (cmark_isspace(input->data[i])) {
      if (i == offset) {
        return -1;
      }
      break;
    } else {
      ++i;
    }
  }

  if (i >= input->len || nb_p != 0)
    return -1;

  {
    cmark_chunk result = {input->data + offset, i - offset};
    *output = result;
  }
  return i - offset;
}

static bufsize_t manual_scan_link_url(cmark_chunk *input, bufsize_t offset,
                                      cmark_chunk *output) {
  bufsize_t i = offset;

  if (i < input->len && input->data[i] == '<') {
    ++i;
    while (i < input->len) {
      if (input->data[i] == '>') {
        ++i;
        break;
      } else if (input->data[i] == '\\')
        i += 2;
      else if (input->data[i] == '\n' || input->data[i] == '<')
        return -1;
      else
        ++i;
    }
  } else {
    return manual_scan_link_url_2(input, offset, output);
  }

  if (i >= input->len)
    return -1;

  {
    cmark_chunk result = {input->data + offset + 1, i - 2 - offset};
    *output = result;
  }
  return i - offset;
}

// Viewer dimensions retain percentages without changing the integer API.
static bufsize_t scan_viewer_dimensions(cmark_chunk *input, bufsize_t start,
                                        bufsize_t *split, int *width,
                                        int *height) {
  bufsize_t pos = start;
  if (pos >= input->len || input->data[pos++] != '=')
    return 0;
  for (int dimension = 0; dimension < 2; ++dimension) {
    int value = 0;
    bufsize_t digits = pos;
    while (pos < input->len && cmark_isdigit(input->data[pos])) {
      int digit = input->data[pos++] - '0';
      value = value > (INT_MAX - digit) / 10 ? INT_MAX : value * 10 + digit;
    }
    if (pos > digits && pos < input->len && input->data[pos] == '%') {
      ++pos;
      value = 0; // Percentages have no absolute-pixel integer value.
    }
    if (!dimension) {
      if (pos >= input->len || input->data[pos] != 'x')
        return 0;
      *split = pos++;
      *width = value;
    } else {
      *height = value;
    }
  }
  return pos - start;
}

static unsigned char *copy_span(cmark_mem *mem, const unsigned char *data,
                                bufsize_t len) {
  unsigned char *copy = (unsigned char *)mem->calloc(len + 1, 1);
  memcpy(copy, data, len);
  return copy;
}

// Return a link, an image, or a literal close bracket.
static cmark_node *handle_close_bracket(subject *subj, int options) {
  bufsize_t initial_pos, after_link_text_pos = 0;
  bufsize_t endurl, starttitle, endtitle, endall;
  bufsize_t starturl = 0;
  bool has_url_pos = false;
  bufsize_t sps, n;
  cmark_reference *ref = NULL;
  cmark_chunk url_chunk, title_chunk;
  unsigned char *url, *title;
  int width = 0, height = 0;
  bufsize_t size_start = 0, size_split = 0, size_end = 0;
  bracket *opener;
  cmark_node *inl;
  cmark_chunk raw_label;
  int found_label;
  cmark_node *tmp, *tmpnext;
  bool is_image;

  advance(subj); // advance past ]
  initial_pos = subj->pos;

  // get last [ or ![
  opener = subj->last_bracket;

  if (opener == NULL) {
    return make_str(subj, subj->pos - 1, subj->pos - 1, cmark_chunk_literal("]"));
  }

  if (opener->inline_note) {
    // Create INLINE_FOOTNOTE container
    cmark_node *inl_note = make_simple(subj->mem, CMARK_NODE_INLINE_FOOTNOTE);
    inl_note->as.footnote_ref.number = subj->footnote_map->next_number++;
    inl_note->start_line = inl_note->end_line = subj->line;
    inl_note->start_column = opener->inl_text->start_column;
    inl_note->end_column = subj->pos + subj->column_offset + subj->block_offset;

    cmark_node_insert_before(opener->inl_text, inl_note);
    cmark_node *tmp = opener->inl_text->next;
    while (tmp) {
      cmark_node *tmpnext = tmp->next;
      cmark_node_unlink(tmp);
      append_child(inl_note, tmp);
      tmp = tmpnext;
    }
    cmark_node_free(opener->inl_text);

    process_emphasis(subj, opener->position);
    pop_bracket(subj);
    return NULL;
  }

  // If we got here, we matched a potential link/image text.
  // Now we check to see if it's a link/image.
  is_image = opener->image;

  if (!is_image && subj->no_link_openers) {
    // take delimiter off stack
    pop_bracket(subj);
    return make_str(subj, subj->pos - 1, subj->pos - 1, cmark_chunk_literal("]"));
  }

  after_link_text_pos = subj->pos;

  // First, look for an inline link.
  if (peek_char(subj) == '(' &&
      ((sps = scan_spacechars(&subj->input, subj->pos + 1)) > -1) &&
      ((n = manual_scan_link_url(&subj->input, subj->pos + 1 + sps,
                                 &url_chunk)) > -1)) {

    // try to parse an explicit link:
    starturl = subj->pos + 1 + sps;
    endurl = starturl + n;
    starttitle = endurl + scan_spacechars(&subj->input, endurl);

    // ensure there are spaces btw url and title
    endtitle = (starttitle == endurl)
                   ? starttitle
                   : starttitle + scan_link_title(&subj->input, starttitle);

    if (is_image && (options & CMARK_OPT_NO_EXTENSIONS) == 0) {
      pair pstruct;
      pstruct.first = &width;
      pstruct.second = &height;
      bufsize_t startsize = endtitle + scan_spacechars(&subj->input, endtitle);
    // ensure there are spaces between title (or url if title is empty) and size info
      int has_space = (starttitle == endtitle) ? starttitle > endurl : startsize > endtitle;
      bufsize_t endsize = startsize;
      if (has_space) {
        if (options & CMARK_OPT_VIEWER_COMPAT) {
          bufsize_t size_len = scan_viewer_dimensions(
              &subj->input, startsize, &size_split, &width, &height);
          if (size_len) {
            size_start = startsize + 1;
            size_end = startsize + size_len;
            endsize = size_end;
          }
        } else {
          endsize += scan_image_size(&subj->input, startsize, &pstruct);
        }
      }
      endall = endsize + scan_spacechars(&subj->input, endsize);
    } else {
      endall = endtitle + scan_spacechars(&subj->input, endtitle);
    }

    if (peek_at(subj, endall) == ')') {
      subj->pos = endall + 1;

      title_chunk =
          cmark_chunk_dup(&subj->input, starttitle, endtitle - starttitle);
      url = cmark_clean_url(subj->mem, &url_chunk);
      title = cmark_clean_title(subj->mem, &title_chunk);
      cmark_chunk_free(&url_chunk);
      cmark_chunk_free(&title_chunk);
      // An empty destination (`[a]()`) spans no bytes, so there is no inclusive
      // end byte to report; leave the position unset rather than emit a
      // reversed range. `<>` does span bytes and keeps a position.
      has_url_pos = endurl > starturl;
      goto match;

    } else {
      // it could still be a shortcut reference link
      size_end = 0;
      width = height = 0;
      subj->pos = after_link_text_pos;
    }
  }

  // Next, look for a following [link label] that matches in refmap.
  // skip spaces
  raw_label = cmark_chunk_literal("");
  found_label = link_label(subj, &raw_label);
  if (!found_label) {
    // If we have a shortcut reference link, back up
    // to before the spaces we skipped.
    subj->pos = initial_pos;
  }

  if ((!found_label || raw_label.len == 0) && !opener->bracket_after) {
    cmark_chunk_free(&raw_label);
    raw_label = cmark_chunk_dup(&subj->input, opener->position,
                                initial_pos - opener->position - 1);
    found_label = true;
  }

  if (found_label) {
    ref = cmark_reference_lookup(subj->refmap, &raw_label);
    cmark_chunk_free(&raw_label);
  }

  if (ref != NULL) { // found
    url = cmark_strdup(subj->mem, ref->url);
    title = cmark_strdup(subj->mem, ref->title);
    goto match;
  } else {
    goto noMatch;
  }

noMatch:
  // If we fall through to here, it means we didn't match a link:
  pop_bracket(subj); // remove this opener from delimiter list
  subj->pos = initial_pos;
  return make_str(subj, subj->pos - 1, subj->pos - 1, cmark_chunk_literal("]"));

match:
  inl = make_simple(subj->mem, is_image ? CMARK_NODE_IMAGE : CMARK_NODE_LINK);
  inl->as.link.url = url;
  inl->as.link.title = title;
  inl->as.link.width = width;
  inl->as.link.height = height;
  if (!is_image)
    cmark_node_set_syntax(inl, subj->source, subj->input.data, subj->input.len,
                          opener->position - 1, opener->position,
                          initial_pos - 1, subj->pos);
  if (is_image && size_end) {
    inl->as.link.width_text = copy_span(
        subj->mem, subj->input.data + size_start, size_split - size_start);
    inl->as.link.height_text =
        copy_span(subj->mem, subj->input.data + size_split + 1,
                  size_end - size_split - 1);
  }
  // The link text's newlines were consumed by parse_inline(), so subj->line and
  // subj->column_offset are accurate as of `after_link_text_pos`. Derive every
  // position below from that reference point before mutating the subject.
  if (has_url_pos) {
    subj_line_column_at(subj, after_link_text_pos, starturl,
                        &inl->as.link.url_start_line,
                        &inl->as.link.url_start_column);
    subj_line_column_at(subj, after_link_text_pos, endurl - 1,
                        &inl->as.link.url_end_line,
                        &inl->as.link.url_end_column);
  }
  // Do NOT collapse the start onto the line where parsing finished: a link or
  // image may legally span several lines.
  inl->start_line = opener->inl_text->start_line;
  inl->start_column = opener->inl_text->start_column;
  subj_line_column_at(subj, after_link_text_pos, subj->pos - 1, &inl->end_line,
                      &inl->end_column);
  // The destination, title and reference label are consumed by scanners, not by
  // parse_inline(), so any newlines they contain still have to be accounted for.
  subj_catch_up_newlines(subj, after_link_text_pos);
  cmark_node_insert_before(opener->inl_text, inl);
  // Add link text:
  tmp = opener->inl_text->next;
  while (tmp) {
    tmpnext = tmp->next;
    cmark_node_unlink(tmp);
    append_child(inl, tmp);
    tmp = tmpnext;
  }

  // Free the bracket [:
  cmark_node_free(opener->inl_text);

  process_emphasis(subj, opener->position);
  pop_bracket(subj);

  // Now, if we have a link, we also want to deactivate links until
  // we get a new opener. (This code can be removed if we decide to allow links
  // inside links.)
  if (!is_image) {
    subj->no_link_openers = true;
  }

  return NULL;
}

// Parse a hard or soft linebreak, returning an inline.
// Assumes the subject has a cr or newline at the current position.
static cmark_node *handle_newline(subject *subj) {
  bufsize_t nlpos = subj->pos;
  // skip over cr, crlf, or lf:
  if (peek_at(subj, subj->pos) == '\r') {
    advance(subj);
  }
  if (peek_at(subj, subj->pos) == '\n') {
    advance(subj);
  }
  ++subj->line;
  subj->column_offset = -subj->pos;
  // skip spaces at beginning of line
  skip_spaces(subj);
  if (nlpos > 1 && peek_at(subj, nlpos - 1) == ' ' &&
      peek_at(subj, nlpos - 2) == ' ') {
    return make_linebreak(subj->mem);
  } else {
    return make_softbreak(subj->mem);
  }
}

static bufsize_t subject_find_special_char(subject *subj, int options) {
  // Bit 1: CommonMark syntax; bit 2: fork-only syntax ($, =, ^, ~).
  static const int8_t SPECIAL_CHARS[256] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 1,
      1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  // " ' . -
  static const char SMART_PUNCT_CHARS[] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  };

  bufsize_t n = subj->pos + 1;
  int special_mask = (options & CMARK_OPT_NO_EXTENSIONS) ? 1 : 3;

  while (n < subj->input.len) {
    if (SPECIAL_CHARS[subj->input.data[n]] & special_mask)
      return n;
    if (options & CMARK_OPT_SMART && SMART_PUNCT_CHARS[subj->input.data[n]])
      return n;
    n++;
  }

  return subj->input.len;
}

// Parse a footnote reference [^label].
// Assumes the subject has already consumed '[' and is pointing at '^'.
// Returns a FOOTNOTE_REFERENCE node if label found in footnote_map, else NULL.
static cmark_node *handle_footnote_ref(subject *subj) {
  bufsize_t startpos = subj->pos; // pointing at '^'
  advance(subj); // skip '^'
  bufsize_t label_start = subj->pos;

  // Scan for closing ']'
  while (!is_eof(subj) && peek_char(subj) != ']' &&
         !S_is_line_end_char(peek_char(subj))) {
    advance(subj);
  }

  if (is_eof(subj) || peek_char(subj) != ']' || subj->pos == label_start) {
    subj->pos = startpos; // restore position
    return NULL;
  }

  bufsize_t label_end = subj->pos;
  advance(subj); // skip ']'

  cmark_chunk label = cmark_chunk_dup(&subj->input, label_start,
                                      label_end - label_start);
  cmark_footnote *footnote = cmark_footnote_lookup(subj->footnote_map, &label);
  cmark_chunk_free(&label);

  if (footnote == NULL) {
    subj->pos = startpos; // restore
    return NULL;
  }

  // Assign number if first reference
  if (footnote->number == 0) {
    footnote->number = subj->footnote_map->next_number++;
    footnote->def_node->as.footnote_def.number = footnote->number;
    cmark_footnote_map *map = subj->footnote_map;
    if (map->collect_references) {
      if (map->pending_size == map->pending_capacity) {
        map->pending_capacity =
            map->pending_capacity ? map->pending_capacity * 2 : 8;
        map->pending = (cmark_node **)map->mem->realloc(
            map->pending, map->pending_capacity * sizeof(*map->pending));
      }
      map->pending[map->pending_size++] = footnote->def_node;
    }
  }

  // Create FOOTNOTE_REFERENCE node
  cmark_node *node = make_literal(subj, CMARK_NODE_FOOTNOTE_REFERENCE,
                                  startpos - 1, subj->pos - 1);
  // Store label in node->data/len
  bufsize_t label_len = label_end - label_start;
  node->data = (unsigned char *)subj->mem->realloc(NULL, label_len + 1);
  memcpy(node->data, subj->input.data + label_start, label_len);
  node->data[label_len] = '\0';
  node->len = label_len;
  node->as.footnote_ref.number = footnote->number;

  return node;
}

// Parse a wikilink [[target]] or [[target|display]].
// Assumes the subject has already consumed the first '[' and is pointing at the
// potential second '['.  Returns a WIKILINK node if valid, else NULL.
static cmark_node *handle_wikilink(subject *subj) {
  if (peek_char(subj) != '[') {
    return NULL;
  }

  bufsize_t startpos = subj->pos; // position of second '['
  advance(subj); // consume second '['

  bufsize_t content_start = subj->pos;

  // Scan for closing ]]
  bufsize_t content_end = 0;
  bool found_close = false;
  while (!is_eof(subj)) {
    unsigned char c = peek_char(subj);
    if (S_is_line_end_char(c)) {
      break;
    }
    if (c == ']' && subj->pos + 1 < subj->input.len &&
        subj->input.data[subj->pos + 1] == ']') {
      content_end = subj->pos;
      found_close = true;
      break;
    }
    advance(subj);
  }

  if (!found_close || content_end == content_start) {
    subj->pos = startpos;
    return NULL;
  }

  // Find first '|' to split target from display text
  bufsize_t pipe_pos = 0;
  bool has_pipe = false;
  for (bufsize_t i = content_start; i < content_end; i++) {
    if (subj->input.data[i] == '|') {
      pipe_pos = i;
      has_pipe = true;
      break;
    }
  }

  // Extract target and display text
  const unsigned char *target;
  bufsize_t target_len;
  const unsigned char *display;
  bufsize_t display_len;

  if (has_pipe) {
    target_len = pipe_pos - content_start;
    target = subj->input.data + content_start;
    display = subj->input.data + pipe_pos + 1;
    display_len = content_end - pipe_pos - 1;
  } else {
    target_len = content_end - content_start;
    target = subj->input.data + content_start;
    display = target;
    display_len = target_len;
  }

  // Create WIKILINK node (startpos - 1 is the first '[')
  cmark_node *node = make_literal(subj, CMARK_NODE_WIKILINK,
                                  startpos - 1, content_end + 1);
  node->as.link.url = (unsigned char *)subj->mem->realloc(NULL, target_len + 1);
  memcpy(node->as.link.url, target, target_len);
  node->as.link.url[target_len] = '\0';
  node->as.link.title = NULL;

  // Create child TEXT node for display text
  cmark_node *text = make_literal(subj, CMARK_NODE_TEXT,
                                  content_start, content_end - 1);
  text->data = (unsigned char *)subj->mem->realloc(NULL, display_len + 1);
  memcpy(text->data, display, display_len);
  text->data[display_len] = '\0';
  text->len = display_len;
  append_child(node, text);

  // Advance past ]]
  subj->pos = content_end + 2;

  return node;
}

// Sub/sup plugins contain literal text, not recursively parsed Markdown.
// Backslash-escaped whitespace is allowed; unescaped whitespace is not.
static cmark_node *handle_script(subject *subj, unsigned char marker) {
  bufsize_t start = subj->pos, pos = start + 1;
  if (pos >= subj->input.len || subj->input.data[pos] == marker)
    return NULL;
  while (pos < subj->input.len) {
    unsigned char c = subj->input.data[pos];
    if (c == '\\' && pos + 1 < subj->input.len) {
      pos += 2;
      continue;
    }
    if (c == marker)
      break;
    if (cmark_isspace(c))
      return NULL;
    if (c == '`') {
      bufsize_t ticks = 1, scan, end;
      while (pos + ticks < subj->input.len &&
             subj->input.data[pos + ticks] == '`')
        ++ticks;
      scan = pos + ticks;
      while (scan < subj->input.len) {
        if (subj->input.data[scan] != '`') {
          ++scan;
          continue;
        }
        end = scan;
        while (end < subj->input.len && subj->input.data[end] == '`')
          ++end;
        if (end - scan == ticks) {
          pos = end;
          break;
        }
        scan = end;
      }
      if (scan < subj->input.len)
        continue;
    }
    ++pos;
  }
  if (pos >= subj->input.len || pos == start + 1)
    return NULL;
  cmark_strbuf text = CMARK_BUF_INIT(subj->mem);
  for (bufsize_t i = start + 1; i < pos; ++i) {
    unsigned char c = subj->input.data[i];
    if (cmark_isspace(c)) {
      cmark_strbuf_free(&text);
      return NULL;
    }
    if (c == '\\' && i + 1 < pos &&
        (cmark_ispunct(subj->input.data[i + 1]) ||
         subj->input.data[i + 1] == ' '))
      c = subj->input.data[++i];
    cmark_strbuf_putc(&text, c);
  }
  cmark_node *node = make_literal(
      subj, marker == '~' ? CMARK_NODE_SUBSCRIPT : CMARK_NODE_SUPERSCRIPT,
      start, pos);
  append_child(node, make_str_from_buf(subj, start + 1, pos - 1, &text));
  subj->pos = pos + 1;
  return node;
}

// Parse an inline, advancing subject, and add it as a child of parent.
// Return 0 if no inline can be parsed, 1 otherwise.
static int parse_inline(subject *subj, cmark_node *parent, int options) {
  cmark_node *new_inl = NULL;
  cmark_chunk contents;
  unsigned char c;
  bufsize_t startpos, endpos;
  c = peek_char(subj);
  if (c == 0) {
    return 0;
  }
  switch (c) {
  case '\r':
  case '\n':
    new_inl = handle_newline(subj);
    break;
  case '`':
    new_inl = handle_backticks(subj);
    break;
  case '$':
    if (options & CMARK_OPT_NO_EXTENSIONS)
      goto text;
    new_inl = handle_dollar(subj, parent, options);
    break;
  case '\\':
    new_inl = handle_backslash(subj);
    break;
  case '&':
    new_inl = handle_entity(subj);
    break;
  case '<':
    new_inl = handle_pointy_brace(subj, options);
    break;
  case '~':
    if (!(options & CMARK_OPT_NO_EXTENSIONS) &&
        (options & CMARK_OPT_SUBSCRIPT) &&
        (new_inl = handle_script(subj, '~')) != NULL)
      break;
    // fall through
  case '=':
    if (options & CMARK_OPT_NO_EXTENSIONS)
      goto text;
    // fall through
  case '*':
  case '_':
  case '\'':
  case '"':
    new_inl = handle_delim(subj, c, (options & CMARK_OPT_SMART) != 0);
    break;
  case '-':
    new_inl = handle_hyphen(subj, (options & CMARK_OPT_SMART) != 0);
    break;
  case '.':
    new_inl = handle_period(subj, (options & CMARK_OPT_SMART) != 0);
    break;
  case '[':
    advance(subj);
    if ((options & CMARK_OPT_NO_EXTENSIONS) == 0) {
      // Check for wikilink [[...]]
      new_inl = handle_wikilink(subj);
      if (new_inl != NULL) {
        break;
      }
      if (peek_char(subj) == '^' && subj->footnote_map != NULL) {
        new_inl = handle_footnote_ref(subj);
        if (new_inl != NULL) {
          break;
        }
      }
    }
    new_inl = make_str(subj, subj->pos - 1, subj->pos - 1, cmark_chunk_literal("["));
    push_bracket(subj, false, new_inl);
    break;
  case ']':
    new_inl = handle_close_bracket(subj, options);
    break;
  case '!':
    advance(subj);
    if (peek_char(subj) == '[') {
      advance(subj);
      new_inl = make_str(subj, subj->pos - 2, subj->pos - 1, cmark_chunk_literal("!["));
      push_bracket(subj, true, new_inl);
    } else {
      new_inl = make_str(subj, subj->pos - 1, subj->pos - 1, cmark_chunk_literal("!"));
    }
    break;
  case '^':
    if (options & CMARK_OPT_NO_EXTENSIONS)
      goto text;
    if ((options & CMARK_OPT_SUPERSCRIPT) &&
        (subj->pos + 1 >= subj->input.len ||
         peek_at(subj, subj->pos + 1) != '[') &&
        (new_inl = handle_script(subj, '^')) != NULL)
      break;
    advance(subj);
    if (peek_char(subj) == '[' && subj->footnote_map != NULL) {
      advance(subj); // skip '['
      new_inl = make_str(subj, subj->pos - 2, subj->pos - 1, cmark_chunk_literal("^["));
      push_bracket(subj, false, new_inl);
      subj->last_bracket->inline_note = true;
    } else {
      new_inl = make_str(subj, subj->pos - 1, subj->pos - 1, cmark_chunk_literal("^"));
    }
    break;
  default:
  text:
    endpos = subject_find_special_char(subj, options);
    contents = cmark_chunk_dup(&subj->input, subj->pos, endpos - subj->pos);
    startpos = subj->pos;
    subj->pos = endpos;

    // if we're at a newline, strip trailing spaces.
    if (S_is_line_end_char(peek_char(subj))) {
      cmark_chunk_rtrim(&contents);
    }

    int text_end = endpos - 1;
    if ((options & CMARK_OPT_LINKIFY) && !(options & CMARK_OPT_NO_EXTENSIONS))
      text_end = startpos + contents.len - 1;
    new_inl = make_str(subj, startpos, text_end, contents);
  }
  if (new_inl != NULL) {
    append_child(parent, new_inl);
  }

  return 1;
}

// Linkification runs only after emphasis/link parsing has fixed the tree.
// Its source cursor walks the original text nodes monotonically, so entity
// expansion and punctuation-node joins never manufacture out-of-range spans.
typedef struct {
  cmark_node *node;
  bufsize_t offset;
} linkify_source;

static unsigned char ascii_lower(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static bool ascii_alnum(unsigned char c) {
  return cmark_isalpha(c) || cmark_isdigit(c);
}

static bool ascii_hex(unsigned char c) {
  c = ascii_lower(c);
  return cmark_isdigit(c) || (c >= 'a' && c <= 'f');
}

static bool ascii_equal(const unsigned char *data, bufsize_t len,
                        const char *word) {
  if ((bufsize_t)strlen(word) != len)
    return false;
  for (bufsize_t i = 0; i < len; ++i)
    if (ascii_lower(data[i]) != (unsigned char)word[i])
      return false;
  return true;
}

static int linkify_character(const unsigned char *data, bufsize_t len,
                             bool domain) {
  if (!len)
    return 0;
  if (data[0] < 0x80)
    return ascii_alnum(data[0]) || data[0] == '-' ||
                   (!domain && strchr("_;:&=+$,.\"", data[0]))
               ? 1
               : 0;
  if (!domain)
    return 0; // Frozen email local parts are ASCII; hosts may be IDN.
  int32_t codepoint = 0;
  int bytes = cmark_utf8proc_iterate(data, len, &codepoint);
  bool symbol_domain = (codepoint >= 0x1f000 && codepoint <= 0x1faff) ||
                       (codepoint >= 0x2600 && codepoint <= 0x27ff);
  return bytes > 0 && !cmark_utf8proc_is_space(codepoint) &&
                 (symbol_domain ||
                  !cmark_utf8proc_is_punctuation_or_symbol(codepoint))
             ? bytes
             : 0;
}

static bool fuzzy_tld(const unsigned char *data, bufsize_t len) {
  static const char *const names[] = {"biz",
                                      "com",
                                      "edu",
                                      "gov",
                                      "net",
                                      "org",
                                      "pro",
                                      "web",
                                      "xxx",
                                      "aero",
                                      "asia",
                                      "coop",
                                      "info",
                                      "museum",
                                      "name",
                                      "shop",
                                      "\xd1\x80\xd1\x84"};
  if (len < 2)
    return false;
  if (len == 4 &&
      ((data[0] == 0xd1 && data[1] == 0x80) ||
       (data[0] == 0xd0 && data[1] == 0xa0)) &&
      ((data[2] == 0xd1 && data[3] == 0x84) ||
       (data[2] == 0xd0 && data[3] == 0xa4)))
    return true;
  if (len > 4 && len <= 63 && ascii_equal(data, 4, "xn--"))
    return true;
  // Frozen linkify-it country-code suffixes, not arbitrary two-letter words.
  static const char *const country_codes[26] = {"cdefgilmnoqrstuwxz",
                                                "abdefghijmnorstvwyz",
                                                "acdfghiklmnoruvwxyz",
                                                "ejkmoz",
                                                "cegrstu",
                                                "ijkmor",
                                                "abdefghilmnpqrstuwy",
                                                "kmnrtu",
                                                "delmnoqrst",
                                                "emop",
                                                "eghimnprwyz",
                                                "abcikrstuvy",
                                                "acdeghklmnopqrstuvwxyz",
                                                "acefgilopruz",
                                                "m",
                                                "aefghklmnrstwy",
                                                "a",
                                                "eosuw",
                                                "abcdeghijklmnortuvxyz",
                                                "cdfghjklmnortvwz",
                                                "agksyz",
                                                "aceginu",
                                                "fs",
                                                "",
                                                "et",
                                                "amw"};
  unsigned char first = ascii_lower(data[0]), second = ascii_lower(data[1]);
  if (len == 2 && first >= 'a' && first <= 'z' &&
      strchr(country_codes[first - 'a'], second))
    return true;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    if (ascii_equal(data, len, names[i]))
      return true;
  return false;
}

static bufsize_t linkify_host(const unsigned char *data, bufsize_t len,
                              bool fuzzy) {
  bufsize_t pos = 0, label = 0, last_label = 0;
  int dots = 0, characters = 0;
  while (pos < len) {
    int bytes = linkify_character(data + pos, len - pos, true);
    if (bytes) {
      if (++characters > 63 || (pos == label && data[pos] == '-'))
        return 0;
      pos += bytes;
      continue;
    }
    if (data[pos] != '.' || pos == label || data[pos - 1] == '-')
      break;
    // A sentence-final period is not part of the hostname.
    if (pos + 1 >= len ||
        !linkify_character(data + pos + 1, len - pos - 1, true))
      break;
    ++dots;
    characters = 0;
    last_label = label = ++pos;
  }
  if (!pos || data[0] == '-' || data[pos - 1] == '-' || data[pos - 1] == '_')
    return 0;
  if (fuzzy && (!dots || !fuzzy_tld(data + last_label, pos - last_label)))
    return 0;
  if (pos < len && (data[pos] == '-' || data[pos] == '_' ||
                    (data[pos] == '.' && pos + 1 < len &&
                     (data[pos + 1] == '-' ||
                      linkify_character(data + pos + 1, len - pos - 1, true)))))
    return 0;
  return pos;
}

static bool linkify_space(const unsigned char *data, bufsize_t len) {
  if (!len)
    return true;
  if (data[0] < 0x80)
    return data[0] < 0x20 || data[0] == 0x7f || cmark_isspace(data[0]);
  int32_t cp;
  int bytes = cmark_utf8proc_iterate(data, len, &cp);
  return bytes <= 0 || cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) ||
         cmark_utf8proc_is_space(cp);
}

static bufsize_t linkify_path(const unsigned char *data, bufsize_t len,
                              bufsize_t pos) {
  // The initial slash may stand alone. A bare trailing ? or # does not.
  bufsize_t start = pos++;
  while (pos < len) {
    unsigned char c = data[pos];
    if (linkify_space(data + pos, len - pos) || c == '<' || c == '>' ||
        c == '\\')
      break;
    if (c >= 0x80) {
      int32_t cp;
      int bytes = cmark_utf8proc_iterate(data + pos, len - pos, &cp);
      if (cp == 0xff5c)
        break;
      pos += bytes;
      continue;
    }
    if (strchr(")]}", c))
      break;
    if (strchr("([{\"'", c)) {
      unsigned char closer = c == '('   ? ')'
                             : c == '[' ? ']'
                             : c == '{' ? '}'
                                        : c;
      bufsize_t end = pos + 1;
      int depth = 1;
      while (end < len && !linkify_space(data + end, len - end)) {
        if (c != closer && data[end] == c)
          ++depth;
        if (data[end] == closer && --depth == 0)
          break;
        if (data[end] < 0x80)
          ++end;
        else {
          int32_t cp;
          end += cmark_utf8proc_iterate(data + end, len - end, &cp);
        }
      }
      if (end < len && data[end] == closer && (end > pos + 1 || c != closer)) {
        pos = end + 1;
        continue;
      }
      if (c == '\'' && pos + 1 < len &&
          linkify_character(data + pos + 1, len - pos - 1, true)) {
        ++pos;
        continue;
      }
      break;
    }
    if (c == '.') {
      bufsize_t end = pos + 1;
      while (end < len && data[end] == '.')
        ++end;
      if (end == len || linkify_space(data + end, len - end))
        break;
      if (end - pos > 1 && !ascii_alnum(data[end]) && !strchr("%/&", data[end]))
        break;
      pos = end;
      continue;
    }
    if (c == ',' || c == ';' || c == '?' || c == '!') {
      bufsize_t end = pos + 1;
      if (c == '!')
        while (end < len && data[end] == '!')
          ++end;
      if (end == len || linkify_space(data + end, len - end) ||
          (c == '?' && data[end] == '?'))
        break;
      pos = end;
      continue;
    }
    ++pos;
  }
  return pos == start + 1 && data[start] != '/' ? start : pos;
}

static bufsize_t linkify_ipv4(const unsigned char *data, bufsize_t len) {
  bufsize_t pos = 0;
  for (int part = 0; part < 4; ++part) {
    unsigned int value = 0;
    bufsize_t start = pos;
    while (pos < len && cmark_isdigit(data[pos]) && pos - start < 3)
      value = value * 10 + data[pos++] - '0';
    if (pos == start || value > 255)
      return 0;
    if (part < 3 && (pos >= len || data[pos++] != '.'))
      return 0;
  }
  if (pos < len &&
      (linkify_character(data + pos, len - pos, true) || data[pos] == '.'))
    return 0;
  return pos;
}

// Return a match length and a normalization prefix (NULL for no match).
static bufsize_t linkify_match(const unsigned char *data, bufsize_t len,
                               bool fuzzy_allowed, bool email_allowed,
                               const char **prefix, bufsize_t *host_start,
                               bufsize_t *host_end) {
  bufsize_t start = 0, end = 0;
  bool explicit_url = false;
  *prefix = NULL;
  if (len >= 7 && ascii_equal(data, 7, "http://")) {
    start = 7;
    explicit_url = true;
  } else if (len >= 8 && ascii_equal(data, 8, "https://")) {
    start = 8;
    explicit_url = true;
  } else if (len >= 6 && ascii_equal(data, 6, "ftp://")) {
    start = 6;
    explicit_url = true;
  } else if (len >= 2 && data[0] == '/' && data[1] == '/') {
    start = 2;
    explicit_url = true;
  }
  bool mailto = len >= 7 && ascii_equal(data, 7, "mailto:");
  if (mailto)
    start = 7;
  if (!explicit_url && (mailto || email_allowed)) {
    bufsize_t local = start;
    while (local < len && (local != start || data[local] != '"')) {
      int bytes = linkify_character(data + local, len - local, false);
      if (!bytes)
        break;
      local += bytes;
    }
    if (local > start && local < len && data[local] == '@') {
      bufsize_t host = linkify_host(data + local + 1, len - local - 1, !mailto);
      if (!host)
        host = linkify_ipv4(data + local + 1, len - local - 1);
      if (host) {
        *prefix = mailto ? "" : "mailto:";
        *host_start = local + 1;
        *host_end = local + 1 + host;
        return *host_end;
      }
    }
    if (mailto)
      return 0;
  } else if (explicit_url) {
    // Optional userinfo belongs to the URL, not the host.
    bufsize_t authority = start;
    while (authority < len && !cmark_isspace(data[authority]) &&
           data[authority] != '/' && data[authority] != '?' &&
           data[authority] != '#') {
      if (data[authority] == '@')
        start = authority + 1;
      ++authority;
    }
  }
  if (!explicit_url && !fuzzy_allowed)
    return 0;
  if (start < len && data[start] == '[' && explicit_url) {
    end = start + 1;
    while (end < len &&
           (ascii_hex(data[end]) || data[end] == ':' || data[end] == '.'))
      ++end;
    if (end == start + 1 || end >= len || data[end] != ']')
      return 0;
    ++end;
  } else {
    bufsize_t host = linkify_host(data + start, len - start, !explicit_url);
    if (!host)
      return 0;
    end = start + host;
  }
  *host_start = start;
  *host_end = end;
  if (end < len && data[end] == ':' && end + 1 < len &&
      cmark_isdigit(data[end + 1])) {
    ++end;
    unsigned int port = 0;
    bufsize_t port_start = end;
    while (end < len && cmark_isdigit(data[end])) {
      if (end - port_start >= 5)
        return 0;
      port = port * 10 + data[end++] - '0';
    }
    if (port > 65535)
      return 0;
  }
  if (end < len && (data[end] == '/' || data[end] == '?' || data[end] == '#'))
    end = linkify_path(data, len, end);
  *prefix = explicit_url ? "" : "http://";
  return end;
}

static unsigned int puny_adapt(uint64_t delta, unsigned int points,
                               bool first) {
  unsigned int k = 0;
  delta = first ? delta / 700 : delta / 2;
  delta += delta / points;
  while (delta > 455) {
    delta /= 35;
    k += 36;
  }
  return k + (unsigned int)(36 * delta / (delta + 38));
}

static void linkify_label(cmark_strbuf *url, const unsigned char *data,
                          bufsize_t len) {
  int32_t points[256];
  unsigned int count = 0, basic = 0;
  for (bufsize_t pos = 0; pos < len;) {
    int32_t cp;
    int bytes = cmark_utf8proc_iterate(data + pos, len - pos, &cp);
    if (bytes <= 0 || count == 256) {
      cmark_strbuf_put(url, data, len);
      return;
    }
    points[count++] = cp;
    if (cp < 128)
      ++basic;
    pos += bytes;
  }
  if (basic == count) {
    cmark_strbuf_put(url, data, len);
    return;
  }
  cmark_strbuf_puts(url, "xn--");
  for (unsigned int i = 0; i < count; ++i)
    if (points[i] < 128)
      cmark_strbuf_putc(url, ascii_lower((unsigned char)points[i]));
  if (basic)
    cmark_strbuf_putc(url, '-');
  uint64_t delta = 0;
  unsigned int n = 128, bias = 72, handled = basic;
  while (handled < count) {
    unsigned int next = UINT_MAX;
    for (unsigned int i = 0; i < count; ++i)
      if ((unsigned int)points[i] >= n && (unsigned int)points[i] < next)
        next = points[i];
    delta += (uint64_t)(next - n) * (handled + 1);
    n = next;
    for (unsigned int i = 0; i < count; ++i) {
      if ((unsigned int)points[i] < n)
        ++delta;
      if ((unsigned int)points[i] == n) {
        uint64_t q = delta;
        for (unsigned int k = 36;; k += 36) {
          unsigned int t = k <= bias ? 1 : k >= bias + 26 ? 26 : k - bias;
          if (q < t)
            break;
          unsigned int digit = t + (unsigned int)((q - t) % (36 - t));
          cmark_strbuf_putc(url, digit < 26 ? 'a' + digit : '0' + digit - 26);
          q = (q - t) / (36 - t);
        }
        cmark_strbuf_putc(url, q < 26 ? 'a' + (int)q : '0' + (int)q - 26);
        bias = puny_adapt(delta, handled + 1, handled == basic);
        delta = 0;
        ++handled;
      }
    }
    ++delta;
    ++n;
  }
}

static unsigned char *linkify_url(cmark_mem *mem, const unsigned char *data,
                                  bufsize_t len, const char *prefix,
                                  bufsize_t host_start, bufsize_t host_end) {
  cmark_strbuf url = CMARK_BUF_INIT(mem);
  cmark_strbuf_puts(&url, prefix);
  cmark_strbuf_put(&url, data, host_start);
  bufsize_t label = host_start;
  for (bufsize_t pos = host_start; pos <= host_end; ++pos) {
    if (pos == host_end || data[pos] == '.') {
      linkify_label(&url, data + label, pos - label);
      if (pos != host_end)
        cmark_strbuf_putc(&url, '.');
      label = pos + 1;
    }
  }
  static const char hex[] = "0123456789ABCDEF";
  for (bufsize_t pos = host_end; pos < len; ++pos) {
    unsigned char c = data[pos];
    if (c >= 0x80 || c == ' ' || strchr("\"[]{}\\`", c) ||
        (c == '%' && (pos + 2 >= len || !ascii_hex(data[pos + 1]) ||
                      !ascii_hex(data[pos + 2])))) {
      cmark_strbuf_putc(&url, '%');
      cmark_strbuf_putc(&url, hex[c >> 4]);
      cmark_strbuf_putc(&url, hex[c & 15]);
    } else
      cmark_strbuf_putc(&url, c);
  }
  return cmark_strbuf_detach(&url);
}

static void linkify_position(linkify_source *source, bufsize_t offset, bool end,
                             int *line, int *column) {
  while (source->node->next && offset >= source->offset + source->node->len) {
    source->offset += source->node->len;
    source->node = source->node->next;
  }
  cmark_node *node = source->node;
  *line = end ? node->end_line : node->start_line;
  if (node->start_line == node->end_line &&
      node->end_column - node->start_column + 1 == node->len)
    *column = node->start_column + offset - source->offset;
  else
    *column = end ? node->end_column : node->start_column;
}

static cmark_node *linkify_text(cmark_node *before, linkify_source *source,
                                const unsigned char *data, bufsize_t start,
                                bufsize_t end) {
  cmark_node *text = make_simple(before->mem, CMARK_NODE_TEXT);
  text->len = end - start;
  text->data = copy_span(before->mem, data + start, text->len);
  linkify_position(source, start, false, &text->start_line,
                   &text->start_column);
  linkify_position(source, end - 1, true, &text->end_line, &text->end_column);
  return text;
}

static bufsize_t linkify_previous_character(const unsigned char *data,
                                            bufsize_t pos) {
  bufsize_t start = pos - 1;
  while (start > 0 && (data[start] & 0xc0) == 0x80)
    --start;
  return start;
}

static bool linkify_word_before(const unsigned char *data, bufsize_t pos) {
  if (!pos)
    return false;
  bufsize_t start = linkify_previous_character(data, pos);
  return linkify_character(data + start, pos - start, true) > 0 ||
         (data[start] < 0x80 && strchr("_@/\\", data[start]));
}

static bool linkify_email_boundary(const unsigned char *data, bufsize_t pos) {
  if (!pos)
    return true;
  bufsize_t start = linkify_previous_character(data, pos);
  if (data[start] < 0x80)
    return cmark_isspace(data[start]) || strchr("><\"(", data[start]);
  int32_t cp;
  int bytes = cmark_utf8proc_iterate(data + start, pos - start, &cp);
  return bytes > 0 && (cmark_utf8proc_is_space(cp) || cp == 0xff5c);
}

static void linkify_run(cmark_node *first, cmark_node *after) {
  cmark_strbuf joined = CMARK_BUF_INIT(first->mem);
  const unsigned char *data = first->data;
  bufsize_t len = first->len;
  if (first->next != after) {
    bool candidate = false;
    len = 0;
    for (cmark_node *node = first; node != after; node = node->next) {
      len += node->len;
      if (!candidate && node->len)
        candidate = memchr(node->data, '.', node->len) ||
                    memchr(node->data, '@', node->len) ||
                    memchr(node->data, ':', node->len);
    }
    if (!candidate)
      return;
    cmark_strbuf_grow(&joined, len);
    for (cmark_node *node = first; node != after; node = node->next)
      cmark_strbuf_put(&joined, node->data, node->len);
    data = joined.ptr;
    len = joined.size;
  }
  linkify_source source = {first, 0};
  bufsize_t emitted = 0;
  bool changed = false;
  for (bufsize_t pos = 0; pos < len;) {
    if (pos == 0 && first->prev &&
        (first->prev->flags & CMARK_NODE__LITERAL_ESCAPE) && first->prev->len &&
        strchr(".:/\\-_@", first->prev->data[first->prev->len - 1]) &&
        !linkify_space(data, len)) {
      while (pos < len && !linkify_space(data + pos, len - pos))
        ++pos;
      continue;
    }
    if (linkify_word_before(data, pos)) {
      ++pos;
      continue;
    }
    const char *prefix;
    bufsize_t host_start, host_end;
    bool fuzzy_allowed = pos == 0 || !strchr(".:/\\-_@", data[pos - 1]);
    bool email_allowed = linkify_email_boundary(data, pos);
    bufsize_t matched =
        linkify_match(data + pos, len - pos, fuzzy_allowed, email_allowed,
                      &prefix, &host_start, &host_end);
    if (!matched) {
      if (linkify_character(data + pos, len - pos, true) || data[pos] == '.') {
        // Skip only the failed word, never a word after a separator. Starting
        // this loop on whitespace would skip the next possible URL entirely.
        do {
          ++pos;
        } while (pos < len && (linkify_character(data + pos, len - pos, true) ||
                               data[pos] == '.'));
      } else {
        ++pos;
      }
      continue;
    }
    if (pos > emitted) {
      cmark_node *text = linkify_text(first, &source, data, emitted, pos);
      cmark_node_insert_before(first, text);
    }
    cmark_node *link = make_simple(first->mem, CMARK_NODE_LINK);
    cmark_node *text = linkify_text(first, &source, data, pos, pos + matched);
    link->start_line = link->as.link.url_start_line = text->start_line;
    link->start_column = link->as.link.url_start_column = text->start_column;
    link->end_line = link->as.link.url_end_line = text->end_line;
    link->end_column = link->as.link.url_end_column = text->end_column;
    link->as.link.url = linkify_url(first->mem, data + pos, matched, prefix,
                                    host_start, host_end);
    append_child(link, text);
    cmark_node_insert_before(first, link);
    emitted = pos += matched;
    changed = true;
  }
  if (changed) {
    if (emitted < len) {
      cmark_node *text = linkify_text(first, &source, data, emitted, len);
      cmark_node_insert_before(first, text);
    }
    while (first != after) {
      cmark_node *next = first->next;
      cmark_node_free(first);
      first = next;
    }
  }
  cmark_strbuf_free(&joined);
}

static void linkify_inlines(cmark_node *parent) {
  cmark_iter *iter = cmark_iter_new(parent);
  cmark_event_type event;
  int html_depth = 0;
  while ((event = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    if (event != CMARK_EVENT_ENTER)
      continue;
    cmark_node *node = cmark_iter_get_node(iter);
    if (node->type == CMARK_NODE_LINK || node->type == CMARK_NODE_WIKILINK ||
        node->type == CMARK_NODE_IMAGE) {
      cmark_iter_reset(iter, node, CMARK_EVENT_EXIT);
    } else if (node->type == CMARK_NODE_HTML_INLINE && node->len > 2) {
      bufsize_t start = node->data[1] == '/' ? 2 : 1, end = start;
      while (end < node->len &&
             (ascii_alnum(node->data[end]) || node->data[end] == '-'))
        ++end;
      if (ascii_equal(node->data + start, end - start, "a") ||
          ascii_equal(node->data + start, end - start, "code") ||
          ascii_equal(node->data + start, end - start, "pre") ||
          ascii_equal(node->data + start, end - start, "script") ||
          ascii_equal(node->data + start, end - start, "style")) {
        if (start == 2) {
          if (html_depth)
            --html_depth;
        } else if (node->data[node->len - 2] != '/')
          ++html_depth;
      }
    } else if (!html_depth && node->type == CMARK_NODE_TEXT &&
               !(node->flags & CMARK_NODE__LITERAL_ESCAPE)) {
      cmark_node *last = node;
      while (last->next && last->next->type == CMARK_NODE_TEXT &&
             !(last->next->flags & CMARK_NODE__LITERAL_ESCAPE))
        last = last->next;
      cmark_node *after = last->next;
      cmark_iter_reset(iter, last, CMARK_EVENT_EXIT);
      linkify_run(node, after);
    }
  }
  cmark_iter_free(iter);
}

// Parse inlines from parent's string_content, adding as children of parent.
void cmark_parse_inlines(cmark_mem *mem, cmark_node *parent,
                         cmark_reference_map *refmap,
                         cmark_footnote_map *footnote_map, int options) {
  int internal_offset = parent->type == CMARK_NODE_HEADING ?
    parent->as.heading.internal_offset : 0;
  subject subj;
  cmark_chunk content = {parent->data, parent->len};
  int content_line = parent->content_start_line ? parent->content_start_line : parent->start_line;
  subject_from_buf(mem, content_line, parent->start_column - 1 + internal_offset,
                   &subj, &content, refmap, footnote_map);
  subj.source = (options & CMARK_OPT_SOURCE_SYNTAX) ? parent : NULL;
  cmark_chunk_rtrim(&subj.input);
  subj.unescape_table_pipes = parent->type == CMARK_NODE_TABLE_CELL &&
                              (options & CMARK_OPT_VIEWER_COMPAT);

  if (!(options & CMARK_OPT_NO_EXTENSIONS) && (options & CMARK_OPT_TASKLISTS) &&
      parent->type == CMARK_NODE_PARAGRAPH && parent->parent &&
      parent->parent->type == CMARK_NODE_ITEM && parent->prev == NULL &&
      subj.input.len >= 4 && subj.input.data[0] == '[' &&
      (subj.input.data[1] == ' ' || subj.input.data[1] == 'x' ||
       subj.input.data[1] == 'X') &&
      subj.input.data[2] == ']' && cmark_isspace(subj.input.data[3])) {
    parent->parent->as.list.task = subj.input.data[1] == ' ' ? 1 : 2;
    subj.pos = 3;
    if (subj.input.data[3] == ' ' || subj.input.data[3] == '\t')
      ++subj.pos;
  }

  while (!is_eof(&subj) && parse_inline(&subj, parent, options))
    ;

  process_emphasis(&subj, 0);
  // free bracket and delim stack
  while (subj.last_delim) {
    remove_delimiter(&subj, subj.last_delim);
  }
  while (subj.last_bracket) {
    pop_bracket(&subj);
  }
  if ((options & CMARK_OPT_LINKIFY) && !(options & CMARK_OPT_NO_EXTENSIONS))
    linkify_inlines(parent);
}

// Parse zero or more space characters, including at most one newline.
static void spnl(subject *subj) {
  skip_spaces(subj);
  if (skip_line_end(subj)) {
    skip_spaces(subj);
  }
}

// Parse reference.  Assumes string begins with '[' character.
// Modify refmap if a reference is encountered.
// Return 0 if no reference found, otherwise position of subject
// after reference is parsed.
bufsize_t cmark_parse_reference_inline(cmark_mem *mem, cmark_chunk *input,
                                       cmark_reference_map *refmap,
                                       bufsize_t *url_start, bufsize_t *url_end) {
  subject subj;

  cmark_chunk lab;
  cmark_chunk url;
  cmark_chunk title;

  bufsize_t matchlen = 0;
  bufsize_t beforetitle;
  bufsize_t starturl;
  bufsize_t endurl;

  if (url_start) {
    *url_start = 0;
  }
  if (url_end) {
    *url_end = 0;
  }
  subject_from_buf(mem, -1, 0, &subj, input, NULL, NULL);

  // parse label:
  if (!link_label(&subj, &lab) || lab.len == 0)
    return 0;

  // colon:
  if (peek_char(&subj) == ':') {
    advance(&subj);
  } else {
    return 0;
  }

  // parse link url:
  spnl(&subj);
  starturl = subj.pos;
  if ((matchlen = manual_scan_link_url(&subj.input, subj.pos, &url)) > -1) {
    subj.pos += matchlen;
  } else {
    return 0;
  }

  endurl = subj.pos;

  // parse optional link_title
  beforetitle = subj.pos;
  spnl(&subj);
  matchlen = subj.pos == beforetitle ? 0 : scan_link_title(&subj.input, subj.pos);
  if (matchlen) {
    title = cmark_chunk_dup(&subj.input, subj.pos, matchlen);
    subj.pos += matchlen;
  } else {
    subj.pos = beforetitle;
    title = cmark_chunk_literal("");
  }

  // parse final spaces and newline:
  skip_spaces(&subj);
  if (!skip_line_end(&subj)) {
    if (matchlen) { // try rewinding before title
      subj.pos = beforetitle;
      skip_spaces(&subj);
      if (!skip_line_end(&subj)) {
        return 0;
      }
    } else {
      return 0;
    }
  }
  // insert reference into refmap
  cmark_reference_create(refmap, &lab, &url, &title);
  if (url_start) {
    *url_start = starturl;
  }
  if (url_end) {
    *url_end = endurl;
  }
  return subj.pos;
}
