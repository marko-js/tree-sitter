// Tree-sitter external scanner for Marko: a faithful port of htmljs-parser's
// state machine (../../src). All terminals are produced here; grammar.js only
// sequences them.
//
// Mapping from parser states to this file:
//   CONCISE_HTML_CONTENT       -> scan_concise (+ dedent handling)
//   HTML_CONTENT               -> scan_html_content
//   PARSED_TEXT_CONTENT        -> scan_parsed_text
//   BEGIN_DELIMITED_HTML_BLOCK -> BLOCK_OPEN / frame "fresh" handling
//   OPEN_TAG / TAG_NAME / ATTRIBUTE -> scan_open_tag
//   EXPRESSION (+ STRING / TEMPLATE_STRING / REGULAR_EXPRESSION /
//     JS_COMMENT_*)            -> scan_expr_inner
//
// Errors: wherever the parser calls emitError (and then stops parsing), this
// scanner refuses to produce a token, so tree-sitter emits an ERROR node and
// the rest of the input is not interpreted - mirroring the parser.
//
// tree-sitter's lexer cannot rewind: a token's start is fixed by the first
// non-skipped advance and mark_end can only mark the current position. The
// EStream type below layers a logical position + lookahead queue on top so
// the parser's lookAheadFor/lookBehindFor logic can be ported directly.

#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef TSMARKO_DEBUG
#include <stdio.h>
#endif

enum TokenType {
  TEXT,
  PLACEHOLDER_START,        // "${"
  PLACEHOLDER_START_RAW,    // "$!{"
  INTERP_START,             // "${" within tag names
  PLACEHOLDER_EXPR,
  PLACEHOLDER_END,          // "}"
  HTML_COMMENT,
  LINE_COMMENT,
  BLOCK_COMMENT,
  CDATA,
  DOCTYPE,
  DECLARATION,
  SCRIPTLET_START,          // "$" html mode
  SCRIPTLET_START_CONCISE,  // "$" concise mode
  SCRIPTLET_BLOCK_OPEN,     // "{"
  SCRIPTLET_BLOCK_EXPR,
  SCRIPTLET_BLOCK_CLOSE,    // "}" [ws ";"]
  SCRIPTLET_EXPR,
  BLOCK_OPEN,               // "--"...
  BLOCK_CLOSE,
  OPEN_TAG_START,           // "<"
  TAG_NAME_FRAGMENT,
  TAG_NAME_EMPTY,
  SHORTHAND_ID_START,       // "#"
  SHORTHAND_CLASS_START,    // "."
  STATEMENT_EXPR,
  TAG_VAR_START,            // "/"
  VAR_PATTERN,
  VAR_COLON,                // ":" before a tag var type
  VAR_TYPE,
  ARGS_OPEN,                // "("
  ARGS_EXPR,
  ARGS_CLOSE,               // ")"
  PARAMS_OPEN,              // "|"
  PARAM_PATTERN,
  PARAM_COLON,              // ":" before a parameter type
  PARAM_TYPE,
  PARAM_EQ,                 // "=" before a parameter default
  PARAM_DEFAULT,
  PARAM_COMMA,              // ","
  PARAMS_CLOSE,             // "|"
  TYPE_ARGS_OPEN,           // "<" adjacent to tag name
  TYPE_PARAMS_OPEN,         // "<"
  TYPE_EXPR,
  TYPES_CLOSE,              // ">"
  ATTR_GROUP_OPEN,          // "["
  ATTR_GROUP_CLOSE,         // "]"
  ATTR_NAME,
  ATTR_EQ,                  // "="
  ATTR_BOUND_EQ,            // ":="
  ATTR_SPREAD_START,        // "..."
  ATTR_VALUE_EXPR,
  METHOD_BODY_OPEN,         // "{"
  METHOD_BODY_EXPR,
  METHOD_BODY_CLOSE,        // "}"
  OPEN_TAG_END,             // ">"
  OPEN_TAG_END_SELF,        // "/>"
  CONCISE_OPEN_TAG_END,     // zero-width or ";"
  CLOSE_TAG_START,          // "</"
  CLOSE_TAG_NAME,
  CLOSE_TAG_END,            // ">"
  ELEMENT_END,              // zero-width
  TAG_COMMENT,              // hidden: comments inside open tags (no events)
  ESCAPE,                   // hidden: omitted backslashes before "${"
  ERROR_SENTINEL,
};

// --------------------------------------------------------------------------
// Character helpers (port of util/util.ts)
// --------------------------------------------------------------------------

static inline bool is_ws(int32_t c) { return c <= ' ' && c >= 0; }
static inline bool is_line(int32_t c) { return c == '\n' || c == '\r'; }
static inline bool is_indent_code(int32_t c) { return c == ' ' || c == '\t'; }
static inline bool is_word_code(int32_t c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '$' || c == '_';
}
static inline bool is_word_or_period_code(int32_t c) {
  return c == '.' || is_word_code(c);
}

#define HASH_INIT 2166136261u
static inline uint32_t hash_push(uint32_t h, int32_t c) {
  return (h ^ (uint32_t)(c & 0xff)) * 16777619u;
}
static uint32_t hash_str(const char *str) {
  uint32_t h = HASH_INIT;
  for (; *str; str++) h = hash_push(h, (int32_t)*str);
  return h;
}

// --------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------

enum TagKind { TAG_HTML = 0, TAG_TEXT = 1, TAG_VOID = 2, TAG_STATEMENT = 3 };

enum ContentKind { CONTENT_HTML = 0, CONTENT_PARSED_TEXT = 1 };

enum FrameFresh {
  FRESH_NONE = 0,
  FRESH_SINGLE,      // "-- x": skip one char, then content
  FRESH_DELIM_LINE,  // "--\n": skip to EOL + newline, then first block line
};

typedef struct {
  uint32_t name_hash;  // hash of the tag name's source text
  uint32_t full_hash;  // name + shorthand source text (for </div.foo>)
  uint32_t indent_hash;
  uint32_t nested_indent_hash;
  uint16_t name_len;
  uint16_t full_len;
  uint16_t indent_len;
  uint16_t nested_indent_len;
  uint8_t type;   // enum TagKind
  uint8_t flags;  // TF_*
} TagFrame;

#define TF_CONCISE 1u
#define TF_BEGIN_MIXED 2u
#define TF_HAS_NESTED_INDENT 4u
#define TF_NAME_INTERP 8u

typedef struct {
  uint32_t indent_hash;
  uint16_t indent_len;
  uint16_t delimiter_len;  // hyphen count; 0 => not a delimited block
  uint8_t content;         // enum ContentKind
  uint8_t flags;           // CF_*
  uint8_t fresh;           // enum FrameFresh
  uint8_t pstring_quote;   // 0 | '"' | '\'' (PARSED_STRING resumes here)
} ContentFrame;

#define CF_SINGLE_LINE 1u
#define CF_IS_BLOCK 2u  // has BLOCK_OPEN/BLOCK_CLOSE tokens (html_block node)

#define MAX_TAGS 24
#define MAX_CONTENTS 12

// Position bookkeeping replacing the parser's backwards reads of the source.
// Committed in lock-step with mark_end (see `mark`).
typedef struct {
  uint8_t prev_char;
  uint8_t prev_nonws_char;
  uint8_t line_only_ws;  // only whitespace since the last newline
  uint16_t cur_indent_len;
  uint32_t cur_indent_hash;
} Cursor;

typedef struct {
  Cursor cur;    // live: tracks every logically consumed/skipped char
  Cursor saved;  // as of the last mark_end: what gets serialized

  uint8_t started;  // BOM handled
  uint8_t begin_mixed_mode;
  uint8_t ending_mixed_at_eol;
  uint8_t semicolon_line;  // after `tag;` only comments may follow
  uint8_t pending_element_ends;
  uint8_t suppress_placeholder;  // next "${" is literal text (escaped)
  uint8_t fail_next;  // emit an error on the next scan (comment trailing)
  uint8_t tag_comma_continue;  // concise open tag continues after , at EOL

  // open tag state (only one tag can be open at a time)
  uint8_t tag_open;
  uint8_t tag_name_done;
  uint8_t in_shorthand;
  uint8_t adjacent_lt;  // "<" directly terminated the tag name
  uint8_t statement_pending;
  uint8_t in_attr_group;
  uint8_t attr_active;
  uint8_t attr_stage;  // ATTR_*
  uint8_t attr_has_args;
  uint8_t attr_has_type_params;
  uint8_t tag_has_attrs;
  uint8_t tag_has_args;
  uint8_t tag_has_params;
  uint8_t tag_has_shorthand_id;
  uint8_t tag_pending_types;   // tag-level <T> awaiting | or (
  uint8_t types_was_args;      // current <...> is a type-args (adjacent)
  uint8_t expect_method_open;  // attr-level <T> closed; "(" must follow
  uint8_t block_after_tag;     // "--" ended a concise tag: indent extension

  uint8_t tag_len;
  uint8_t content_len;
  TagFrame tags[MAX_TAGS];
  ContentFrame contents[MAX_CONTENTS];

  // scratch (not serialized)
  char *buf;
  uint32_t buf_len, buf_cap;
} Scanner;

// ATTR_STAGE mirror of states/ATTRIBUTE.ts
enum {
  ATTR_UNKNOWN = 0,
  ATTR_NAME_STAGE,
  ATTR_VALUE_STAGE,
  ATTR_ARGUMENT_STAGE,
  ATTR_TYPE_PARAMS_STAGE,
  ATTR_BLOCK_STAGE,
};

// isConcise: only HTML_CONTENT.enter flips the parser out of concise mode;
// PARSED_TEXT_CONTENT keeps the current mode. So concise mode holds unless
// an html content frame is on the stack.
static inline bool is_concise(Scanner *s) {
  for (uint8_t i = 0; i < s->content_len; i++) {
    if (s->contents[i].content == CONTENT_HTML) return false;
  }
  return true;
}
static inline TagFrame *top_tag(Scanner *s) {
  return s->tag_len ? &s->tags[s->tag_len - 1] : NULL;
}
static inline ContentFrame *top_content(Scanner *s) {
  return s->content_len ? &s->contents[s->content_len - 1] : NULL;
}

static void cursor_advance(Cursor *cur, int32_t c) {
  cur->prev_char = (uint8_t)(c & 0xff);
  if (is_line(c)) {
    cur->line_only_ws = 1;
    cur->cur_indent_len = 0;
    cur->cur_indent_hash = HASH_INIT;
  } else if (is_ws(c)) {
    if (cur->line_only_ws) {
      cur->cur_indent_len++;
      cur->cur_indent_hash = hash_push(cur->cur_indent_hash, c);
    }
  } else {
    cur->line_only_ws = 0;
    cur->prev_nonws_char = (uint8_t)(c & 0xff);
  }
}

// Skip a trivia char (not part of any token; token start moves past it).
static inline void skipc(Scanner *s, TSLexer *lexer) {
  cursor_advance(&s->cur, lexer->lookahead);
  lexer->advance(lexer, true);
}

// Mark the current lexer position as the token end and commit the cursor.
// Only call when the lexer position equals the logical position.
static inline void mark(Scanner *s, TSLexer *lexer) {
  lexer->mark_end(lexer);
  s->saved = s->cur;
}

static inline bool at_eof(TSLexer *lexer) { return lexer->eof(lexer); }

static inline void buf_push(Scanner *s, int32_t c) {
  if (s->buf_len == s->buf_cap) {
    s->buf_cap = s->buf_cap ? s->buf_cap * 2 : 256;
    s->buf = (char *)realloc(s->buf, s->buf_cap);
  }
  s->buf[s->buf_len++] = (char)(c & 0xff);
}

// --------------------------------------------------------------------------
// EStream: logical stream with lookahead over the TSLexer
// --------------------------------------------------------------------------

typedef struct {
  Scanner *s;
  TSLexer *lexer;
  uint32_t start;  // s->buf index where the current construct begins
  uint32_t marked_len;  // buf_len at the last committed mark
  uint8_t before_token;  // the char immediately before the token start
  int32_t *la;
  uint32_t la_len, la_pos, la_cap;
  bool automark;  // mark after every consumed char (expressions)
} EStream;

static void es_init(EStream *es, Scanner *s, TSLexer *lexer, bool automark) {
  memset(es, 0, sizeof(*es));
  es->s = s;
  es->lexer = lexer;
  es->automark = automark;
  es->before_token = s->cur.prev_char;
  s->buf_len = 0;
}

static void es_free(EStream *es) { free(es->la); }

// Peek the k-th char ahead of the logical position (k=0 -> current char).
// Returns -1 at EOF. The queue holds chars the lexer has advanced past;
// lexer->lookahead is the next unqueued char, so peeking k chars ahead only
// advances the lexer k times (the last char inspected stays unconsumed).
static int32_t es_peek(EStream *es, uint32_t k) {
  uint32_t avail = es->la_len - es->la_pos;
  if (k < avail) return es->la[es->la_pos + k];
  while (avail < k) {
    if (at_eof(es->lexer)) return -1;
    if (es->la_len == es->la_cap) {
      es->la_cap = es->la_cap ? es->la_cap * 2 : 64;
      es->la = (int32_t *)realloc(es->la, es->la_cap * sizeof(int32_t));
    }
    es->la[es->la_len++] = es->lexer->lookahead;
    es->lexer->advance(es->lexer, false);
    avail++;
  }
  return at_eof(es->lexer) ? -1 : es->lexer->lookahead;
}

static inline bool es_drained(EStream *es) { return es->la_pos == es->la_len; }

// Logically consume one char. Marks the token end when the lookahead queue
// drains (so the committed end always sits at a logical position).
static void es_consume(EStream *es, bool always_mark) {
  int32_t c;
  if (es->la_pos < es->la_len) {
    c = es->la[es->la_pos++];
    if (es->la_pos == es->la_len) es->la_pos = es->la_len = 0;
  } else {
    if (at_eof(es->lexer)) return;
    c = es->lexer->lookahead;
    es->lexer->advance(es->lexer, false);
  }
  buf_push(es->s, c);
  cursor_advance(&es->s->cur, c);
  if (es->la_pos == es->la_len && (always_mark || es->automark)) {
    mark(es->s, es->lexer);
    es->marked_len = es->s->buf_len;
  }
}

static inline void es_next(EStream *es) { es_consume(es, false); }
static inline void es_next_mark(EStream *es) { es_consume(es, true); }

// Explicitly mark at the current logical position if no lookahead is pending.
static void es_mark(EStream *es) {
  if (es_drained(es)) {
    mark(es->s, es->lexer);
    es->marked_len = es->s->buf_len;
  }
}

static int32_t es_at(EStream *es, int64_t i) {
  Scanner *s = es->s;
  if (i >= 0 && (uint64_t)i < s->buf_len) return (int32_t)(uint8_t)s->buf[i];
  // The parser reads the raw document before the expression (eg the ":"
  // before a tag var type keeps whitespace continuation working); one char
  // of history is tracked.
  if (i == -1 && es->before_token) return (int32_t)es->before_token;
  return -1;
}

static int32_t es_prev_char(EStream *es) {
  Scanner *s = es->s;
  if (s->buf_len > 0) return (int32_t)(uint8_t)s->buf[s->buf_len - 1];
  return s->cur.prev_char;
}

static int32_t es_prev_nonws(EStream *es) {
  Scanner *s = es->s;
  for (int64_t i = (int64_t)s->buf_len - 1; i >= 0; i--) {
    int32_t c = (int32_t)(uint8_t)s->buf[i];
    if (!is_ws(c)) return c;
  }
  return s->cur.prev_nonws_char;
}

// --------------------------------------------------------------------------
// Expression scanning (port of states/EXPRESSION.ts)
// --------------------------------------------------------------------------

enum Term {
  TERM_NONE = 0,
  TERM_CLOSE_PAREN,
  TERM_CLOSE_CURLY,
  TERM_CLOSE_ANGLE,
  TERM_PIPE,
  TERM_HTML_ATTR_NAME,
  TERM_CONCISE_ATTR_NAME,
  TERM_CONCISE_GROUPED_ATTR_NAME,
  TERM_HTML_ATTR_VALUE,
  TERM_CONCISE_ATTR_VALUE,
  TERM_CONCISE_GROUPED_ATTR_VALUE,
  TERM_HTML_TAG_VAR,
  TERM_CONCISE_TAG_VAR,
  TERM_PARAM_PATTERN,  // "|" "," ":" "=" at depth 0
  TERM_PARAM_TYPE,     // "|" "," "=" at depth 0
  TERM_PARAM_DEFAULT,  // "|" "," at depth 0
};

typedef struct {
  bool operators;
  bool terminated_by_eol;
  bool terminated_by_whitespace;
  bool consume_indented;
  bool in_type;
  bool force_type;
  bool split_at_type_colon;  // stop before the ":" that enters type mode
  bool resume_lookbehind;    // token resumes a split expression: operator
                             // lookbehind may cross the token start
  uint8_t terminator;
} ExprCfg;

typedef struct {
  ExprCfg cfg;
  uint8_t group[128];
  uint32_t group_len;
  int32_t ternary_depth;
  bool in_type;
  bool force_type;
  bool was_comment;
} ExprState;

// shouldTerminate(code, data, pos, expression); `k` is how far ahead of the
// logical position `code` sits (0 = current char).
static bool should_terminate(EStream *es, ExprState *e, int32_t code,
                             uint32_t k) {
  int32_t next = es_peek(es, k + 1);
  int32_t prev = k == 0 ? es_prev_char(es) : es_peek(es, k - 1);
  bool at_start = k == 0 && es->s->buf_len == es->start;

  switch (e->cfg.terminator) {
    case TERM_NONE:
      return false;
    case TERM_CLOSE_PAREN:
      return code == ')';
    case TERM_CLOSE_CURLY:
      return code == '}';
    case TERM_CLOSE_ANGLE:
      return code == '>';
    case TERM_PIPE:
      return code == '|';
    case TERM_HTML_ATTR_NAME:
      switch (code) {
        case ',': case '=': case '(': case '>': case '<':
          return true;
        case ':':
          return next == '=';
        case '/':
          return next == '>';
        default:
          return false;
      }
    case TERM_CONCISE_ATTR_NAME:
      switch (code) {
        case ',': case '=': case '(': case ';': case '<':
          return true;
        case ':':
          return next == '=';
        case '-':
          return next == '-' && is_ws(prev);
        default:
          return false;
      }
    case TERM_CONCISE_GROUPED_ATTR_NAME:
      switch (code) {
        case ',': case '=': case '(': case ']': case '<':
          return true;
        case ':':
          return next == '=';
        default:
          return false;
      }
    case TERM_HTML_ATTR_VALUE:
      switch (code) {
        case ',':
          return true;
        case '/':
          return next == '>';
        case '>':
          // `=>` continues unless it is the very start of the expression.
          return at_start || prev != '=';
        default:
          return false;
      }
    case TERM_CONCISE_ATTR_VALUE:
      switch (code) {
        case ',': case ';':
          return true;
        case '-':
          return next == '-' && is_ws(prev);
        default:
          return false;
      }
    case TERM_CONCISE_GROUPED_ATTR_VALUE:
      return code == ',' || code == ']';
    case TERM_HTML_TAG_VAR:
      switch (code) {
        case '|': case ',': case '=': case '(': case '>':
          return true;
        case '<':
          return !e->in_type;
        case ':':
          return next == '=';
        case '/':
          return next == '>';
        default:
          return false;
      }
    case TERM_CONCISE_TAG_VAR:
      switch (code) {
        case ',': case '=': case '|': case '(': case ';':
          return true;
        case '<':
          return !e->in_type;
        case '-':
          return next == '-';
        case ':':
          return next == '=';
        default:
          return false;
      }
    case TERM_PARAM_PATTERN:
      return code == '|' || code == ',' || code == ':' || code == '=';
    case TERM_PARAM_TYPE:
      return code == '|' || code == ',' || code == '=';
    case TERM_PARAM_DEFAULT:
      return code == '|' || code == ',';
  }
  return false;
}

static const char *const UNARY_KEYWORDS[] = {
    "async", "await", "class", "function", "new", "typeof", "void", NULL,
};
static const char *const TS_UNARY_KEYWORDS[] = {
    "async",  "await",   "class", "function", "new",   "typeof", "void",
    "asserts", "infer",  "is",    "keyof",    "readonly", "unique", NULL,
};
static const char *const BINARY_KEYWORDS[] = {
    "as", "extends", "instanceof", "in", "satisfies", NULL,
};

static int64_t look_behind_for(EStream *es, int64_t pos, const char *str) {
  int64_t len = (int64_t)strlen(str);
  int64_t end_pos = pos - len + 1;
  if (end_pos < 0) return -1;
  for (int64_t i = 0; i < len; i++) {
    if (es_at(es, end_pos + i) != (int32_t)str[i]) return -1;
  }
  return end_pos;
}

static int64_t look_behind_while_ws(EStream *es, int64_t pos) {
  int64_t i = pos;
  while (i >= 0) {
    int32_t c = es_at(es, i);
    if (c < 0 || !is_ws(c)) return i + 1;
    i--;
  }
  return 0;
}

// lookBehindForOperator: `pos` is a buf index just past the candidate.
static int64_t look_behind_for_operator(EStream *es, ExprState *e,
                                        int64_t pos) {
  int64_t cur_pos = pos - 1;
  int32_t code = es_at(es, cur_pos);

  switch (code) {
    case '&': case '*': case '^': case ':': case '=': case '!':
    case '<': case '%': case '|': case '?': case '~':
      // The operator may sit just before the token start (cur_pos == -1, eg
      // resuming a tag var type after its ":"); callers only test != -1.
      return cur_pos < 0 ? 0 : cur_pos;

    case '>':
      if (es_at(es, cur_pos - 1) == '=') return cur_pos - 1;
      return e->in_type ? -1 : (cur_pos < 0 ? 0 : cur_pos);

    case '.': {
      // `.` followed (over whitespace) by an identifier continues.
      // Chars at/past the logical position live in the lookahead queue.
      int64_t buf_len = (int64_t)es->s->buf_len;
      int64_t i = pos;
      int32_t c;
      for (;;) {
        c = i < buf_len ? es_at(es, i) : es_peek(es, (uint32_t)(i - buf_len));
        if (c >= 0 && is_ws(c)) i++;
        else break;
      }
      return (c >= 0 && is_word_code(c)) ? i : -1;
    }

    case '+':
    case '-': {
      if (es_at(es, cur_pos - 1) == code) {
        // eg "typeof++ x"
        return look_behind_for_operator(es, e,
                                        look_behind_while_ws(es, cur_pos - 2));
      }
      return cur_pos;
    }

    default: {
      const char *const *keywords =
          e->in_type ? TS_UNARY_KEYWORDS : UNARY_KEYWORDS;
      for (int i = 0; keywords[i]; i++) {
        int64_t keyword_pos = look_behind_for(es, cur_pos, keywords[i]);
        if (keyword_pos != -1) {
          int32_t before = keyword_pos > 0 ? es_at(es, keyword_pos - 1) : -1;
          return is_word_or_period_code(before) ? -1 : keyword_pos;
        }
      }
      return -1;
    }
  }
}

// lookAheadForOperator: examines chars `k` ahead. Returns chars to consume
// (the new logical position relative to now) or -1.
static int64_t look_ahead_for_operator(EStream *es, ExprState *e, uint32_t k) {
  int32_t c = es_peek(es, k);
  switch (c) {
    case '&': case '*': case '^': case '!': case '<': case '%':
    case '|': case '~': case '+': case '-':
      return (int64_t)k + 1;

    case '/': case '{': case '(': case '>': case '?': case ':': case '=':
      return (int64_t)k;  // defer to the base expression state

    case '.': {
      uint32_t k2 = k + 1;
      while (true) {
        int32_t c2 = es_peek(es, k2);
        if (c2 >= 0 && is_ws(c2)) k2++;
        else return (c2 >= 0 && is_word_code(c2)) ? (int64_t)k2 : -1;
      }
    }

    default: {
      for (int i = 0; BINARY_KEYWORDS[i]; i++) {
        const char *kw = BINARY_KEYWORDS[i];
        size_t len = strlen(kw);
        bool match_kw = true;
        for (size_t j = 0; j < len; j++) {
          if (es_peek(es, k + (uint32_t)j) != (int32_t)kw[j]) {
            match_kw = false;
            break;
          }
        }
        if (!match_kw) continue;
        int32_t c_after = es_peek(es, k + (uint32_t)len);
        if (c_after < 0 || !is_ws(c_after)) break;

        // skip whitespace; an operand must exist before EOF
        uint32_t k2 = k + (uint32_t)len + 1;
        int32_t c2;
        while ((c2 = es_peek(es, k2)) >= 0 && is_ws(c2)) k2++;
        if (c2 < 0) break;

        switch (c2) {
          case ':': case ',': case '=': case '/': case '>': case ';':
            break;
          default:
            if (!e->in_type &&
                (strcmp(kw, "as") == 0 || strcmp(kw, "satisfies") == 0)) {
              e->in_type = true;
              if (!(e->ternary_depth || e->group_len)) e->force_type = true;
            }
            return (int64_t)k2;
        }
        break;
      }
      return -1;
    }
  }
}

static bool can_follow_division(int32_t c) {
  if (is_word_code(c)) return true;
  switch (c) {
    case '`': case '\'': case '"': case '%': case ')': case '.':
    case '<': case ']': case '}':
      return true;
    default:
      return false;
  }
}

static void es_consume_ws(EStream *es) {
  while (true) {
    int32_t c = es_peek(es, 0);
    if (c >= 0 && is_ws(c)) es_next(es);
    else break;
  }
}

// checkForOperators(parser, expression, eol)
static bool check_for_operators(EStream *es, ExprState *e, bool eol) {
  if (!e->cfg.operators) return false;
  Scanner *s = es->s;

  if (look_behind_for_operator(es, e, (int64_t)s->buf_len) != -1) {
    es_consume_ws(es);
    return true;
  }

  bool terminated_by_eol = e->cfg.terminated_by_eol || is_concise(s);
  if (!(terminated_by_eol && eol)) {
    uint32_t k = 1;
    while (true) {
      int32_t c = es_peek(es, k);
      if (c >= 0 && (terminated_by_eol ? is_indent_code(c) : is_ws(c))) k++;
      else break;
    }

    int32_t next_c = es_peek(es, k);
    if (next_c >= 0 && !should_terminate(es, e, next_c, k)) {
      int64_t consume = look_ahead_for_operator(es, e, k);
      if (consume != -1) {
        for (int64_t i = 0; i < consume; i++) es_next(es);
        return true;
      }
    }
  }

  return false;
}

static bool scan_expr_inner(EStream *es, ExprState *e);

// STRING state
static bool sub_scan_string(EStream *es, int32_t quote) {
  while (true) {
    int32_t c = es_peek(es, 0);
    if (c < 0) return false;  // EOF: INVALID_STRING
    if (c == '\\') {
      es_next(es);
      if (es_peek(es, 0) < 0) return false;
      es_next(es);
    } else if (c == quote) {
      es_next(es);
      return true;
    } else {
      es_next(es);
    }
  }
}

// TEMPLATE_STRING state (shared by expressions and parsed text)
static bool sub_scan_template(EStream *es) {
  while (true) {
    int32_t c = es_peek(es, 0);
    if (c < 0) return false;  // EOF: INVALID_TEMPLATE_STRING
    if (c == '$') {
      if (es_peek(es, 1) == '{') {
        es_next(es);
        es_next(es);
        uint32_t before = es->s->buf_len;
        uint32_t prev_start = es->start;
        es->start = es->s->buf_len;
        ExprState nested;
        memset(&nested, 0, sizeof(nested));
        nested.cfg.terminator = TERM_CLOSE_CURLY;
        bool ok = scan_expr_inner(es, &nested);
        es->start = prev_start;
        if (!ok) return false;
        if (es->s->buf_len == before) return false;  // MALFORMED_PLACEHOLDER
        if (es_peek(es, 0) != '}') return false;
        es_next(es);  // skip }
      } else {
        es_next(es);
      }
    } else if (c == '\\') {
      es_next(es);
      if (es_peek(es, 0) < 0) return false;
      es_next(es);
    } else if (c == '`') {
      es_next(es);
      return true;
    } else {
      es_next(es);
    }
  }
}

// REGULAR_EXPRESSION state
static bool sub_scan_regex(EStream *es) {
  bool in_charset = false;
  while (true) {
    int32_t c = es_peek(es, 0);
    if (c < 0) return false;  // EOF: INVALID_REGULAR_EXPRESSION
    switch (c) {
      case '\\': {
        int32_t escaped = es_peek(es, 1);
        if (escaped >= 0 && !is_line(escaped)) {
          es_next(es);
          es_next(es);
          break;
        }
        return false;  // EOL while parsing regular expression
      }
      case '\n':
      case '\r':
        return false;
      case '[':
        in_charset = true;
        es_next(es);
        break;
      case ']':
        in_charset = false;
        es_next(es);
        break;
      case '/':
        es_next(es);
        if (!in_charset) return true;
        break;
      default:
        es_next(es);
        break;
    }
  }
}

// The core of EXPRESSION.parse. On success the logical position sits at the
// terminator (not consumed). Returns false on parser-error paths.
static bool scan_expr_inner(EStream *es, ExprState *e) {
  Scanner *s = es->s;

  while (true) {
    int32_t code = es_peek(es, 0);

    if (code < 0) {
      // EOF
      if (!e->group_len && (is_concise(s) || e->cfg.terminated_by_eol)) {
        return true;
      }
      return false;
    }

    // EOL handling
    if (code == '\n' || code == '\r') {
      uint32_t len = (code == '\r' && es_peek(es, 1) == '\n') ? 2 : 1;
      uint32_t before = s->buf_len;
      int32_t after_newline = es_peek(es, len);

      if (!e->group_len &&
          (e->cfg.terminated_by_eol || e->cfg.terminated_by_whitespace) &&
          (e->was_comment || !check_for_operators(es, e, true)) &&
          !(e->cfg.consume_indented && after_newline >= 0 &&
            is_indent_code(after_newline))) {
        return true;  // exit before the newline
      }

      e->was_comment = false;
      if (s->buf_len == before) {
        for (uint32_t i = 0; i < len; i++) es_next(es);
      }
      continue;
    }

    if (!e->group_len) {
      if (e->cfg.terminated_by_whitespace && is_ws(code)) {
        if (!check_for_operators(es, e, false)) return true;
        continue;
      }

      if (should_terminate(es, e, code, 0)) {
        bool was_expression = false;
        if (e->cfg.operators) {
          int64_t prev_nonws_pos =
              look_behind_while_ws(es, (int64_t)s->buf_len - 1);
          bool can_look = prev_nonws_pos > (int64_t)es->start;
          if (!can_look && e->cfg.resume_lookbehind && es->start == 0 &&
              es->before_token && !is_ws((int32_t)es->before_token)) {
            // This token continues a split expression (eg a tag var type
            // after its ":"); the parser's lookbehind would see that char.
            can_look = true;
          }
          if (can_look) {
            was_expression =
                look_behind_for_operator(es, e, prev_nonws_pos) != -1;
          }
        }
        if (!was_expression) return true;
      }
    }

    switch (code) {
      case '"':
        es_next(es);
        if (!sub_scan_string(es, '"')) return false;
        break;
      case '\'':
        es_next(es);
        if (!sub_scan_string(es, '\'')) return false;
        break;
      case '`':
        es_next(es);
        if (!sub_scan_template(es)) return false;
        break;
      case '?':
        if (e->cfg.operators && !e->group_len) {
          e->ternary_depth++;
          es_next(es);
          es_consume_ws(es);
          continue;
        }
        es_next(es);
        break;
      case ':':
        if (e->cfg.operators && !e->group_len) {
          if (e->ternary_depth) {
            e->ternary_depth--;
          } else {
            if (e->cfg.split_at_type_colon && !e->in_type) {
              // Stop before the ":" that would enter type mode: the caller
              // emits it as a separate token and resumes with in_type set,
              // reproducing the parser's exact scan as two tokens.
              return true;
            }
            e->in_type = true;
          }
          es_next(es);
          es_consume_ws(es);
          continue;
        }
        es_next(es);
        break;
      case '=':
        if (e->cfg.operators) {
          if (es_peek(es, 1) == '>') {
            if (e->in_type && !e->force_type && es_prev_nonws(es) != ')') {
              e->in_type = false;
            }
            es_next(es);  // skip =; the loop handles >
          } else if (!(e->force_type || e->group_len)) {
            e->in_type = false;
          }
          es_next(es);  // skip = (or the char after =>)
          es_consume_ws(es);
          continue;
        }
        es_next(es);
        break;
      case '/':
        switch (es_peek(es, 1)) {
          case '/': {
            es_next(es);
            es_next(es);
            // JS_COMMENT_LINE: consume to EOL (the EOL itself stays).
            while (true) {
              int32_t c2 = es_peek(es, 0);
              if (c2 < 0 || is_line(c2)) break;
              es_next(es);
            }
            e->was_comment = true;
            break;
          }
          case '*': {
            es_next(es);
            es_next(es);
            // JS_COMMENT_BLOCK: consume through */
            while (true) {
              int32_t c2 = es_peek(es, 0);
              if (c2 < 0) return false;  // MALFORMED_COMMENT
              if (c2 == '*' && es_peek(es, 1) == '/') {
                es_next(es);
                es_next(es);
                break;
              }
              es_next(es);
            }
            break;
          }
          default:
            if (can_follow_division(es_prev_nonws(es))) {
              es_next(es);
              es_consume_ws(es);
              continue;
            } else {
              es_next(es);
              if (!sub_scan_regex(es)) return false;
            }
            break;
        }
        break;
      case '(':
        if (e->group_len >= sizeof(e->group)) return false;
        e->group[e->group_len++] = ')';
        es_next(es);
        break;
      case '[':
        if (e->group_len >= sizeof(e->group)) return false;
        e->group[e->group_len++] = ']';
        es_next(es);
        break;
      case '{':
        if (e->group_len >= sizeof(e->group)) return false;
        e->group[e->group_len++] = '}';
        es_next(es);
        break;
      case '<':
        if (e->in_type) {
          if (e->group_len >= sizeof(e->group)) return false;
          e->group[e->group_len++] = '>';
          es_next(es);
        } else if (e->cfg.operators && !e->group_len) {
          es_next(es);
          es_consume_ws(es);
          continue;
        } else {
          es_next(es);
        }
        break;

      case ')':
      case ']':
      case '}':
      case '>': {
        if (code == '>') {
          if (!e->in_type || es_prev_char(es) == '=') {
            es_next(es);
            break;
          }
        }

        if (!e->group_len) return false;  // mismatched group

        uint8_t expected = e->group[--e->group_len];
        if ((int32_t)expected != code) return false;  // mismatched group

        es_next(es);
        break;
      }

      default:
        es_next(es);
        break;
    }
  }
}

// Scan an expression on an existing stream (which may hold pending
// lookahead). `*empty` reports whether nothing was consumed. The committed
// token end is maintained by automark; callers must have marked the token
// start/end fallback before queueing any lookahead.
static bool scan_expr_es(EStream *es, ExprCfg cfg, bool *empty) {
  bool prev_automark = es->automark;
  uint32_t start = es->s->buf_len;
  if (start == 0) es->before_token = es->s->cur.prev_char;
  es->automark = true;
  es->start = start;

  ExprState e;
  memset(&e, 0, sizeof(e));
  e.cfg = cfg;
  e.in_type = cfg.in_type;
  e.force_type = cfg.force_type;

  bool ok = scan_expr_inner(es, &e);
  if (empty) *empty = es->s->buf_len == start;
  es->automark = prev_automark;
  return ok;
}

// Scan a whole expression token on a fresh stream.
static bool scan_expr_token(Scanner *s, TSLexer *lexer, ExprCfg cfg,
                            bool *empty) {
  EStream es;
  es_init(&es, s, lexer, true);
  mark(s, lexer);  // empty expression: zero-width token
  bool ok = scan_expr_es(&es, cfg, empty);
  es_free(&es);
  return ok;
}

// Consume a "//" comment's chars up to (not including) the EOL/EOF.
static void es_consume_line_comment(EStream *es) {
  es_next_mark(es);  // /
  es_next_mark(es);  // /
  while (true) {
    int32_t c = es_peek(es, 0);
    if (c < 0 || is_line(c)) break;
    es_next_mark(es);
  }
}

// Consume a "/*" comment through its closing "*/"; false at EOF
// (MALFORMED_COMMENT).
static bool es_consume_block_comment(EStream *es) {
  es_next_mark(es);  // /
  es_next_mark(es);  // *
  while (true) {
    int32_t c = es_peek(es, 0);
    if (c < 0) return false;
    if (c == '*' && es_peek(es, 1) == '/') {
      es_next_mark(es);
      es_next_mark(es);
      return true;
    }
    es_next_mark(es);
  }
}

// --------------------------------------------------------------------------
// Tag bookkeeping
// --------------------------------------------------------------------------

// Tag types follow the tags API (and the vscode tmLanguage grammar this
// replaces): html void elements plus openTagOnly core tags are void;
// text:true core tags are text; statement:true core tags (and class, kept
// for class-API compatibility) are statements.
static uint8_t classify_tag_name(Scanner *s) {
  static const char *const VOID_TAGS[] = {
      "area",      "base", "br",     "col",   "hr",    "embed", "img",
      "input",     "link", "meta",   "param", "source", "track", "wbr",
      "const",     "debug", "id",    "let",   "lifecycle",
      "log",       "return", NULL,
  };
  static const char *const TEXT_TAGS[] = {
      "script", "style", "html-script", "html-style", "html-comment", NULL,
  };
  static const char *const STATEMENT_TAGS[] = {
      "import", "export", "static", "class", "server", "client", NULL,
  };
  for (int i = 0; VOID_TAGS[i]; i++) {
    if (strlen(VOID_TAGS[i]) == s->buf_len &&
        memcmp(VOID_TAGS[i], s->buf, s->buf_len) == 0)
      return TAG_VOID;
  }
  for (int i = 0; TEXT_TAGS[i]; i++) {
    if (strlen(TEXT_TAGS[i]) == s->buf_len &&
        memcmp(TEXT_TAGS[i], s->buf, s->buf_len) == 0)
      return TAG_TEXT;
  }
  for (int i = 0; STATEMENT_TAGS[i]; i++) {
    if (strlen(STATEMENT_TAGS[i]) == s->buf_len &&
        memcmp(STATEMENT_TAGS[i], s->buf, s->buf_len) == 0)
      return TAG_STATEMENT;
  }
  return TAG_HTML;
}

static bool push_tag(Scanner *s, bool concise) {
  if (s->tag_len == MAX_TAGS) return false;
  TagFrame *t = &s->tags[s->tag_len++];
  memset(t, 0, sizeof(*t));
  t->name_hash = HASH_INIT;
  t->full_hash = HASH_INIT;
  t->indent_len = s->cur.cur_indent_len;
  t->indent_hash = s->cur.cur_indent_hash;
  t->type = TAG_HTML;
  if (concise) t->flags |= TF_CONCISE;
  if (s->begin_mixed_mode || s->ending_mixed_at_eol) t->flags |= TF_BEGIN_MIXED;
  s->begin_mixed_mode = 0;
  s->ending_mixed_at_eol = 0;

  s->tag_open = 1;
  s->tag_name_done = 0;
  s->in_shorthand = 0;
  s->adjacent_lt = 0;
  s->statement_pending = 0;
  s->in_attr_group = 0;
  s->attr_active = 0;
  s->attr_stage = ATTR_UNKNOWN;
  s->attr_has_args = 0;
  s->attr_has_type_params = 0;
  s->tag_has_attrs = 0;
  s->tag_has_args = 0;
  s->tag_has_params = 0;
  s->tag_has_shorthand_id = 0;
  s->tag_pending_types = 0;
  s->expect_method_open = 0;
  s->tag_comma_continue = 0;
  return true;
}

static void pop_tag(Scanner *s) {
  if (!s->tag_len) return;
  TagFrame *t = &s->tags[--s->tag_len];
  if (t->flags & TF_BEGIN_MIXED) s->ending_mixed_at_eol = 1;
}

static bool push_content(Scanner *s, uint8_t content_kind, uint8_t flags,
                         uint16_t delimiter_len, uint16_t indent_len,
                         uint32_t indent_hash, uint8_t fresh) {
  if (s->content_len == MAX_CONTENTS) return false;
  ContentFrame *f = &s->contents[s->content_len++];
  memset(f, 0, sizeof(*f));
  f->content = content_kind;
  f->flags = flags;
  f->delimiter_len = delimiter_len;
  f->indent_len = indent_len;
  f->indent_hash = indent_hash;
  f->fresh = fresh;
  return true;
}

// beginHtmlBlock: the content kind depends on the active tag's type.
static uint8_t block_content_kind(Scanner *s) {
  TagFrame *t = top_tag(s);
  return (t && t->type == TAG_TEXT) ? CONTENT_PARSED_TEXT : CONTENT_HTML;
}

// --------------------------------------------------------------------------
// Forward declarations
// --------------------------------------------------------------------------

static bool scan_content(Scanner *s, TSLexer *lexer, const bool *valid);
static bool scan_open_tag(Scanner *s, TSLexer *lexer, const bool *valid);
static bool scan_open_tag_es(Scanner *s, TSLexer *lexer, const bool *valid,
                             EStream *es);
static bool scan_frame_content(Scanner *s, TSLexer *lexer, const bool *valid,
                               TSSymbol *result);

// After most tokens inside a concise open tag, look ahead to see whether a
// `,` follows (over any whitespace, including newlines): if so the open tag
// continues on the next line (consumeWhitespaceIfBefore(",")).
static void concise_tag_epilogue(Scanner *s, TSLexer *lexer) {
  if (!s->tag_open || !is_concise(s) || s->in_attr_group) return;
  // raw lookahead beyond the committed token end
  while (!at_eof(lexer) && is_ws(lexer->lookahead)) lexer->advance(lexer, false);
  s->tag_comma_continue = !at_eof(lexer) && lexer->lookahead == ',';
}

// --------------------------------------------------------------------------
// Open tag end transitions
// --------------------------------------------------------------------------

static void open_tag_ended(Scanner *s, bool self_closed) {
  TagFrame *t = top_tag(s);
  s->tag_open = 0;
  s->attr_active = 0;
  s->statement_pending = 0;
  s->tag_comma_continue = 0;
  if (!t) return;

  if (self_closed || t->type == TAG_VOID || t->type == TAG_STATEMENT) {
    s->pending_element_ends++;
  } else if (t->type == TAG_TEXT && !(t->flags & TF_CONCISE)) {
    // html mode <script>/<style>/<textarea>/<html-comment>
    push_content(s, CONTENT_PARSED_TEXT, 0, 0, 0, HASH_INIT, FRESH_NONE);
  }
}

// --------------------------------------------------------------------------
// Expression token configs
// --------------------------------------------------------------------------

static ExprCfg cfg_enclosed(uint8_t term) {
  ExprCfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.terminator = term;
  return cfg;
}

static ExprCfg cfg_type_expr(void) {
  ExprCfg cfg = cfg_enclosed(TERM_CLOSE_ANGLE);
  cfg.in_type = true;
  cfg.force_type = true;
  return cfg;
}

static ExprCfg cfg_attr_name(Scanner *s) {
  ExprCfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.terminated_by_whitespace = true;
  cfg.terminator = is_concise(s)
                       ? (s->in_attr_group ? TERM_CONCISE_GROUPED_ATTR_NAME
                                           : TERM_CONCISE_ATTR_NAME)
                       : TERM_HTML_ATTR_NAME;
  return cfg;
}

static ExprCfg cfg_attr_value(Scanner *s) {
  ExprCfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.operators = true;
  cfg.terminated_by_whitespace = true;
  cfg.terminator = is_concise(s)
                       ? (s->in_attr_group ? TERM_CONCISE_GROUPED_ATTR_VALUE
                                           : TERM_CONCISE_ATTR_VALUE)
                       : TERM_HTML_ATTR_VALUE;
  return cfg;
}

static ExprCfg cfg_tag_var(Scanner *s) {
  ExprCfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.operators = true;
  cfg.terminated_by_whitespace = true;
  cfg.terminator = is_concise(s) ? TERM_CONCISE_TAG_VAR : TERM_HTML_TAG_VAR;
  return cfg;
}

// --------------------------------------------------------------------------
// Close tags
// --------------------------------------------------------------------------

static bool scan_close_tag_name(Scanner *s, TSLexer *lexer, TSSymbol *result) {
  TagFrame *t = top_tag(s);
  if (!t) return false;  // EXTRA_CLOSING_TAG

  EStream es;
  es_init(&es, s, lexer, false);
  mark(s, lexer);  // name may be empty (</>)

  uint32_t h = HASH_INIT;
  uint32_t len = 0;
  while (true) {
    int32_t c = es_peek(&es, 0);
    if (c < 0) {
      es_free(&es);
      return false;  // MALFORMED_CLOSE_TAG (EOF)
    }
    if (c == '>') break;
    h = hash_push(h, c);
    len++;
    es_next_mark(&es);
  }
  es_free(&es);

  if (len > 0) {
    bool match;
    if (t->name_len > 0) {
      match = (len == t->name_len && h == t->name_hash) ||
              (len == t->full_len && h == t->full_hash);
    } else {
      match = (len == 3 && h == hash_str("div")) ||
              (len == t->full_len && h == t->full_hash);
    }
    if (!match) return false;  // MISMATCHED_CLOSING_TAG
  }

  *result = CLOSE_TAG_NAME;
  return true;
}

static bool scan_close_tag_end(Scanner *s, TSLexer *lexer, TSSymbol *result) {
  if (at_eof(lexer) || lexer->lookahead != '>') return false;
  cursor_advance(&s->cur, '>');
  lexer->advance(lexer, false);
  mark(s, lexer);

  // checkForClosingTag exits PARSED_TEXT_CONTENT.
  ContentFrame *f = top_content(s);
  if (f && f->content == CONTENT_PARSED_TEXT && !(f->flags & CF_IS_BLOCK) &&
      !(f->flags & CF_SINGLE_LINE) && f->delimiter_len == 0) {
    s->content_len--;
  }

  s->pending_element_ends++;
  *result = CLOSE_TAG_END;
  return true;
}

// --------------------------------------------------------------------------
// Placeholders & scriptlets (grammar-position driven pieces)
// --------------------------------------------------------------------------

static void accumulate_tag_name_hash(Scanner *s) {
  // Tag name interpolation source feeds the open tag's name/full hashes.
  if (!s->tag_open) return;
  TagFrame *t = top_tag(s);
  if (!t) return;
  if (!s->tag_name_done || s->in_shorthand) {
    for (uint32_t i = 0; i < s->buf_len; i++) {
      int32_t c = (int32_t)(uint8_t)s->buf[i];
      t->full_hash = hash_push(t->full_hash, c);
      t->full_len++;
      if (!s->tag_name_done) {
        t->name_hash = hash_push(t->name_hash, c);
        t->name_len++;
      }
    }
    t->flags |= TF_NAME_INTERP;
    if (!s->tag_name_done) t->type = TAG_HTML;
  }
}

static bool scan_placeholder_expr(Scanner *s, TSLexer *lexer,
                                  TSSymbol *result) {
  bool empty = false;
  if (!scan_expr_token(s, lexer, cfg_enclosed(TERM_CLOSE_CURLY), &empty)) {
    return false;
  }
  if (empty) return false;  // MALFORMED_PLACEHOLDER: expression missing
  accumulate_tag_name_hash(s);
  *result = PLACEHOLDER_EXPR;
  return true;
}

static bool scan_placeholder_end(Scanner *s, TSLexer *lexer,
                                 TSSymbol *result) {
  if (at_eof(lexer) || lexer->lookahead != '}') return false;
  s->buf_len = 0;
  buf_push(s, '}');
  cursor_advance(&s->cur, '}');
  lexer->advance(lexer, false);
  mark(s, lexer);
  accumulate_tag_name_hash(s);
  *result = PLACEHOLDER_END;
  return true;
}

static bool scan_scriptlet_body(Scanner *s, TSLexer *lexer, const bool *valid,
                                TSSymbol *result) {
  // INLINE_SCRIPT.parse: consumeWhitespace, then `{` block or statement.
  if (valid[SCRIPTLET_BLOCK_OPEN] || valid[SCRIPTLET_EXPR]) {
    while (!at_eof(lexer) && is_ws(lexer->lookahead)) skipc(s, lexer);
    if (!at_eof(lexer) && lexer->lookahead == '{' &&
        valid[SCRIPTLET_BLOCK_OPEN]) {
      cursor_advance(&s->cur, '{');
      lexer->advance(lexer, false);
      mark(s, lexer);
      *result = SCRIPTLET_BLOCK_OPEN;
      return true;
    }
    if (!valid[SCRIPTLET_EXPR]) return false;
    ExprCfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.operators = true;
    cfg.terminated_by_eol = true;
    if (!scan_expr_token(s, lexer, cfg, NULL)) return false;
    *result = SCRIPTLET_EXPR;
    return true;
  }

  if (valid[SCRIPTLET_BLOCK_EXPR] || valid[SCRIPTLET_BLOCK_CLOSE]) {
    if (valid[SCRIPTLET_BLOCK_EXPR]) {
      bool empty = false;
      if (!scan_expr_token(s, lexer, cfg_enclosed(TERM_CLOSE_CURLY), &empty)) {
        return false;
      }
      if (!empty) {
        *result = SCRIPTLET_BLOCK_EXPR;
        return true;
      }
      // fall through to the close token in the same call
    }
    // "}" plus optional (whitespace then) ";"
    if (at_eof(lexer) || lexer->lookahead != '}') return false;
    EStream es;
    es_init(&es, s, lexer, false);
    es_next_mark(&es);  // }
    // lookAtCharCodeAhead(0) === ";" || consumeWhitespaceIfBefore(";")
    uint32_t k = 0;
    while (true) {
      int32_t c = es_peek(&es, k);
      if (c == ';') {
        for (uint32_t i = 0; i <= k; i++) es_next_mark(&es);
        break;
      }
      if (c < 0 || !is_ws(c)) break;
      k++;
    }
    es_free(&es);
    *result = SCRIPTLET_BLOCK_CLOSE;
    return true;
  }

  return false;
}

// --------------------------------------------------------------------------
// Delimited blocks
// --------------------------------------------------------------------------

// Scan the BLOCK_OPEN token: a run of 2+ hyphens. Decides whether the block
// is single-line or delimited and pushes the content frame. Operates on the
// dispatcher's stream.
static bool scan_block_open(Scanner *s, EStream *es, TSSymbol *result) {
  uint16_t indent_len = s->cur.cur_indent_len;
  uint32_t indent_hash = s->cur.cur_indent_hash;

  uint32_t hyphens = 0;
  while (es_peek(es, 0) == '-') {
    es_next_mark(es);
    hyphens++;
  }
  if (hyphens < 2) return false;

  if (s->block_after_tag) {
    // OPEN_TAG's "--" handling: extend this.indent to the next line's indent
    // when it is deeper.
    s->block_after_tag = 0;
    uint32_t k = 0;
    while (true) {
      int32_t c = es_peek(es, k);
      if (c < 0 || c == '\n') break;
      k++;
    }
    if (es_peek(es, k) == '\n') {
      uint32_t start = ++k;
      uint32_t h = HASH_INIT;
      while (true) {
        int32_t c = es_peek(es, k);
        if (c >= 0 && is_indent_code(c)) {
          h = hash_push(h, c);
          k++;
        } else {
          break;
        }
      }
      if (k - start > indent_len) {
        indent_len = (uint16_t)(k - start);
        indent_hash = h;
      }
    }
  }

  // Single-line vs delimited: whitespace-only to EOL => delimited.
  uint8_t fresh = FRESH_DELIM_LINE;
  uint8_t flags = CF_IS_BLOCK;
  uint32_t k = 0;
  while (true) {
    int32_t c = es_peek(es, k);
    if (c < 0 || is_line(c)) break;  // delimited
    if (!is_ws(c)) {
      fresh = FRESH_SINGLE;
      flags |= CF_SINGLE_LINE;
      break;
    }
    k++;
  }

  if (!push_content(s, block_content_kind(s), flags,
                    (flags & CF_SINGLE_LINE) ? 0 : (uint16_t)hyphens,
                    indent_len, indent_hash, fresh)) {
    return false;
  }
  *result = BLOCK_OPEN;
  return true;
}

// --------------------------------------------------------------------------
// Content scanning (HTML_CONTENT / PARSED_TEXT_CONTENT / blocks)
// --------------------------------------------------------------------------

// checkForPlaceholder outcome
enum PhResult {
  PH_NO = 0,     // not a boundary; the caller consumes one char as text
  PH_BOUNDARY,   // a token boundary; *result/handled_now describe the token
  PH_CONSUMED,   // not a boundary, but chars were consumed; continue as text
};

// Inspect a `$`/`\\` at the current logical position (port of
// checkForPlaceholder).
//  - pending text (buf non-empty) before a boundary: caller emits TEXT and
//    this re-runs with an empty buffer.
//  - no pending text: emits the placeholder start, or an `escape` node
//    covering the whole backslash run (the harness splits it into its
//    text/omitted halves); odd runs set suppress_placeholder so the literal
//    ${ that follows is consumed as text.
// PH_NO with backslashes consumed: they were plain text; the caller's loop
// continues (buf already holds them).
static enum PhResult check_placeholder(Scanner *s, EStream *es,
                                       const bool *valid, bool *handled_now,
                                       TSSymbol *result) {
  *handled_now = false;
  if (s->suppress_placeholder) {
    // The escaped "${" is literal text: consume it and continue as text.
    if (es_peek(es, 0) == '$') {
      s->suppress_placeholder = 0;
      es_next_mark(es);                             // $
      if (es_peek(es, 0) == '!') es_next_mark(es);  // !
      if (es_peek(es, 0) == '{') es_next_mark(es);  // {
      return PH_CONSUMED;
    }
    return PH_NO;
  }

  if (s->buf_len > 0) {
    // Verify with peeks only; the pending text token's end is already
    // committed before the run.
    uint32_t ahead = 0;
    while (es_peek(es, ahead) == '\\') ahead++;
    int32_t c = es_peek(es, ahead);
    if (c != '$') return PH_NO;
    uint32_t after = ahead + 1;
    if (es_peek(es, after) == '!') after++;
    if (es_peek(es, after) != '{') return PH_NO;
    return PH_BOUNDARY;  // end TEXT; re-detect next call with empty buffer
  }

  // No pending text: consume the backslash run first (peek(0) never queues,
  // so the marks land), then verify what follows.
  uint32_t run = 0;
  while (es_peek(es, 0) == '\\') {
    es_next_mark(es);
    run++;
  }

  bool escape = true;
  uint32_t after = 1;
  bool is_placeholder = es_peek(es, 0) == '$';
  if (is_placeholder && es_peek(es, after) == '!') {
    escape = false;
    after++;
  }
  if (is_placeholder && es_peek(es, after) != '{') is_placeholder = false;

  if (!is_placeholder || run == 0) {
    if (run > 0) return PH_CONSUMED;  // plain backslashes: already in buf
    if (!is_placeholder) return PH_NO;
    // A placeholder start at the current position.
    es_next_mark(es);               // $
    if (!escape) es_next_mark(es);  // !
    es_next_mark(es);               // {
    *handled_now = true;
    *result = escape ? PLACEHOLDER_START : PLACEHOLDER_START_RAW;
    return valid[*result] ? PH_BOUNDARY : PH_NO;
  }

  // Backslashes followed by ${ / $!{: the whole run is one escape token.
  if (run % 2) s->suppress_placeholder = 1;
  *handled_now = true;
  *result = ESCAPE;
  return valid[ESCAPE] ? PH_BOUNDARY : PH_NO;
}

// Try to match the parsed-text closing tag at the current position (peeks
// only). checkForClosingTag in CLOSE_TAG.ts.
static bool peek_parsed_close_tag(Scanner *s, EStream *es) {
  TagFrame *t = top_tag(s);
  if (!t) return false;
  if (es_peek(es, 0) != '<' || es_peek(es, 1) != '/') return false;
  if (es_peek(es, 2) == '>') return true;  // </>
  uint32_t len = t->name_len;
  uint32_t h = HASH_INIT;
  for (uint32_t i = 0; i < len; i++) {
    int32_t c = es_peek(es, 2 + i);
    if (c < 0) return false;
    h = hash_push(h, c);
  }
  return h == t->name_hash && es_peek(es, 2 + len) == '>';
}

// Scan one token of html / parsed-text content (or hand off to structure).
static bool scan_frame_content(Scanner *s, TSLexer *lexer, const bool *valid,
                               TSSymbol *result) {
  ContentFrame *f = top_content(s);
  bool parsed = f->content == CONTENT_PARSED_TEXT;
  bool delim = f->delimiter_len > 0;
  bool single = (f->flags & CF_SINGLE_LINE) != 0;
  bool mixed = !delim && !single && (f->flags & CF_IS_BLOCK) == 0;

  // Fresh frames: skip past the BLOCK_OPEN line remainder.
  if (f->fresh == FRESH_SINGLE) {
    f->fresh = FRESH_NONE;
    if (!at_eof(lexer) && !is_line(lexer->lookahead)) skipc(s, lexer);
  } else if (f->fresh == FRESH_DELIM_LINE) {
    f->fresh = FRESH_NONE;
    while (!at_eof(lexer) && is_ws(lexer->lookahead) &&
           !is_line(lexer->lookahead)) {
      skipc(s, lexer);
    }
    // The first newline is not content (handleDelimitedBlockEOL first=true).
    if (!at_eof(lexer) && lexer->lookahead == '\r') skipc(s, lexer);
    if (!at_eof(lexer) && lexer->lookahead == '\n') skipc(s, lexer);
  }

  // Delimited blocks: at the start of each line, skip the block indent.
  bool at_line_start = false;
  if (delim && s->cur.line_only_ws && s->cur.cur_indent_len == 0 &&
      !at_eof(lexer)) {
    at_line_start = true;
    uint16_t n = 0;
    while (n < f->indent_len && !at_eof(lexer) &&
           is_ws(lexer->lookahead) && !is_line(lexer->lookahead)) {
      skipc(s, lexer);
      n++;
    }
  }

  EStream es;
  es_init(&es, s, lexer, false);
  mark(s, lexer);

  // A closing delimiter at the start of a line (reachable when the newline
  // was skipped as the first line of the block).
  if (at_line_start && !at_eof(lexer) && lexer->lookahead == '-') {
    uint32_t k = 0;
    bool close_match = true;
    for (uint16_t i = 0; i < f->delimiter_len; i++) {
      if (es_peek(&es, k) != '-') {
        close_match = false;
        break;
      }
      k++;
    }
    if (close_match) {
      for (uint32_t i = 0; i < k; i++) es_next(&es);
      while (true) {
        int32_t c2 = es_peek(&es, 0);
        if (c2 < 0 || is_line(c2)) break;
        if (!is_ws(c2)) {
          es_free(&es);
          return false;  // INVALID_CHARACTER after closing delimiter
        }
        es_next(&es);
      }
      es_mark(&es);
      es_free(&es);
      s->content_len--;
      *result = BLOCK_CLOSE;
      return valid[BLOCK_CLOSE];
    }
  }


#define EMIT_TEXT_OR(fallthrough_block)                       \
  do {                                                        \
    if (s->buf_len > 0) {                                     \
      es_mark(&es);                                           \
      es_free(&es);                                           \
      *result = TEXT;                                         \
      return valid[TEXT];                                     \
    }                                                         \
    fallthrough_block                                         \
  } while (0)

  while (true) {
    int32_t c = es_peek(&es, 0);

    // Newlines are consumed without marking so EOF trimming works; any other
    // char commits the token end up to here (text includes interior \n).
    if (c >= 0 && !is_line(c)) es_mark(&es);

    if (c < 0) {
      // EOF inside content. The token (if any) ends at the last mark, which
      // excludes trailing newlines (htmlEOF trimming). Structure handling
      // happens on the next call.
      if (f->pstring_quote) {
        es_free(&es);
        return false;  // EOF while parsing string expression
      }
      // Only the marked part counts: trailing newlines stay unmarked and a
      // zero-width text token would loop forever.
      bool have_text = es.marked_len > 0;
      if (have_text) {
        es_free(&es);
        *result = TEXT;
        return valid[TEXT];
      }
      if (f->flags & CF_IS_BLOCK) {
        // Trailing newlines before EOF: htmlEOF trims them from the text;
        // the block close absorbs them (block tokens produce no events).
        es_mark(&es);
        es_free(&es);
        s->content_len--;
        *result = BLOCK_CLOSE;
        return valid[BLOCK_CLOSE];
      }
      es_free(&es);
      return false;
    }

    // Parsed string state: quotes parse placeholders but nothing else.
    if (f->pstring_quote) {
      if (c == (int32_t)f->pstring_quote) {
        f->pstring_quote = 0;
        es_next_mark(&es);
        continue;
      }
      if (c == '$' || c == '\\') {
        bool handled = false;
        enum PhResult r = check_placeholder(s, &es, valid, &handled, result);
        if (r == PH_BOUNDARY) {
          es_free(&es);
          if (!handled) *result = TEXT;
          return *result != TEXT || s->buf_len > 0;
        }
        if (r == PH_NO) es_next_mark(&es);
        continue;
      }
      es_next_mark(&es);
      continue;
    }

    if (is_line(c)) {
      uint32_t len = (c == '\r' && es_peek(&es, 1) == '\n') ? 2 : 1;

      if (single) {
        // handleDelimitedEOL: single line block ends at EOL.
        EMIT_TEXT_OR({
          es_free(&es);
          s->content_len--;  // pop the block frame
          if (!valid[BLOCK_CLOSE]) return false;
          mark(s, lexer);
          *result = BLOCK_CLOSE;
          return true;
        });
      }

      if (mixed && !parsed && (s->begin_mixed_mode || s->ending_mixed_at_eol)) {
        // Mixed-mode html line ends at this newline.
        EMIT_TEXT_OR({
          es_free(&es);
          s->begin_mixed_mode = 0;
          s->ending_mixed_at_eol = 0;
          s->content_len--;
          if (!scan_content(s, lexer, valid)) return false;
          *result = lexer->result_symbol;
          return true;
        });
      }

      if (delim) {
        // handleDelimitedBlockEOL (first=false). Pending text ends here (the
        // harness merges adjacent text events, so the newline may land in
        // the next text token). A buffer of pure newlines is not emitted
        // yet: if the block ends, htmlEOF/dedent trimming drops it (the
        // BLOCK_CLOSE token absorbs the run instead).
        bool buf_has_content = false;
        for (uint32_t bi = 0; bi < s->buf_len; bi++) {
          if (!is_line((int32_t)(uint8_t)s->buf[bi])) {
            buf_has_content = true;
            break;
          }
        }
        if (buf_has_content) {
          es_mark(&es);
          es_free(&es);
          *result = TEXT;
          return valid[TEXT];
        }

        if (s->buf_len > 0) {
          // A pure-newline run: if the line after this newline closes the
          // block, everything accumulated so far is still text (the parser
          // only excludes the newline directly before the delimiter line).
          uint32_t kc = len;
          uint32_t hc = HASH_INIT;
          bool close_ahead = true;
          for (uint16_t i = 0; i < f->indent_len; i++) {
            int32_t ci = es_peek(&es, kc);
            if (ci < 0 || is_line(ci) || !is_ws(ci)) {
              close_ahead = false;
              break;
            }
            hc = hash_push(hc, ci);
            kc++;
          }
          if (close_ahead && f->indent_len > 0 && hc != f->indent_hash) {
            close_ahead = false;
          }
          if (close_ahead) {
            for (uint16_t i = 0; i < f->delimiter_len; i++) {
              if (es_peek(&es, kc) != '-') {
                close_ahead = false;
                break;
              }
              kc++;
            }
          }
          if (close_ahead) {
            es_mark(&es);
            es_free(&es);
            *result = TEXT;
            return valid[TEXT];
          }
        }

        if (f->indent_len == 0 && f->delimiter_len > 0) {
          // With an empty indent, only the closing delimiter check applies
          // (lookAheadFor("") is falsy in the parser); peek before consuming.
          bool close0 = true;
          for (uint16_t i = 0; i < f->delimiter_len; i++) {
            if (es_peek(&es, len + i) != '-') {
              close0 = false;
              break;
            }
          }
          if (!close0) {
            // text continues through the newline
            for (uint32_t i = 0; i < len; i++) es_next(&es);
            continue;
          }
        }

        // Consume the newline first (the token can still become text), then
        // inspect the next line.
        for (uint32_t i = 0; i < len; i++) es_next(&es);
        es_mark(&es);

        uint32_t k = 0;
        uint32_t h = HASH_INIT;
        bool indent_match = true;
        for (uint16_t i = 0; i < f->indent_len; i++) {
          int32_t ci = es_peek(&es, k);
          if (ci < 0 || is_line(ci) || !is_ws(ci)) {
            indent_match = false;
            break;
          }
          h = hash_push(h, ci);
          k++;
        }
        if (indent_match && f->indent_len > 0 && h != f->indent_hash) {
          indent_match = false;
        }
        bool close_match = indent_match;
        if (close_match) {
          for (uint16_t i = 0; i < f->delimiter_len; i++) {
            if (es_peek(&es, k) != '-') {
              close_match = false;
              break;
            }
            k++;
          }
        }

        if (close_match) {
          // consume indent + delimiter into the BLOCK_CLOSE token (block
          // tokens produce no events), then require whitespace to EOL.
          for (uint32_t i = 0; i < k; i++) es_next(&es);
          while (true) {
            int32_t c2 = es_peek(&es, 0);
            if (c2 < 0 || is_line(c2)) break;
            if (!is_ws(c2)) {
              es_free(&es);
              return false;  // INVALID_CHARACTER after closing delimiter
            }
            es_next(&es);
          }
          es_mark(&es);
          es_free(&es);
          s->content_len--;
          *result = BLOCK_CLOSE;
          return valid[BLOCK_CLOSE];
        }

        if (at_eof(lexer) && es_drained(&es)) {
          // Trailing newline right before EOF: htmlEOF trims it; the block
          // close absorbs it instead of a text token.
          es_free(&es);
          s->content_len--;
          *result = BLOCK_CLOSE;
          return valid[BLOCK_CLOSE];
        }

        if (f->indent_len == 0 || indent_match) {
          // Content continues on the next line: emit the newline as its own
          // text token; the next call skips the indent.
          es_free(&es);
          *result = TEXT;
          return valid[TEXT];
        }

        // Not enough indentation: blank lines stay in the block, anything
        // else ends it.
        bool blank = true;
        uint32_t k2 = 0;
        while (true) {
          int32_t c2 = es_peek(&es, k2);
          if (c2 < 0 || is_line(c2)) break;
          if (!is_ws(c2)) {
            blank = false;
            break;
          }
          k2++;
        }

        if (blank) continue;  // the whitespace joins the text

        // dedent: the block ends; the newline becomes the BLOCK_CLOSE token
        // (the parser leaves it for the concise dispatcher, but block tokens
        // produce no events so consuming it is equivalent).
        es_free(&es);
        s->content_len--;
        if (!valid[BLOCK_CLOSE]) return false;
        *result = BLOCK_CLOSE;
        return true;
      }

      // Plain content: text crosses newlines (left unmarked for EOF trim).
      for (uint32_t i = 0; i < len; i++) es_next(&es);
      continue;
    }

    if (c == '<') {
      if (parsed) {
        if (is_concise(s)) {
          // checkForClosingTag is skipped in concise mode: "</script>" in a
          // concise delimited block is plain text.
          es_next_mark(&es);
          continue;
        }
        if (s->buf_len > 0) {
          // Pending text must end before a matching close tag; peeking past
          // the mark is safe because the token end is already committed.
          if (peek_parsed_close_tag(s, &es)) {
            es_free(&es);
            *result = TEXT;
            return valid[TEXT];
          }
          es_next_mark(&es);
          continue;
        }
        // No pending text: consume "</" first so the token end can be
        // marked, then verify; an unmatched "</" just continues as text.
        if (es_peek(&es, 1) == '/') {
          es_next_mark(&es);  // <
          es_next_mark(&es);  // /
          TagFrame *t = top_tag(s);
          bool match = false;
          if (t) {
            if (es_peek(&es, 0) == '>') {
              match = true;  // </>
            } else {
              uint32_t len = t->name_len;
              uint32_t h = HASH_INIT;
              bool ok = true;
              for (uint32_t i = 0; i < len; i++) {
                int32_t ci = es_peek(&es, i);
                if (ci < 0) {
                  ok = false;
                  break;
                }
                h = hash_push(h, ci);
              }
              match = ok && h == t->name_hash &&
                      es_peek(&es, len) == '>';
            }
          }
          if (match) {
            es_free(&es);
            *result = CLOSE_TAG_START;
            return valid[CLOSE_TAG_START];
          }
          continue;  // "</" is plain text
        }
        es_next_mark(&es);
        continue;
      }

      int32_t n1 = es_peek(&es, 1);

      if (n1 == '!') {
        if (es_peek(&es, 2) == '-' && es_peek(&es, 3) == '-') {
          // <!-- ... -> (the comment ends at the first "->")
          EMIT_TEXT_OR({
            es_next_mark(&es);
            es_next_mark(&es);
            es_next_mark(&es);
            es_next_mark(&es);
            while (true) {
              int32_t c2 = es_peek(&es, 0);
              if (c2 < 0) {
                es_free(&es);
                return false;  // MALFORMED_COMMENT
              }
              if (c2 == '-' && es_peek(&es, 1) == '>') {
                es_next_mark(&es);
                es_next_mark(&es);
                break;
              }
              es_next_mark(&es);
            }
            es_free(&es);
            *result = HTML_COMMENT;
            return valid[HTML_COMMENT];
          });
        }
        // <![CDATA[ ... ]]>
        static const char CDATA_OPEN[] = "<![CDATA[";
        bool cdata = true;
        for (uint32_t i = 0; i < sizeof(CDATA_OPEN) - 1; i++) {
          if (es_peek(&es, i) != (int32_t)CDATA_OPEN[i]) {
            cdata = false;
            break;
          }
        }
        if (cdata) {
          EMIT_TEXT_OR({
            for (uint32_t i = 0; i < sizeof(CDATA_OPEN) - 1; i++)
              es_next_mark(&es);
            while (true) {
              int32_t c2 = es_peek(&es, 0);
              if (c2 < 0) {
                es_free(&es);
                return false;  // MALFORMED_CDATA
              }
              if (c2 == ']' && es_peek(&es, 1) == ']' &&
                  es_peek(&es, 2) == '>') {
                es_next_mark(&es);
                es_next_mark(&es);
                es_next_mark(&es);
                break;
              }
              es_next_mark(&es);
            }
            es_free(&es);
            *result = CDATA;
            return valid[CDATA];
          });
        }
        // <! ... > (doctype)
        EMIT_TEXT_OR({
          es_next_mark(&es);
          es_next_mark(&es);
          while (true) {
            int32_t c2 = es_peek(&es, 0);
            if (c2 < 0) {
              es_free(&es);
              return false;  // MALFORMED_DOCUMENT_TYPE
            }
            es_next_mark(&es);
            if (c2 == '>') break;
          }
          es_free(&es);
          *result = DOCTYPE;
          return valid[DOCTYPE];
        });
      }

      if (n1 == '?') {
        // <? ... > (declaration)
        EMIT_TEXT_OR({
          es_next_mark(&es);
          es_next_mark(&es);
          while (true) {
            int32_t c2 = es_peek(&es, 0);
            if (c2 < 0) {
              es_free(&es);
              return false;  // MALFORMED_DECLARATION
            }
            es_next_mark(&es);
            if (c2 == '>') break;
          }
          es_free(&es);
          *result = DECLARATION;
          return valid[DECLARATION];
        });
      }

      if (n1 == '/') {
        EMIT_TEXT_OR({
          es_next_mark(&es);
          es_next_mark(&es);
          es_free(&es);
          *result = CLOSE_TAG_START;
          return valid[CLOSE_TAG_START];
        });
      }

      if (n1 == '>' || n1 == '<' || n1 < 0 || is_ws(n1)) {
        if (n1 < 0) {
          // `<` at EOF: the parser starts an open tag and errors at EOF.
          EMIT_TEXT_OR({
            es_free(&es);
            return false;  // MALFORMED_OPEN_TAG
          });
        }
        es_next_mark(&es);
        continue;
      }

      // open tag
      EMIT_TEXT_OR({
        es_next_mark(&es);
        es_free(&es);
        if (!valid[OPEN_TAG_START]) return false;
        if (!push_tag(s, false)) return false;
        *result = OPEN_TAG_START;
        return true;
      });
    }

    if (!parsed && c == '$' && s->cur.line_only_ws) {
      // "$ " at the start of a line: an inline scriptlet.
      int32_t n1 = es_peek(&es, 1);
      if (n1 >= 0 && is_ws(n1)) {
        EMIT_TEXT_OR({
          es_next_mark(&es);  // $
          es_free(&es);
          *result = SCRIPTLET_START;
          return valid[SCRIPTLET_START];
        });
      }
    }

    if (c == '/') {
      int32_t n1 = es_peek(&es, 1);
      if (parsed) {
        if (n1 == '/') {
          // JS line comment within parsed text: part of the text, but the
          // closing tag is still recognized inside it.
          es_next_mark(&es);
          es_next_mark(&es);
          while (true) {
            int32_t c2 = es_peek(&es, 0);
            if (c2 < 0 || is_line(c2)) break;
            if (c2 == '<' && !is_concise(s) && peek_parsed_close_tag(s, &es))
              break;
            es_next_mark(&es);
          }
          continue;
        }
        if (n1 == '*') {
          // JS block comment: consumed blindly (no placeholders, no close
          // tags, delimiters not considered).
          es_next_mark(&es);
          es_next_mark(&es);
          while (true) {
            int32_t c2 = es_peek(&es, 0);
            if (c2 < 0) {
              es_free(&es);
              return false;  // MALFORMED_COMMENT
            }
            if (c2 == '*' && es_peek(&es, 1) == '/') {
              es_next_mark(&es);
              es_next_mark(&es);
              break;
            }
            es_next_mark(&es);
          }
          continue;
        }
        es_next_mark(&es);
        continue;
      }

      // html content: //... and /*...*/ comments when preceded by whitespace
      int32_t prev = es_prev_char(&es);
      if (is_ws(prev) && prev != 0 && (n1 == '/' || n1 == '*')) {
        EMIT_TEXT_OR({
          if (n1 == '/') {
            es_consume_line_comment(&es);
            es_free(&es);
            *result = LINE_COMMENT;
            return valid[LINE_COMMENT];
          }
          bool comment_ok = es_consume_block_comment(&es);
          es_free(&es);
          if (!comment_ok) return false;  // MALFORMED_COMMENT
          *result = BLOCK_COMMENT;
          return valid[BLOCK_COMMENT];
        });
      }
      es_next_mark(&es);
      continue;
    }

    if (parsed && c == '`') {
      es_next_mark(&es);
      if (!sub_scan_template(&es)) {
        es_free(&es);
        return false;
      }
      es_mark(&es);
      continue;
    }

    if (parsed && (c == '"' || c == '\'')) {
      f->pstring_quote = (uint8_t)c;
      es_next_mark(&es);
      continue;
    }

    if (c == '$' || c == '\\') {
      bool handled = false;
      enum PhResult r = check_placeholder(s, &es, valid, &handled, result);
      if (r == PH_BOUNDARY) {
        es_free(&es);
        if (!handled) *result = TEXT;
        return *result != TEXT || s->buf_len > 0;
      }
      if (r == PH_NO) es_next_mark(&es);
      continue;
    }

    es_next_mark(&es);
  }
#undef EMIT_TEXT_OR
}

// --------------------------------------------------------------------------
// Concise mode content (CONCISE_HTML_CONTENT)
// --------------------------------------------------------------------------

static bool scan_concise(Scanner *s, TSLexer *lexer, const bool *valid,
                         TSSymbol *result) {
  // Trivia: newlines and indentation.
  while (!at_eof(lexer)) {
    int32_t c = lexer->lookahead;
    if (is_line(c)) {
      s->semicolon_line = 0;
      skipc(s, lexer);
      continue;
    }
    if (is_ws(c)) {
      skipc(s, lexer);
      continue;
    }
    break;
  }

  if (at_eof(lexer)) {
    // htmlEOF: close remaining concise tags.
    TagFrame *t = top_tag(s);
    if (t) {
      if (!(t->flags & TF_CONCISE)) return false;  // MISSING_END_TAG
      if (!valid[ELEMENT_END]) return false;
      mark(s, lexer);
      pop_tag(s);
      *result = ELEMENT_END;
      return true;
    }
    return false;
  }

  int32_t c = lexer->lookahead;

  if (s->semicolon_line) {
    // Only comments may follow `tag;` on the same line (// and /* via the
    // JS comment states, <!-- via HTML_COMMENT).
    if (c != '/' && c != '<') return false;  // INVALID_CODE_AFTER_SEMICOLON
    EStream es;
    es_init(&es, s, lexer, false);
    mark(s, lexer);
    if (c == '<') {
      if (es_peek(&es, 1) == '!' && es_peek(&es, 2) == '-' &&
          es_peek(&es, 3) == '-') {
        for (int i = 0; i < 4; i++) es_next_mark(&es);
        while (true) {
          int32_t c2 = es_peek(&es, 0);
          if (c2 < 0) {
            es_free(&es);
            return false;  // MALFORMED_COMMENT
          }
          if (c2 == '-' && es_peek(&es, 1) == '>') {
            es_next_mark(&es);
            es_next_mark(&es);
            break;
          }
          es_next_mark(&es);
        }
        es_free(&es);
        *result = HTML_COMMENT;
        return valid[HTML_COMMENT];
      }
      es_free(&es);
      return false;  // INVALID_CODE_AFTER_SEMICOLON
    }
    int32_t n1 = es_peek(&es, 1);
    bool ok = false;
    if (n1 == '/') {
      es_consume_line_comment(&es);
      *result = LINE_COMMENT;
      ok = valid[LINE_COMMENT];
    } else if (n1 == '*') {
      *result = BLOCK_COMMENT;
      ok = es_consume_block_comment(&es) && valid[BLOCK_COMMENT];
    }
    es_free(&es);
    return ok;
  }

  uint16_t cur_indent = s->cur.cur_indent_len;

  // Close concise tags whose indent is >= the current line's indent.
  TagFrame *t = top_tag(s);
  if (t && (t->flags & TF_CONCISE) && t->indent_len >= cur_indent) {
    if (!valid[ELEMENT_END]) return false;
    mark(s, lexer);
    pop_tag(s);
    *result = ELEMENT_END;
    return true;
  }

  if (!t && cur_indent > 0 && c != '/') {
    return false;  // INVALID_INDENTATION: extra indentation at the beginning
  }

  if (t) {
    if (t->type == TAG_TEXT && c != '-') {
      return false;  // INVALID_LINE_START: text tags only allow "-" lines
    }
    if (!(t->flags & TF_HAS_NESTED_INDENT)) {
      t->flags |= TF_HAS_NESTED_INDENT;
      t->nested_indent_len = cur_indent;
      t->nested_indent_hash = s->cur.cur_indent_hash;
    } else if (t->nested_indent_len != cur_indent ||
               t->nested_indent_hash != s->cur.cur_indent_hash) {
      return false;  // INVALID_INDENTATION: does not match previous line
    }
  }

  if (c == '<') {
    // Begin a mixed-mode html block; HTML_CONTENT handles the "<".
    s->begin_mixed_mode = 1;
    if (!push_content(s, block_content_kind(s), 0, 0, cur_indent,
                      s->cur.cur_indent_hash, FRESH_NONE)) {
      return false;
    }
    return scan_frame_content(s, lexer, valid, result);
  }

  EStream es;
  es_init(&es, s, lexer, false);
  mark(s, lexer);
  bool ok = false;

  if (c == '$' && is_ws(es_peek(&es, 1)) && es_peek(&es, 1) >= 0) {
    es_next_mark(&es);  // $
    *result = SCRIPTLET_START_CONCISE;
    ok = valid[SCRIPTLET_START_CONCISE];
    es_free(&es);
    return ok;
  }

  if (c == '-') {
    if (es_peek(&es, 1) == '-') {
      ok = valid[BLOCK_OPEN] && scan_block_open(s, &es, result);
    }
    // else: INVALID_LINE_START: single hyphen
    es_free(&es);
    return ok;
  }

  if (c == '/') {
    int32_t n1 = es_peek(&es, 1);
    if (n1 == '/') {
      es_consume_line_comment(&es);
      *result = LINE_COMMENT;
      ok = valid[LINE_COMMENT];
    } else if (n1 == '*') {
      ok = es_consume_block_comment(&es);
      if (ok) {
        // In concise mode, only whitespace may follow a comment block; the
        // comment event still fires before the error.
        uint32_t k = 0;
        while (true) {
          int32_t c2 = es_peek(&es, k);
          if (c2 < 0 || is_line(c2)) break;
          if (!is_ws(c2)) {
            s->fail_next = 1;  // INVALID_CHARACTER after this token
            break;
          }
          k++;
        }
      }
      *result = BLOCK_COMMENT;
      ok = ok && valid[BLOCK_COMMENT];
    }
    // n1 anything else: INVALID_LINE_START
    es_free(&es);
    return ok;
  }

  // A concise open tag.
  if (!push_tag(s, true)) {
    es_free(&es);
    return false;
  }
  ok = scan_open_tag_es(s, lexer, valid, &es);
  if (ok) *result = lexer->result_symbol;
  es_free(&es);
  return ok;
}

// --------------------------------------------------------------------------
// Open tags (OPEN_TAG / TAG_NAME / ATTRIBUTE)
// --------------------------------------------------------------------------

// Is this character a TAG_NAME terminator?
static bool is_tag_name_terminator(Scanner *s, int32_t c, int32_t next) {
  if (c < 0 || is_ws(c)) return true;
  switch (c) {
    case '=': case '(': case '/': case '|': case '<': case ',':
      return true;
    case ':':
      return next == '=';
    case ';': case '[':
      return is_concise(s);
    case '>':
      return !is_concise(s);
    default:
      return false;
  }
}

// Finish the current tag name segment (called when a terminator is seen).
static void finalize_tag_name(Scanner *s, int32_t stop_char) {
  if (!s->tag_name_done) {
    s->tag_name_done = 1;
    s->adjacent_lt = stop_char == '<';
    TagFrame *t = top_tag(s);
    if (t && t->type == TAG_STATEMENT && stop_char != '.' &&
        stop_char != '#') {
      s->statement_pending = 1;
    }
  }
  s->in_shorthand = 0;
}

// Scan one tag name fragment / shorthand-start / interp-start token.
// Returns 0 on failure, 1 when a token was produced, 2 when the name section
// ended without producing a token (continue with tag-level dispatch).
static int scan_tag_name_token(Scanner *s, const bool *valid, EStream *es,
                               TSSymbol *result) {
  TagFrame *t = top_tag(s);

  int32_t c = es_peek(es, 0);
  int32_t n1 = es_peek(es, 1);

  if (c == '$' && n1 == '{') {
    es_next_mark(es);
    es_next_mark(es);
    if (t) {
      t->full_hash = hash_push(hash_push(t->full_hash, '$'), '{');
      t->full_len += 2;
      if (!s->tag_name_done) {
        t->name_hash = hash_push(hash_push(t->name_hash, '$'), '{');
        t->name_len += 2;
        t->flags |= TF_NAME_INTERP;
      }
    }
    *result = INTERP_START;
    return valid[INTERP_START] ? 1 : 0;
  }

  if (c == '.' || c == '#') {
    bool had_name_parts =
        !s->tag_name_done &&
        (t ? (t->name_len > 0 || (t->flags & TF_NAME_INTERP)) : false);
    if (!s->tag_name_done && !had_name_parts && !s->in_shorthand) {
      // Empty tag name (eg `<.foo>`): emit the zero-width marker first and
      // stay in the name section so the shorthand follows.
      s->tag_name_done = 1;
      s->in_shorthand = 1;
      *result = TAG_NAME_EMPTY;
      return valid[TAG_NAME_EMPTY] ? 1 : 0;
    }
    finalize_tag_name(s, c);
    if (c == '#') {
      if (s->tag_has_shorthand_id) {
        return 0;  // INVALID_TAG_SHORTHAND: multiple IDs
      }
      s->tag_has_shorthand_id = 1;
    }
    s->in_shorthand = 1;
    es_next_mark(es);
    if (t) {
      t->full_hash = hash_push(t->full_hash, c);
      t->full_len++;
    }
    *result = c == '#' ? SHORTHAND_ID_START : SHORTHAND_CLASS_START;
    return valid[*result] ? 1 : 0;
  }

  if (c < 0 || is_line(c) || is_tag_name_terminator(s, c, n1)) {
    if (!s->tag_name_done && (!t || t->name_len == 0) &&
        !(t && (t->flags & TF_NAME_INTERP))) {
      finalize_tag_name(s, c);
      *result = TAG_NAME_EMPTY;
      return valid[TAG_NAME_EMPTY] ? 1 : 0;
    }
    finalize_tag_name(s, c);
    return 2;
  }

  // A fragment: consume until a terminator / `.` / `#` / `${`.
  bool first_part = !s->tag_name_done && t && t->name_len == 0 &&
                    !(t->flags & TF_NAME_INTERP) && !s->in_shorthand;
  while (true) {
    c = es_peek(es, 0);
    n1 = es_peek(es, 1);
    if (c < 0 || c == '.' || c == '#' || (c == '$' && n1 == '{') ||
        is_line(c) || is_tag_name_terminator(s, c, n1)) {
      break;
    }
    if (t) {
      t->full_hash = hash_push(t->full_hash, c);
      t->full_len++;
      if (!s->tag_name_done) {
        t->name_hash = hash_push(t->name_hash, c);
        t->name_len++;
      }
    }
    es_next_mark(es);
  }

  if (first_part && t && !(c == '$' && n1 == '{')) {
    // The whole name was this single fragment: classify it.
    t->type = classify_tag_name(s);
  }

  if (c != '.' && c != '#' && !(c == '$' && n1 == '{')) {
    finalize_tag_name(s, c);
  }

  *result = TAG_NAME_FRAGMENT;
  return valid[TAG_NAME_FRAGMENT] ? 1 : 0;
}

// Statement tags (import/export/static/class): the expression begins right
// after the tag name with no trivia skipping (it consumes its own
// whitespace, EOL continuation via indentation, etc).
static bool scan_statement_tail(Scanner *s, TSLexer *lexer, const bool *valid,
                                EStream *es) {
  TagFrame *t = top_tag(s);
  s->statement_pending = 0;
  if (!t) return false;
  if (!(t->flags & TF_CONCISE)) return false;  // RESERVED_TAG_NAME
  if (s->tag_len > 1) return false;            // ROOT_TAG_ONLY
  if (!valid[STATEMENT_EXPR]) return false;

  ExprCfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.operators = true;
  cfg.terminated_by_eol = true;
  cfg.consume_indented = true;

  // lookAheadFor("declare " | "interface " | "type ") from pos + 1.
  static const char *const PREFIXES[] = {"declare ", "interface ", "type ",
                                         NULL};
  for (int i = 0; PREFIXES[i]; i++) {
    const char *p = PREFIXES[i];
    bool match_p = true;
    for (uint32_t j = 0; p[j]; j++) {
      if (es_peek(es, 1 + j) != (int32_t)p[j]) {
        match_p = false;
        break;
      }
    }
    if (match_p) {
      cfg.in_type = true;
      cfg.force_type = true;
      break;
    }
  }

  if (!scan_expr_es(es, cfg, NULL)) return false;
  lexer->result_symbol = STATEMENT_EXPR;
  concise_tag_epilogue(s, lexer);
  return true;
}

// The real open-tag scanner. `es` may hold pending lookahead when inherited
// from the concise dispatcher.
static bool scan_open_tag_es(Scanner *s, TSLexer *lexer, const bool *valid,
                             EStream *es) {
  TagFrame *t = top_tag(s);
  if (!t) return false;

  // ---- statement tags: the expression starts immediately after the name
  if (s->statement_pending) {
    return scan_statement_tail(s, lexer, valid, es);
  }

  // ---- grammar-position expressions (no trivia before them)
  if (valid[ARGS_EXPR] && !valid[ATTR_NAME]) {
    bool empty = false;
    if (!scan_expr_es(es, cfg_enclosed(TERM_CLOSE_PAREN), &empty)) {
      return false;
    }
    if (!empty) {
      lexer->result_symbol = ARGS_EXPR;
      return true;
    }
    // empty: fall through to ARGS_CLOSE below
  }
  if (valid[ARGS_CLOSE] && !valid[ATTR_NAME]) {
    if (es_peek(es, 0) != ')') return false;
    es_next_mark(es);
    if (s->attr_active) {
      s->attr_has_args = 1;
      s->attr_stage = ATTR_ARGUMENT_STAGE;
      // An attribute cannot have both type parameters and arguments unless
      // a method body follows.
      if (s->attr_has_type_params) {
        uint32_t k = 0;
        int32_t c2;
        while ((c2 = es_peek(es, k)) >= 0 && is_ws(c2)) k++;
        if (c2 != '{') return false;
      }
    } else {
      s->tag_has_args = 1;
      // Tag args followed by "{" become a default-attribute method
      // (OPEN_TAG.return: consumeWhitespaceIfBefore("{")).
      uint32_t k = 0;
      int32_t c2;
      while ((c2 = es_peek(es, k)) >= 0 && is_ws(c2)) k++;
      if (c2 == '{') {
        s->attr_active = 1;
        s->attr_has_args = 1;
        s->attr_stage = ATTR_ARGUMENT_STAGE;
        s->tag_has_attrs = 1;
      }
    }
    lexer->result_symbol = ARGS_CLOSE;
    concise_tag_epilogue(s, lexer);
    return true;
  }
  // Tag params: the parser scans one opaque expression to the closing "|";
  // the scanner splits it into pattern/type/default pieces at depth-0
  // boundaries (",", ":", "=") so each piece can highlight properly. The
  // union of the sub-scans consumes exactly the parser's chars.
  if (valid[PARAM_PATTERN] && !valid[ATTR_NAME]) {
    if (es_peek(es, 0) != '|' || !valid[PARAMS_CLOSE]) {
      bool empty = false;
      if (!scan_expr_es(es, cfg_enclosed(TERM_PARAM_PATTERN), &empty)) {
        return false;
      }
      lexer->result_symbol = PARAM_PATTERN;  // may be zero-width
      return true;
    }
    // "|" right away: an empty params list; fall through to PARAMS_CLOSE.
  }
  if ((valid[PARAM_COLON] || valid[PARAM_EQ] || valid[PARAM_COMMA] ||
       valid[PARAM_TYPE] || valid[PARAM_DEFAULT] || valid[PARAMS_CLOSE]) &&
      !valid[ATTR_NAME]) {
    if (valid[PARAM_TYPE]) {
      bool empty = false;
      if (!scan_expr_es(es, cfg_enclosed(TERM_PARAM_TYPE), &empty)) {
        return false;
      }
      lexer->result_symbol = PARAM_TYPE;  // may be zero-width
      return true;
    }
    if (valid[PARAM_DEFAULT]) {
      bool empty = false;
      if (!scan_expr_es(es, cfg_enclosed(TERM_PARAM_DEFAULT), &empty)) {
        return false;
      }
      lexer->result_symbol = PARAM_DEFAULT;  // may be zero-width
      return true;
    }
    int32_t pc = es_peek(es, 0);
    if (pc == ':' && valid[PARAM_COLON]) {
      es_next_mark(es);
      lexer->result_symbol = PARAM_COLON;
      return true;
    }
    if (pc == '=' && valid[PARAM_EQ]) {
      es_next_mark(es);
      lexer->result_symbol = PARAM_EQ;
      return true;
    }
    if (pc == ',' && valid[PARAM_COMMA]) {
      es_next_mark(es);
      lexer->result_symbol = PARAM_COMMA;
      return true;
    }
    if (pc == '|' && valid[PARAMS_CLOSE]) {
      es_next_mark(es);
      lexer->result_symbol = PARAMS_CLOSE;
      concise_tag_epilogue(s, lexer);
      return true;
    }
    // The sub-scans only stop at the separators above, so reaching here is
    // EOF inside an unterminated params list: MALFORMED_OPEN_TAG.
    return false;
  }
  if (valid[TYPE_EXPR] && !valid[ATTR_NAME]) {
    bool empty = false;
    if (!scan_expr_es(es, cfg_type_expr(), &empty)) return false;
    if (!empty) {
      lexer->result_symbol = TYPE_EXPR;
      return true;
    }
  }
  if (valid[TYPES_CLOSE] && !valid[ATTR_NAME]) {
    if (es_peek(es, 0) != '>') return false;
    es_next_mark(es);
    if (s->types_was_args) {
      // tagTypeArgs: nothing to validate.
      s->types_was_args = 0;
    } else if (s->attr_active) {
      // Attribute type params: "(" must follow.
      s->attr_has_type_params = 1;
      s->expect_method_open = 1;
    } else if (true) {
      // Tag-level type params must be followed by | or ( (INVALID_TAG_TYPES
      // fires when the types expression returns, before anything else).
      uint32_t k = 0;
      int32_t c2;
      while ((c2 = es_peek(es, k)) >= 0 && is_ws(c2)) k++;
      if (c2 == '|') {
        if (s->tag_has_params) return false;
      } else if (c2 == '(') {
        if (s->attr_has_type_params || s->tag_has_params || s->tag_has_args) {
          return false;
        }
      } else {
        return false;  // INVALID_TAG_TYPES
      }
      s->tag_pending_types = 1;
    }
    s->adjacent_lt = 0;
    lexer->result_symbol = TYPES_CLOSE;
    concise_tag_epilogue(s, lexer);
    return true;
  }
  if (valid[METHOD_BODY_EXPR] && !valid[ATTR_NAME]) {
    bool empty = false;
    if (!scan_expr_es(es, cfg_enclosed(TERM_CLOSE_CURLY), &empty)) {
      return false;
    }
    if (!empty) {
      lexer->result_symbol = METHOD_BODY_EXPR;
      return true;
    }
  }
  if (valid[METHOD_BODY_CLOSE] && !valid[ATTR_NAME]) {
    if (es_peek(es, 0) != '}') return false;
    es_next_mark(es);
    // onAttrMethod fires and the attribute exits.
    s->attr_active = 0;
    s->attr_stage = ATTR_UNKNOWN;
    s->attr_has_args = 0;
    s->attr_has_type_params = 0;
    s->tag_has_attrs = 1;
    lexer->result_symbol = METHOD_BODY_CLOSE;
    concise_tag_epilogue(s, lexer);
    return true;
  }
  if (valid[VAR_PATTERN] && !valid[ATTR_NAME]) {
    int32_t c0 = es_peek(es, 0);
    if (c0 >= 0 && is_ws(c0)) return false;  // MISSING_TAG_VARIABLE
    ExprCfg cfg = cfg_tag_var(s);
    cfg.split_at_type_colon = true;
    bool empty = false;
    if (!scan_expr_es(es, cfg, &empty)) return false;
    if (empty) return false;  // MISSING_TAG_VARIABLE
    lexer->result_symbol = VAR_PATTERN;
    concise_tag_epilogue(s, lexer);
    return true;
  }
  if (valid[VAR_TYPE] && !valid[ATTR_NAME]) {
    // Resume the parser's expression scan after the ":" (in type mode; the
    // ":" lookbehind keeps whitespace continuation working).
    ExprCfg cfg = cfg_tag_var(s);
    cfg.in_type = true;
    cfg.resume_lookbehind = true;
    if (!scan_expr_es(es, cfg, NULL)) return false;
    lexer->result_symbol = VAR_TYPE;  // may be zero-width
    concise_tag_epilogue(s, lexer);
    return true;
  }
  if (valid[VAR_COLON] && es_drained(es)) {
    // The pattern scan stopped before the ":" type annotation, possibly
    // with whitespace in between (the parser's operator continuation covers
    // `a : T`; in concise mode it does not cross newlines). The whitespace
    // is skipped via the raw lookahead so the no-colon path stays clean for
    // the normal trivia/dispatch flow.
    bool concise_var = is_concise(s);
    while (!at_eof(lexer) && is_ws(lexer->lookahead) &&
           (!concise_var || !is_line(lexer->lookahead))) {
      skipc(s, lexer);
    }
    if (!at_eof(lexer) && lexer->lookahead == ':' && es_peek(es, 1) != '=') {
      es_next_mark(es);
      lexer->result_symbol = VAR_COLON;
      return true;
    }
    // ":=" (or anything else) continues with the regular dispatch below.
  }
  if (valid[ATTR_VALUE_EXPR] && !valid[ATTR_NAME]) {
    // After = / := the parser consumes whitespace; after ... it does not.
    if (s->attr_stage == ATTR_VALUE_STAGE) {
      while (es_drained(es) && !at_eof(lexer) && is_ws(lexer->lookahead)) {
        skipc(s, lexer);
      }
      es_mark(es);
    }
    bool empty = false;
    if (!scan_expr_es(es, cfg_attr_value(s), &empty)) return false;
    if (empty) return false;  // INVALID_ATTRIBUTE_VALUE / missing value
    // The attribute exits after its value.
    s->attr_active = 0;
    s->attr_stage = ATTR_UNKNOWN;
    s->attr_has_args = 0;
    s->attr_has_type_params = 0;
    lexer->result_symbol = ATTR_VALUE_EXPR;
    concise_tag_epilogue(s, lexer);
    return true;
  }

  bool concise = is_concise(s);

  // ---- trivia loop (only when no lookahead is pending)
  while (es_drained(es)) {
    if (at_eof(lexer)) {
      if (!s->tag_name_done || s->in_shorthand) finalize_tag_name(s, -1);
      s->statement_pending = 0;  // an empty statement expression: no events
      if (!concise) return false;          // MALFORMED_OPEN_TAG: EOF
      if (s->in_attr_group) return false;  // EOF within attribute group
      if (!valid[CONCISE_OPEN_TAG_END]) return false;
      mark(s, lexer);
      open_tag_ended(s, false);
      lexer->result_symbol = CONCISE_OPEN_TAG_END;
      return true;
    }

    int32_t cc = lexer->lookahead;

    if ((!s->tag_name_done || s->in_shorthand) &&
        (is_ws(cc) || cc == ',')) {
      // TAG_NAME terminates at whitespace/comma before any trivia skipping.
      finalize_tag_name(s, cc);
      if (s->statement_pending) {
        return scan_statement_tail(s, lexer, valid, es);
      }
      continue;
    }

    if (is_line(cc)) {
      s->attr_active = 0;
      s->attr_stage = ATTR_UNKNOWN;
      s->attr_has_args = 0;
      s->attr_has_type_params = 0;
      if (!concise || s->in_attr_group) {
        skipc(s, lexer);
        continue;
      }
      if (s->tag_comma_continue) {
        // consumeWhitespaceIfBefore(","): skip to and through the comma.
        while (!at_eof(lexer) && is_ws(lexer->lookahead)) skipc(s, lexer);
        if (!at_eof(lexer) && lexer->lookahead == ',') skipc(s, lexer);
        while (!at_eof(lexer) && is_ws(lexer->lookahead)) skipc(s, lexer);
        s->tag_comma_continue = 0;
        continue;
      }
      if (!valid[CONCISE_OPEN_TAG_END]) return false;
      mark(s, lexer);  // zero-width at the newline
      open_tag_ended(s, false);
      lexer->result_symbol = CONCISE_OPEN_TAG_END;
      return true;
    }

    if (is_ws(cc)) {
      skipc(s, lexer);
      continue;
    }

    if (cc == ',') {
      skipc(s, lexer);
      while (!at_eof(lexer) && is_ws(lexer->lookahead)) skipc(s, lexer);
      continue;
    }

    break;
  }

  es_mark(es);  // the token starts here
  int32_t c = es_peek(es, 0);
#ifdef TSMARKO_DEBUG
  fprintf(stderr,
          "[open-tag dispatch c=%c active=%d stage=%d args=%d expect=%d nd=%d ish=%d]\n",
          c < 0 ? '?' : (char)c, s->attr_active, s->attr_stage,
          s->attr_has_args, s->expect_method_open, s->tag_name_done,
          s->in_shorthand);
#endif
  if (c < 0) return false;

  // ---- tag name section (TAG_NAME handles its chars before OPEN_TAG does)
  if (!s->tag_name_done || s->in_shorthand) {
    TSSymbol name_sym = 0;
    int r = scan_tag_name_token(s, valid, es, &name_sym);
    if (r == 0) return false;
    if (r == 1) {
      lexer->result_symbol = name_sym;
      concise_tag_epilogue(s, lexer);
      return true;
    }
    // r == 2: the name section ended without a token; redispatch.
    return scan_open_tag_es(s, lexer, valid, es);
  }

  // ---- concise structure
  if (concise) {
    if (c == ';' && !s->in_attr_group) {
      if (!valid[CONCISE_OPEN_TAG_END]) return false;
      es_next_mark(es);
      s->semicolon_line = 1;
      open_tag_ended(s, false);
      lexer->result_symbol = CONCISE_OPEN_TAG_END;
      return true;
    }
    if (c == '-' && s->tag_name_done) {
      // OPEN_TAG sees "-" whenever an attribute is not mid-expression:
      // "--" ends the tag and begins a delimited block; "-" alone is an
      // error inside an open tag.
      if (es_peek(es, 1) != '-') {
        return false;  // '"-" not allowed as first character of attr name'
      }
      s->attr_active = 0;
      s->attr_stage = ATTR_UNKNOWN;
      s->attr_has_args = 0;
      s->attr_has_type_params = 0;
      if (s->in_attr_group) return false;  // group was not properly ended
      if (!valid[CONCISE_OPEN_TAG_END]) return false;
      // zero-width at the first hyphen (the mark from the trivia loop).
      s->block_after_tag = 1;
      open_tag_ended(s, false);
      lexer->result_symbol = CONCISE_OPEN_TAG_END;
      return true;
    }
    if (c == '[' && s->tag_name_done) {
      // OPEN_TAG handles "[" before attribute dispatch in concise mode.
      if (s->in_attr_group) return false;  // nested group: MALFORMED_OPEN_TAG
      if (!valid[ATTR_GROUP_OPEN]) return false;
      s->attr_active = 0;
      s->attr_stage = ATTR_UNKNOWN;
      s->attr_has_args = 0;
      s->attr_has_type_params = 0;
      es_next_mark(es);
      s->in_attr_group = 1;
      lexer->result_symbol = ATTR_GROUP_OPEN;
      return true;
    }
    if (c == ']') {
      if (!s->in_attr_group) return false;
      if (!valid[ATTR_GROUP_CLOSE]) return false;
      s->attr_active = 0;
      s->attr_stage = ATTR_UNKNOWN;
      s->attr_has_args = 0;
      s->attr_has_type_params = 0;
      es_next_mark(es);
      s->in_attr_group = 0;
      lexer->result_symbol = ATTR_GROUP_CLOSE;
      concise_tag_epilogue(s, lexer);
      return true;
    }
  } else {
    if (c == '>') {
      if (!valid[OPEN_TAG_END]) return false;
      es_next_mark(es);
      open_tag_ended(s, false);
      lexer->result_symbol = OPEN_TAG_END;
      return true;
    }
  }

  // ---- "/" disambiguation: />, //, /*, tag var, attr content
  if (c == '/') {
    int32_t n1 = es_peek(es, 1);

    if (!concise && n1 == '>') {
      if (!valid[OPEN_TAG_END_SELF]) return false;
      es_next_mark(es);
      es_next_mark(es);
      open_tag_ended(s, true);
      lexer->result_symbol = OPEN_TAG_END_SELF;
      return true;
    }

    if (n1 == '/' || n1 == '*') {
      // Comments inside open tags produce no events: a hidden token.
      if (!valid[TAG_COMMENT]) return false;
      if (n1 == '/') {
        es_consume_line_comment(es);
      } else if (!es_consume_block_comment(es)) {
        return false;  // MALFORMED_COMMENT
      }
      lexer->result_symbol = TAG_COMMENT;
      concise_tag_epilogue(s, lexer);
      return true;
    }
  }

  if (s->expect_method_open && c != '(') {
#ifdef TSMARKO_DEBUG
    fprintf(stderr, "[expect_method_open fail c=%c]\n", (char)c);
#endif
    return false;  // INVALID_ATTR_TYPE_PARAMS
  }
  if (s->tag_pending_types) {
    if (c == '|') {
      if (s->tag_has_params) return false;  // INVALID_TAG_TYPES
      s->tag_pending_types = 0;
    } else if (c == '(') {
      if (s->attr_has_type_params || s->tag_has_params || s->tag_has_args) {
        return false;  // INVALID_TAG_TYPES
      }
      s->tag_pending_types = 0;
      s->attr_active = 1;
      s->attr_has_type_params = 1;
      s->attr_stage = ATTR_UNKNOWN;
    } else {
      return false;  // INVALID_TAG_TYPES
    }
  }

  // ---- attribute / tag-part dispatch
  bool attr_context = s->tag_has_attrs || s->attr_active;

  if (!attr_context) {
    switch (c) {
      case '/': {
        s->adjacent_lt = 0;
        if (!valid[TAG_VAR_START]) return false;
        es_next_mark(es);
        lexer->result_symbol = TAG_VAR_START;
        return true;
      }
      case '(': {
        if (s->tag_has_args) {
          return false;  // INVALID_TAG_ARGUMENT: only one argument
        }
        s->adjacent_lt = 0;
        if (!valid[ARGS_OPEN]) return false;
        es_next_mark(es);
        lexer->result_symbol = ARGS_OPEN;
        return true;
      }
      case '|': {
        if (s->tag_has_params) return false;  // INVALID_TAG_PARAMS
        s->adjacent_lt = 0;
        s->tag_has_params = 1;
        if (!valid[PARAMS_OPEN]) return false;
        es_next_mark(es);
        lexer->result_symbol = PARAMS_OPEN;
        return true;
      }
      case '<': {
        bool adjacent = s->adjacent_lt != 0;
        s->adjacent_lt = 0;
        s->types_was_args = adjacent;
        TSSymbol open_sym = adjacent ? TYPE_ARGS_OPEN : TYPE_PARAMS_OPEN;
        if (!valid[open_sym]) return false;
        es_next_mark(es);
        lexer->result_symbol = open_sym;
        return true;
      }
      default:
        s->tag_has_attrs = 1;
        s->attr_active = 1;
        s->attr_stage = ATTR_UNKNOWN;
        s->attr_has_args = 0;
        s->attr_has_type_params = 0;
        break;
    }
  } else if (!s->attr_active) {
    s->attr_active = 1;
    s->attr_stage = ATTR_UNKNOWN;
    s->attr_has_args = 0;
    s->attr_has_type_params = 0;
  }

  s->adjacent_lt = 0;

  // ---- ATTRIBUTE.parse dispatch
  if (c == '=') {
    if (!valid[ATTR_EQ]) return false;
    es_next_mark(es);
    s->attr_stage = ATTR_VALUE_STAGE;
    lexer->result_symbol = ATTR_EQ;
    return true;
  }
  if (c == ':' && es_peek(es, 1) == '=') {
    if (!valid[ATTR_BOUND_EQ]) return false;
    es_next_mark(es);
    es_next_mark(es);
    s->attr_stage = ATTR_VALUE_STAGE;
    lexer->result_symbol = ATTR_BOUND_EQ;
    return true;
  }
  if (c == '.' && es_peek(es, 1) == '.' && es_peek(es, 2) == '.') {
    if (!valid[ATTR_SPREAD_START]) return false;
    es_next_mark(es);
    es_next_mark(es);
    es_next_mark(es);
    s->attr_stage = ATTR_BLOCK_STAGE;  // marker: no ws-skip before the value
    lexer->result_symbol = ATTR_SPREAD_START;
    return true;
  }
  if (c == '(') {
    if (s->attr_has_args) return false;  // one set of arguments only
    if (!valid[ARGS_OPEN]) return false;
    s->expect_method_open = 0;
    es_next_mark(es);
    lexer->result_symbol = ARGS_OPEN;
    return true;
  }
  if (c == '<' && s->attr_stage == ATTR_NAME_STAGE) {
    if (!valid[TYPE_PARAMS_OPEN]) return false;
    s->types_was_args = 0;
    es_next_mark(es);
    lexer->result_symbol = TYPE_PARAMS_OPEN;
    return true;
  }
  if (c == '{' && s->attr_has_args) {
    if (!valid[METHOD_BODY_OPEN]) return false;
    s->expect_method_open = 0;
    es_next_mark(es);
    lexer->result_symbol = METHOD_BODY_OPEN;
    return true;
  }

  if (s->attr_stage != ATTR_UNKNOWN) {
    // The attribute ends; what follows starts a fresh attribute.
    s->attr_active = 1;
    s->attr_stage = ATTR_UNKNOWN;
    s->attr_has_args = 0;
    s->attr_has_type_params = 0;
    s->tag_has_attrs = 1;
  }

  if (c == '<') return false;  // INVALID_ATTRIBUTE_NAME
  {
    bool empty = false;
    if (!scan_expr_es(es, cfg_attr_name(s), &empty)) return false;
    if (empty) return false;
    if (!valid[ATTR_NAME]) return false;
    s->attr_stage = ATTR_NAME_STAGE;
    lexer->result_symbol = ATTR_NAME;
    concise_tag_epilogue(s, lexer);
    return true;
  }
}

static bool scan_open_tag(Scanner *s, TSLexer *lexer, const bool *valid) {
  EStream es;
  es_init(&es, s, lexer, false);
  mark(s, lexer);
  bool ok = scan_open_tag_es(s, lexer, valid, &es);
  es_free(&es);
  return ok;
}

// --------------------------------------------------------------------------
// Content dispatch
// --------------------------------------------------------------------------

static bool scan_content(Scanner *s, TSLexer *lexer, const bool *valid) {
  TSSymbol result = 0;

  if (s->block_after_tag) {
    // A concise open tag ended at "--": BEGIN_DELIMITED_HTML_BLOCK follows
    // immediately (the concise dispatcher never sees these hyphens).
    if (!valid[BLOCK_OPEN]) return false;
    EStream es;
    es_init(&es, s, lexer, false);
    mark(s, lexer);
    bool ok = scan_block_open(s, &es, &result);
    es_free(&es);
    if (!ok) return false;
    lexer->result_symbol = result;
    return true;
  }

  ContentFrame *f = top_content(s);

  if (!f) {
    if (!scan_concise(s, lexer, valid, &result)) return false;
    lexer->result_symbol = result;
    return true;
  }

  // EOF handling for frames.
  if (at_eof(lexer) && f->fresh == FRESH_NONE) {
    if (f->flags & CF_IS_BLOCK) {
      if (!valid[BLOCK_CLOSE]) return false;
      mark(s, lexer);
      s->content_len--;
      lexer->result_symbol = BLOCK_CLOSE;
      return true;
    }
    if (f->content == CONTENT_PARSED_TEXT) {
      // <script> etc reached EOF: MISSING_END_TAG
      return false;
    }
    // mixed html line at EOF: pop silently and continue.
    s->begin_mixed_mode = 0;
    s->ending_mixed_at_eol = 0;
    s->content_len--;
    return scan_content(s, lexer, valid);
  }

  if (!scan_frame_content(s, lexer, valid, &result)) return false;
  lexer->result_symbol = result;
  return true;
}

// --------------------------------------------------------------------------
// Entry points
// --------------------------------------------------------------------------

static bool scan_main(Scanner *s, TSLexer *lexer, const bool *valid) {
  if (valid[ERROR_SENTINEL]) return false;  // error recovery: give up
  if (s->fail_next) return false;           // deferred error

  if (!s->started) {
    if (!at_eof(lexer) && lexer->lookahead == 0xFEFF) skipc(s, lexer);
    s->started = 1;
    s->cur.line_only_ws = 1;
    s->cur.cur_indent_hash = HASH_INIT;
  }

  // Structural pendings first.
  if (s->pending_element_ends) {
    if (!valid[ELEMENT_END]) return false;
    mark(s, lexer);
    s->pending_element_ends--;
    pop_tag(s);
    lexer->result_symbol = ELEMENT_END;
    return true;
  }

  // Grammar-position-driven pieces (TEXT is never valid at these positions).
  if (valid[CLOSE_TAG_NAME] && !valid[TEXT] && !valid[ATTR_NAME]) {
    TSSymbol result = 0;
    if (!scan_close_tag_name(s, lexer, &result)) return false;
    lexer->result_symbol = result;
    return true;
  }
  if (valid[CLOSE_TAG_END] && !valid[TEXT] && !valid[ATTR_NAME]) {
    TSSymbol result = 0;
    if (!scan_close_tag_end(s, lexer, &result)) return false;
    lexer->result_symbol = result;
    return true;
  }
  if (valid[PLACEHOLDER_EXPR] && !valid[TEXT] && !valid[ATTR_NAME]) {
    TSSymbol result = 0;
    if (!scan_placeholder_expr(s, lexer, &result)) return false;
    lexer->result_symbol = result;
    return true;
  }
  if (valid[PLACEHOLDER_END] && !valid[TEXT] && !valid[ATTR_NAME]) {
    TSSymbol result = 0;
    if (!scan_placeholder_end(s, lexer, &result)) return false;
    lexer->result_symbol = result;
    return true;
  }
  if ((valid[SCRIPTLET_BLOCK_OPEN] || valid[SCRIPTLET_EXPR] ||
       valid[SCRIPTLET_BLOCK_EXPR] || valid[SCRIPTLET_BLOCK_CLOSE]) &&
      !valid[TEXT] && !valid[ATTR_NAME]) {
    TSSymbol result = 0;
    if (!scan_scriptlet_body(s, lexer, valid, &result)) return false;
    lexer->result_symbol = result;
    return true;
  }

  if (s->tag_open) return scan_open_tag(s, lexer, valid);

  return scan_content(s, lexer, valid);
}

// --------------------------------------------------------------------------
// Serialization
// --------------------------------------------------------------------------

#define WRITE(field)                          \
  do {                                        \
    memcpy(buffer + n, &(field), sizeof(field)); \
    n += sizeof(field);                       \
  } while (0)
#define READ(field)                           \
  do {                                        \
    memcpy(&(field), buffer + n, sizeof(field)); \
    n += sizeof(field);                       \
  } while (0)

void *tree_sitter_marko_external_scanner_create(void) {
  Scanner *s = (Scanner *)calloc(1, sizeof(Scanner));
  s->cur.line_only_ws = 1;
  s->cur.cur_indent_hash = HASH_INIT;
  s->saved = s->cur;
  return s;
}

void tree_sitter_marko_external_scanner_destroy(void *payload) {
  Scanner *s = (Scanner *)payload;
  free(s->buf);
  free(s);
}

unsigned tree_sitter_marko_external_scanner_serialize(void *payload,
                                                      char *buffer) {
  Scanner *s = (Scanner *)payload;
  unsigned n = 0;
  WRITE(s->saved);
  WRITE(s->started);
  WRITE(s->begin_mixed_mode);
  WRITE(s->ending_mixed_at_eol);
  WRITE(s->semicolon_line);
  WRITE(s->pending_element_ends);
  WRITE(s->suppress_placeholder);
  WRITE(s->fail_next);
  WRITE(s->tag_comma_continue);
  WRITE(s->tag_open);
  WRITE(s->tag_name_done);
  WRITE(s->in_shorthand);
  WRITE(s->adjacent_lt);
  WRITE(s->statement_pending);
  WRITE(s->in_attr_group);
  WRITE(s->attr_active);
  WRITE(s->attr_stage);
  WRITE(s->attr_has_args);
  WRITE(s->attr_has_type_params);
  WRITE(s->tag_has_attrs);
  WRITE(s->tag_has_args);
  WRITE(s->tag_has_params);
  WRITE(s->tag_has_shorthand_id);
  WRITE(s->tag_pending_types);
  WRITE(s->types_was_args);
  WRITE(s->expect_method_open);
  WRITE(s->block_after_tag);
  WRITE(s->tag_len);
  WRITE(s->content_len);
  for (uint8_t i = 0; i < s->tag_len; i++) WRITE(s->tags[i]);
  for (uint8_t i = 0; i < s->content_len; i++) WRITE(s->contents[i]);
  return n;
}

void tree_sitter_marko_external_scanner_deserialize(void *payload,
                                                    const char *buffer,
                                                    unsigned length) {
  Scanner *s = (Scanner *)payload;
  char *buf = s->buf;
  uint32_t buf_cap = s->buf_cap;
  memset(s, 0, sizeof(Scanner));
  s->buf = buf;
  s->buf_cap = buf_cap;
  s->cur.line_only_ws = 1;
  s->cur.cur_indent_hash = HASH_INIT;

  if (length == 0) {
    s->saved = s->cur;
    return;
  }

  unsigned n = 0;
  const char *bufferc = buffer;
  {
    const char *buffer = bufferc;
    READ(s->saved);
    READ(s->started);
    READ(s->begin_mixed_mode);
    READ(s->ending_mixed_at_eol);
    READ(s->semicolon_line);
    READ(s->pending_element_ends);
    READ(s->suppress_placeholder);
    READ(s->fail_next);
    READ(s->tag_comma_continue);
    READ(s->tag_open);
    READ(s->tag_name_done);
    READ(s->in_shorthand);
    READ(s->adjacent_lt);
    READ(s->statement_pending);
    READ(s->in_attr_group);
    READ(s->attr_active);
    READ(s->attr_stage);
    READ(s->attr_has_args);
    READ(s->attr_has_type_params);
    READ(s->tag_has_attrs);
    READ(s->tag_has_args);
    READ(s->tag_has_params);
    READ(s->tag_has_shorthand_id);
    READ(s->tag_pending_types);
    READ(s->types_was_args);
    READ(s->expect_method_open);
    READ(s->block_after_tag);
    READ(s->tag_len);
    READ(s->content_len);
    for (uint8_t i = 0; i < s->tag_len; i++) READ(s->tags[i]);
    for (uint8_t i = 0; i < s->content_len; i++) READ(s->contents[i]);
  }
  s->cur = s->saved;
}

#ifdef TSMARKO_DEBUG
static const char *const TOKEN_NAMES[] = {
    "TEXT", "PLACEHOLDER_START", "PLACEHOLDER_START_RAW", "INTERP_START",
    "PLACEHOLDER_EXPR", "PLACEHOLDER_END", "HTML_COMMENT", "LINE_COMMENT",
    "BLOCK_COMMENT", "CDATA", "DOCTYPE", "DECLARATION", "SCRIPTLET_START",
    "SCRIPTLET_START_CONCISE", "SCRIPTLET_BLOCK_OPEN", "SCRIPTLET_BLOCK_EXPR",
    "SCRIPTLET_BLOCK_CLOSE", "SCRIPTLET_EXPR", "BLOCK_OPEN", "BLOCK_CLOSE",
    "OPEN_TAG_START", "TAG_NAME_FRAGMENT", "TAG_NAME_EMPTY",
    "SHORTHAND_ID_START", "SHORTHAND_CLASS_START", "STATEMENT_EXPR",
    "TAG_VAR_START", "VAR_PATTERN", "VAR_COLON", "VAR_TYPE", "ARGS_OPEN",
    "ARGS_EXPR", "ARGS_CLOSE", "PARAMS_OPEN", "PARAM_PATTERN", "PARAM_COLON",
    "PARAM_TYPE", "PARAM_EQ", "PARAM_DEFAULT", "PARAM_COMMA",
    "PARAMS_CLOSE", "TYPE_ARGS_OPEN",
    "TYPE_PARAMS_OPEN", "TYPE_EXPR", "TYPES_CLOSE", "ATTR_GROUP_OPEN",
    "ATTR_GROUP_CLOSE", "ATTR_NAME", "ATTR_EQ", "ATTR_BOUND_EQ",
    "ATTR_SPREAD_START", "ATTR_VALUE_EXPR", "METHOD_BODY_OPEN",
    "METHOD_BODY_EXPR", "METHOD_BODY_CLOSE", "OPEN_TAG_END",
    "OPEN_TAG_END_SELF", "CONCISE_OPEN_TAG_END", "CLOSE_TAG_START",
    "CLOSE_TAG_NAME", "CLOSE_TAG_END", "ELEMENT_END", "TAG_COMMENT",
    "ESCAPE", "ERROR_SENTINEL",
};
#endif

bool tree_sitter_marko_external_scanner_scan(void *payload, TSLexer *lexer,
                                             const bool *valid_symbols) {
  Scanner *s = (Scanner *)payload;
  s->buf_len = 0;
#ifdef TSMARKO_DEBUG
  uint32_t col = lexer->get_column(lexer);
  bool ok = scan_main(s, lexer, valid_symbols);
  fprintf(stderr, "[scan col=%u eof=%d err=%d] -> %s (buf=%.*s)\n", col,
          at_eof(lexer), valid_symbols[ERROR_SENTINEL],
          ok ? TOKEN_NAMES[lexer->result_symbol] : "(none)",
          (int)(s->buf_len > 40 ? 40 : s->buf_len), s->buf ? s->buf : "");
  if (!ok) {
    fprintf(stderr, "  valid:");
    for (int i = 0; i <= ERROR_SENTINEL; i++) {
      if (valid_symbols[i]) fprintf(stderr, " %s", TOKEN_NAMES[i]);
    }
    fprintf(stderr, "\n");
  }
  return ok;
#else
  return scan_main(s, lexer, valid_symbols);
#endif
}
