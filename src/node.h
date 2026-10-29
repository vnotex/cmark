#ifndef CMARK_NODE_H
#define CMARK_NODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cmark.h"
#include "buffer.h"

typedef struct {
  int marker_offset;
  int padding;
  int start;
  unsigned char list_type;
  unsigned char delimiter;
  unsigned char bullet_char;
  bool tight;
  unsigned char task; /* 0: ordinary item, 1: unchecked, 2: checked */
} cmark_list;

typedef struct {
  unsigned char *info;
  uint8_t fence_length;
  uint8_t fence_offset;
  unsigned char fence_char;
  int8_t fenced;
} cmark_code;

typedef struct {
  uint8_t fence_length;
  uint8_t fence_offset;
  unsigned char *label;
  unsigned char *environment; /* raw TeX block, NULL for dollar fences */
  bool viewer;
  bool closed;
  bool require_closer;
  bool invalid_environment;
  bool raw_end_pending;
  bool invalid_closer;
} cmark_formula;

typedef struct {
  bufsize_t fence_length;
} cmark_alert;

typedef struct {
  int internal_offset;
  int8_t level;
  bool setext;
} cmark_heading;

typedef enum {
  CMARK_TABLE_ALIGN_NONE = 0,    // No alignment specified (---)
  CMARK_TABLE_ALIGN_LEFT = 1,    // :---
  CMARK_TABLE_ALIGN_CENTER = 2,  // :---:
  CMARK_TABLE_ALIGN_RIGHT = 3    // ---:
} cmark_table_align;

typedef enum {
  CMARK_TABLE_ROW_TYPE_HEADER,
  CMARK_TABLE_ROW_TYPE_DELIMITER,
  CMARK_TABLE_ROW_TYPE_DATA
} cmark_table_row_type;

typedef struct {
  int columns_cnt;
  cmark_table_align *alignments;
} cmark_table;

typedef struct {
  cmark_table_row_type type;
} cmark_table_row;

typedef struct {
  int idx;
  bool is_delimiter;
} cmark_table_cell;

typedef struct {
  unsigned char *url;
  unsigned char *title;
  int width;
  int height;
  unsigned char *width_text;
  unsigned char *height_text;
  /* Source position of the raw destination as spelled in the input, for inline
   * links, images and autolinks with a non-empty destination only. Autolink
   * spans exclude their angle brackets. Zero for reference-style links and
   * for an empty destination such as `[a]()`.
   * `url_end_column` is inclusive of the last byte, so that it matches the
   * convention of cmark_node's own `end_column`. */
  int url_start_line;
  int url_start_column;
  int url_end_line;
  int url_end_column;
} cmark_link;

typedef struct {
  unsigned char *on_enter;
  unsigned char *on_exit;
} cmark_custom;

typedef struct {
  int marker_offset;
  int padding;
  int number;  // assigned during inline parsing (0 = unreferenced)
} cmark_footnote_def;

typedef struct {
  int number;
} cmark_footnote_ref;

enum cmark_node__internal_flags {
  CMARK_NODE__OPEN = (1 << 0),
  CMARK_NODE__LAST_LINE_BLANK = (1 << 1),
  CMARK_NODE__LAST_LINE_CHECKED = (1 << 2),
  CMARK_NODE__LIST_LAST_LINE_BLANK = (1 << 3),
  CMARK_NODE__LITERAL_ESCAPE = (1 << 4),
  CMARK_NODE__UNCLOSED_FORMULA = (1 << 5),
};

// Exact byte-preserving portions of a block's inline input. Synthetic tab
// padding and normalized line endings are deliberately not mapped.
typedef struct {
  bufsize_t offset;
  bufsize_t length;
  int line;
  int column;
} cmark_source_map;

typedef struct {
  int line;
  int column;
  int length;
  const unsigned char *text;
} cmark_syntax_span;

typedef struct {
  int start_line;
  int start_column;
  int end_line;
  int end_column;
  int count;
  // The span array and its raw verification bytes share this allocation.
  // The pointer also gives the header the span array's required alignment.
  cmark_syntax_span *spans;
} cmark_syntax;

struct cmark_node {
  cmark_mem *mem;

  struct cmark_node *next;
  struct cmark_node *prev;
  struct cmark_node *parent;
  struct cmark_node *first_child;
  struct cmark_node *last_child;

  void *user_data;

  unsigned char *data;
  bufsize_t len;

  // First retained content line after reference definitions are removed.
  // Keep start_line/start_column as the original block-prefix coordinate anchor.
  int content_start_line;
  cmark_source_map *source_map;
  int source_map_count;
  int source_map_capacity;
  cmark_syntax *syntax;
  int start_line;
  int start_column;
  int end_line;
  int end_column;
  uint16_t type;
  uint16_t flags;

  union {
    cmark_list list;
    cmark_code code;
    cmark_heading heading;
    cmark_link link;
    cmark_custom custom;
    cmark_formula formula;
    cmark_alert alert;
    cmark_table table;
    cmark_table_row table_row;
    cmark_table_cell table_cell;
    int html_block_type;
    cmark_footnote_def footnote_def;
    cmark_footnote_ref footnote_ref;
  } as;
};

void cmark_node_add_source_map(cmark_node *node, bufsize_t offset,
                               bufsize_t length, int line, int column);
void cmark_node_drop_source_map(cmark_node *node, bufsize_t length);
void cmark_node_set_syntax(cmark_node *node, cmark_node *source,
                           const unsigned char *data, bufsize_t length,
                           bufsize_t start, bufsize_t open_end,
                           bufsize_t close_start, bufsize_t end);

CMARK_EXPORT int cmark_node_check(cmark_node *node, FILE *out);

#ifdef __cplusplus
}
#endif

#endif
