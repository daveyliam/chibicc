// This file implements the C preprocessor.
//
// The preprocessor takes a list of tokens as an input and returns a
// new list of tokens as an output.
//
// The preprocessing language is designed in such a way that that's
// guaranteed to stop even if there is a recursive macro.
// Informally speaking, a macro is applied only once for each token.
// That is, if a macro token T appears in a result of direct or
// indirect macro expansion of T, T won't be expanded any further.
// For example, if T is defined as U, and U is defined as T, then
// token T is expanded to U and then to T and the macro expansion
// stops at that point.
//
// To achieve the above behavior, we attach for each token a set of
// macro names from which the token is expanded. The set is called
// "hideset". Hideset is initially empty, and every time we expand a
// macro, the macro name is added to the resulting tokens' hidesets.
//
// The above macro expansion algorithm is explained in this document
// written by Dave Prossor, which is used as a basis for the
// standard's wording:
// https://github.com/rui314/chibicc/wiki/cpp.algo.pdf

#include "chibicc.h"
#include <stdlib.h>

typedef struct MacroParam MacroParam;
struct MacroParam {
  MacroParam *next;
  char *name;
};

typedef struct MacroArg MacroArg;
struct MacroArg {
  MacroArg *next;
  char *name;
  bool is_va_args;
  Token *tok;
};

typedef Token *macro_handler_fn(Token *);

typedef struct Macro Macro;
struct Macro {
  char *name;
  bool is_objlike; // Object-like or function-like
  MacroParam *params;
  char *va_args_name;
  Token *body;
  macro_handler_fn *handler;
};

// `#if` can be nested, so we use a stack to manage nested `#if`s.
typedef struct CondIncl CondIncl;
struct CondIncl {
  CondIncl *next;
  enum { IN_THEN, IN_ELIF, IN_ELSE } ctx;
  Token *tok;
  bool included;
};

typedef struct Hideset Hideset;
struct Hideset {
  Hideset *next;
  char *name;
};

static HashMap macros;
static CondIncl *cond_incl;
static HashMap pragma_once;
static int include_next_idx;
static HashMap include_cache;
static int counter_macro_id_next;
static HashMap include_guards;

static Token *preprocess2(Token *tok);
static Macro *find_macro(Token *tok);

static bool is_hash(Token *tok) { return tok->at_bol && equal(tok, "#"); }

static void free_hideset(Hideset *start) {
  for (Hideset *hs = start; hs;) {
    Hideset *hs_next = hs->next;
    free(hs);
    hs = hs_next;
  }
}

static void clear_token(Token *tok) {
  if (tok->str != NULL) {
    free(tok->str);
  }
  tok->str = NULL;
  if (tok->hideset != NULL) {
    free_hideset(tok->hideset);
  }
  tok->hideset = NULL;
}

void free_token(Token *tok) {
  clear_token(tok);
  free(tok);
}

void free_token_list(Token *start) {
  for (Token *tok = start; tok;) {
    Token *tok_next = tok->next;
    free_token(tok);
    tok = tok_next;
  }
}

static Token *new_eof(Token *tok) {
  Token *t = calloc(1, sizeof(Token));
  *t = *tok;
  t->str = NULL;
  t->hideset = NULL;
  t->next = NULL;
  t->kind = TK_EOF;
  t->len = 0;
  return t;
}

static Hideset *new_hideset(char *name) {
  Hideset *hs = calloc(1, sizeof(Hideset));
  hs->name = name;
  return hs;
}

static Hideset *hideset_union(Hideset *hs1, Hideset *hs2) {
  Hideset head = {};
  Hideset *cur = &head;
  for (; hs1; hs1 = hs1->next) {
    cur = cur->next = new_hideset(hs1->name);
  }
  for (; hs2; hs2 = hs2->next) {
    cur = cur->next = new_hideset(hs2->name);
  }
  return head.next;
}

static bool hideset_contains(Hideset *hs, char *s, int len) {
  for (; hs; hs = hs->next) {
    if (strlen(hs->name) == len && !strncmp(hs->name, s, len)) {
      return true;
    }
  }
  return false;
}

static Hideset *hideset_intersection(Hideset *hs1, Hideset *hs2) {
  Hideset head = {};
  Hideset *cur = &head;

  for (; hs1; hs1 = hs1->next) {
    if (hideset_contains(hs2, hs1->name, strlen(hs1->name))) {
      cur = cur->next = new_hideset(hs1->name);
    }
  }
  return head.next;
}

static Token *copy_token(Token *tok) {
  Token *t = calloc(1, sizeof(Token));
  *t = *tok;
  if (tok->str != NULL) {
    t->str = calloc(tok->ty->size, 1);
    memcpy(t->str, tok->str, tok->ty->size);
  }
  if (tok->hideset != NULL) {
    t->hideset = hideset_union(tok->hideset, NULL);
  }
  t->next = NULL;
  return t;
}

static Token *copy_tokens(Token *tok) {
  Token head = {};
  Token *cur = &head;
  for (; tok; tok = tok->next) {
    cur = cur->next = copy_token(tok);
  }
  return head.next;
}

static void add_hideset(Token *tok, Hideset *hs) {
  for (; tok; tok = tok->next) {
    Hideset *hs2 = hideset_union(tok->hideset, hs);
    if (tok->hideset) {
      free_hideset(tok->hideset);
    }
    tok->hideset = hs2;
  }
}

// Copies all tokens in tok1 and joins to the left of tok2.
static Token *append(Token *tok1, Token *tok2) {
  if (tok1->kind == TK_EOF) {
    return tok2;
  }

  Token head = {};
  Token *cur = &head;

  for (; tok1->kind != TK_EOF; tok1 = tok1->next) {
    cur = cur->next = copy_token(tok1);
  }
  cur->next = tok2;
  return head.next;
}

// Frees the current token, returns the next token.
static Token *advance(Token *tok) {
  Token *tok_next = tok->next;
  free_token(tok);
  return tok_next;
}

// Some preprocessor directives such as #include allow extraneous
// tokens before newline. This function skips such tokens.
static Token *skip_line_and_free(Token *tok) {
  if (tok->at_bol || tok->kind == TK_EOF) {
    return tok;
  }
  warn_tok(tok, "extra token");
  while (!tok->at_bol && tok->kind != TK_EOF) {
    tok = advance(tok);
  }
  return tok;
}

// Ensure that the current token is `op`, and if so free it and return the
// next token.
static Token *skip_and_free(Token *tok, char *op) {
  if (!equal(tok, op)) {
    error_tok(tok, "expected '%s'", op);
  }
  return advance(tok);
}

// Skip and free tokens until the token is `end` (pointer equality).
// Does not free the `end` token.
static Token *skip_and_free_until(Token *tok, Token *end) {
  while (tok != end && tok->kind != TK_EOF) {
    tok = advance(tok);
  }
  return tok;
}

// Consumes the current token if it matches `op`.
static bool consume_and_free(Token **rest, Token *tok, char *str) {
  if (equal(tok, str)) {
    *rest = advance(tok);
    return true;
  }
  *rest = tok;
  return false;
}

static Token *skip_cond_incl2(Token *tok, bool consume) {
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) &&
        (equal(tok->next, "if") || equal(tok->next, "ifdef") || equal(tok->next, "ifndef"))) {
      if (consume) {
        tok = advance(tok);
        tok = advance(tok);
        tok = skip_cond_incl2(tok, true);
      } else {
        tok = skip_cond_incl2(tok->next->next, false);
      }
      continue;
    }
    if (is_hash(tok) && equal(tok->next, "endif")) {
      if (consume) {
        tok = advance(tok);
        tok = advance(tok);
        return tok;
      } else {
        return tok->next->next;
      }
    }
    if (consume) {
      tok = advance(tok);
    } else {
      tok = tok->next;
    }
  }
  return tok;
}

// Skip until next `#else`, `#elif` or `#endif`.
// Nested `#if` and `#endif` are skipped.
static Token *_skip_cond_incl(Token *tok, bool consume) {
  while (tok->kind != TK_EOF) {
    if (is_hash(tok) &&
        (equal(tok->next, "if") || equal(tok->next, "ifdef") || equal(tok->next, "ifndef"))) {
      if (consume) {
        tok = advance(tok);
        tok = advance(tok);
        tok = skip_cond_incl2(tok, true);
      } else {
        tok = skip_cond_incl2(tok->next->next, false);
      }
      continue;
    }

    if (is_hash(tok) &&
        (equal(tok->next, "elif") || equal(tok->next, "else") || equal(tok->next, "endif"))) {
      break;
    }
    if (consume) {
      tok = advance(tok);
    } else {
      tok = tok->next;
    }
  }
  return tok;
}

static Token *skip_cond_incl(Token *tok) {
  return _skip_cond_incl(tok, true);
}

static Token *skip_cond_incl_nofree(Token *tok) {
  return _skip_cond_incl(tok, false);
}

// Double-quote a given string and returns it.
static char *quote_string(char *str) {
  int bufsize = 3;
  for (int i = 0; str[i]; i++) {
    if (str[i] == '\\' || str[i] == '"') {
      bufsize++;
    }
    bufsize++;
  }

  char *buf = calloc(bufsize, 1);
  char *p = buf;
  *p++ = '"';
  for (int i = 0; str[i]; i++) {
    if (str[i] == '\\' || str[i] == '"') {
      *p++ = '\\';
    }
    *p++ = str[i];
  }
  *p++ = '"';
  *p++ = '\0';
  return buf;
}

static Token *new_str_token(char *str, Token *tmpl) {
  char *buf = quote_string(str);
  Token *toks = tokenize(new_file(tmpl->file->name, tmpl->file->file_no, buf));
  free(buf);
  return toks;
}

// Read all tokens until the next newline, terminate them with
// an EOF token and then returns them. This function is used to
// create a new list of tokens for `#if` arguments.
static Token *read_line(Token **rest, Token *tok) {
  if (tok->at_bol || tok->kind == TK_EOF) {
    *rest = tok;
    return new_eof(tok);
  }

  Token *start = tok;

  // Advance tok until at points to the token to the last token in the line.
  for (; !tok->next->at_bol && tok->next->kind != TK_EOF; tok = tok->next) {
  }

  *rest = tok->next;
  tok->next = new_eof(tok);
  return start;
}

static Token *new_num_token(int val, Token *tmpl) {
  char *buf = format("%d\n", val);
  Token *num_toks = tokenize(new_file(tmpl->file->name, tmpl->file->file_no, buf));
  free(buf);
  return num_toks;
}

static Token *read_const_expr(Token **rest, Token *tok) {
  tok = read_line(rest, tok);

  Token head = {};
  Token *cur = &head;

  while (tok->kind != TK_EOF) {
    // "defined(foo)" or "defined foo" becomes "1" if macro "foo"
    // is defined. Otherwise "0".
    if (equal(tok, "defined")) {
      Token *start = copy_token(tok);
      tok = advance(tok);
      bool has_paren = consume_and_free(&tok, tok, "(");

      if (tok->kind != TK_IDENT) {
        error_tok(start, "macro name must be an identifier");
      }
      Macro *m = find_macro(tok);
      tok = advance(tok);

      if (has_paren) {
        tok = skip_and_free(tok, ")");
      }

      // new_num_token returns the token list from parsing '<number>\n'.
      // We only need the first token, the remainder can be freed.
      Token *num_tok = new_num_token(m ? 1 : 0, start);
      free_token_list(num_tok->next);
      num_tok->next = NULL;
      free_token(start);

      cur = cur->next = num_tok;
      continue;
    }

    // Move token onto output list.
    cur = cur->next = tok;
    tok = tok->next;
  }

  // Move EOF token onto output list.
  cur->next = tok;
  return head.next;
}

// Read and evaluate a constant expression.
static long eval_const_expr(Token **rest, Token *tok) {
  Token *start = copy_token(tok);
  Token *expr = read_const_expr(rest, tok);
  expr = preprocess2(expr);

  if (expr->kind == TK_EOF) {
    error_tok(start, "no expression");
  }
  free_token(start);

  // [https://www.sigbus.info/n1570#6.10.1p4] The standard requires
  // we replace remaining non-macro identifiers with "0" before
  // evaluating a constant expression. For example, `#if foo` is
  // equivalent to `#if 0` if foo is not defined.
  for (Token *t = expr; t->kind != TK_EOF; t = t->next) {
    if (t->kind == TK_IDENT) {
      Token *next = t->next;
      Token *num_tok = new_num_token(0, t);
      clear_token(t);
      *t = *num_tok;
      num_tok->str = NULL;
      num_tok->hideset = NULL;
      free_token_list(num_tok);
      t->next = next;
    }
  }

  // Convert pp-numbers to regular numbers
  convert_pp_tokens(expr);

  Token *rest2;
  long val = const_expr(&rest2, expr);
  if (rest2->kind != TK_EOF) {
    error_tok(rest2, "extra token");
  }
  // const_expr does not consume tokens, so need to free.
  free_token_list(expr);

  return val;
}

static CondIncl *push_cond_incl(Token *tok, bool included) {
  CondIncl *ci = calloc(1, sizeof(CondIncl));
  ci->next = cond_incl;
  ci->ctx = IN_THEN;
  ci->tok = copy_token(tok);
  ci->included = included;
  cond_incl = ci;
  return ci;
}

static void free_cond_incl(CondIncl *ci) {
  free_token(ci->tok);
  free(ci);
}

static Macro *find_macro(Token *tok) {
  if (tok->kind != TK_IDENT) {
    return NULL;
  }
  return hashmap_get2(&macros, tok->loc, tok->len);
}

static void free_macro(Macro *m) {
  free(m->name);
  m->name = NULL;
  if (m->va_args_name) {
    free(m->va_args_name);
  }
  m->va_args_name = NULL;
  for (MacroParam *param = m->params; param;) {
    MacroParam *param_next = param->next;
    free(param->name);
    free(param);
    param = param_next;
  }
  m->params = NULL;
  free_token_list(m->body);
  m->body = NULL;
  free(m);
}

static Macro *add_macro(char *name, bool is_objlike, Token *body) {
  Macro *m_prev = hashmap_get(&macros, name);
  if (m_prev != NULL) {
    free_macro(m_prev);
  }

  Macro *m = calloc(1, sizeof(Macro));
  m->name = strdup(name);
  m->is_objlike = is_objlike;
  m->body = body;
  hashmap_put(&macros, name, m);
  return m;
}

static MacroParam *read_macro_params(Token **rest, Token *tok, char **va_args_name) {
  MacroParam head = {};
  MacroParam *cur = &head;

  while (!equal(tok, ")")) {
    if (cur != &head) {
      tok = skip_and_free(tok, ",");
    }

    if (equal(tok, "...")) {
      *va_args_name = strdup("__VA_ARGS__");
      tok = advance(tok);
      *rest = skip_and_free(tok, ")");
      return head.next;
    }

    if (tok->kind != TK_IDENT) {
      error_tok(tok, "expected an identifier");
    }

    if (equal(tok->next, "...")) {
      *va_args_name = strndup(tok->loc, tok->len);
      tok = advance(tok);
      tok = advance(tok);
      *rest = skip_and_free(tok, ")");
      return head.next;
    }

    MacroParam *m = calloc(1, sizeof(MacroParam));
    m->name = strndup(tok->loc, tok->len);
    cur = cur->next = m;
    tok = advance(tok);
  }

  tok = advance(tok);
  *rest = tok;
  return head.next;
}

static void read_macro_definition(Token **rest, Token *tok) {
  if (tok->kind != TK_IDENT) {
    error_tok(tok, "macro name must be an identifier");
  }
  char *name = strndup(tok->loc, tok->len);
  tok = advance(tok);

  if (!tok->has_space && equal(tok, "(")) {
    // Function-like macro
    char *va_args_name = NULL;
    tok = advance(tok);
    MacroParam *params = read_macro_params(&tok, tok, &va_args_name);

    Macro *m = add_macro(name, false, read_line(rest, tok));
    m->params = params;
    if (va_args_name) {
      m->va_args_name = strdup(va_args_name);
      free(va_args_name);
    }
  } else {
    // Object-like macro
    add_macro(name, true, read_line(rest, tok));
  }
  free(name);
}

static MacroArg *read_macro_arg_one(Token **rest, Token *tok, bool read_rest) {
  Token head = {};
  Token *cur = &head;
  int level = 0;

  for (;;) {
    if (level == 0 && equal(tok, ")")) {
      break;
    }
    if (level == 0 && !read_rest && equal(tok, ",")) {
      break;
    }

    if (tok->kind == TK_EOF) {
      error_tok(tok, "premature end of input");
    }

    if (equal(tok, "(")) {
      level++;
    } else if (equal(tok, ")")) {
      level--;
    }

    cur = cur->next = tok;
    tok = tok->next;
  }

  cur->next = new_eof(tok);

  MacroArg *arg = calloc(1, sizeof(MacroArg));
  arg->tok = head.next;
  *rest = tok;
  return arg;
}

static MacroArg *read_macro_args(Token **rest, Token *tok, MacroParam *params, char *va_args_name) {
  Token *start = copy_token(tok);
  tok = advance(tok);
  tok = advance(tok);

  MacroArg head = {};
  MacroArg *cur = &head;

  MacroParam *pp = params;
  for (; pp; pp = pp->next) {
    if (cur != &head) {
      tok = skip_and_free(tok, ",");
    }
    cur = cur->next = read_macro_arg_one(&tok, tok, false);
    cur->name = pp->name;
  }

  if (va_args_name) {
    MacroArg *arg;
    if (equal(tok, ")")) {
      arg = calloc(1, sizeof(MacroArg));
      arg->tok = new_eof(tok);
    } else {
      if (pp != params) {
        tok = skip_and_free(tok, ",");
      }
      arg = read_macro_arg_one(&tok, tok, true);
    }
    arg->name = va_args_name;
    arg->is_va_args = true;
    cur = cur->next = arg;
  } else if (pp) {
    error_tok(start, "too many arguments");
  }

  free_token(start);
  // skip() here just checks that the next token is ')'.
  skip(tok, ")");
  // rest points to the ')' token.
  *rest = tok;
  return head.next;
}

static MacroArg *find_arg(MacroArg *args, Token *tok) {
  for (MacroArg *ap = args; ap; ap = ap->next) {
    if (tok->len == strlen(ap->name) && !strncmp(tok->loc, ap->name, tok->len)) {
      return ap;
    }
  }
  return NULL;
}

// Concatenates all tokens in `tok` and returns a new string.
// Does not free input tokens.
static char *join_tokens(Token *tok, Token *end) {
  // Compute the length of the resulting token.
  int len = 1;
  for (Token *t = tok; t != end && t->kind != TK_EOF; t = t->next) {
    if (t != tok && t->has_space) {
      len++;
    }
    len += t->len;
  }

  char *buf = calloc(len, 1);

  // Copy token texts.
  int pos = 0;
  for (Token *t = tok; t != end && t->kind != TK_EOF; t = t->next) {
    if (t != tok && t->has_space) {
      buf[pos++] = ' ';
    }
    strncpy(buf + pos, t->loc, t->len);
    pos += t->len;
  }
  buf[pos] = '\0';
  return buf;
}

// Concatenates all tokens in `arg` and returns a new string token.
// This function is used for the stringizing operator (#).
// Does not free input tokens.
static Token *stringize(Token *hash, Token *arg) {
  // Create a new string token. We need to set some value to its
  // source location for error reporting function, so we use a macro
  // name token as a template.
  char *s = join_tokens(arg, NULL);
  Token *s_tok = new_str_token(s, hash);
  free(s);
  return s_tok;
}

// Concatenate two tokens to create a new token.
// Does not free input tokens.
static Token *paste(Token *lhs, Token *rhs) {
  // Paste the two tokens.
  ByteArray arr = {};
  bytearray_extend(&arr, (uint8_t *)lhs->loc, lhs->len);
  bytearray_extend(&arr, (uint8_t *)rhs->loc, rhs->len);
  bytearray_append(&arr, 0);
  char *buf = (char *)arr.data;

  // Tokenize the resulting string.
  Token *tok = tokenize(new_file(lhs->file->name, lhs->file->file_no, buf));
  if (tok->next->kind != TK_EOF) {
    error_tok(lhs, "pasting forms '%s', an invalid token", buf);
  }
  free(buf);
  return tok;
}

static bool has_varargs(MacroArg *args) {
  for (MacroArg *ap = args; ap; ap = ap->next) {
    if (!strcmp(ap->name, "__VA_ARGS__")) {
      return ap->tok->kind != TK_EOF;
    }
  }
  return false;
}

static void free_macro_arg(MacroArg *arg) {
  if (arg->tok) {
    free_token_list(arg->tok);
  }
  free(arg);
}

static void free_macro_args_list(MacroArg *args) {
  for (MacroArg *arg = args; arg;) {
    MacroArg *arg_next = arg->next;
    free_macro_arg(arg);
    arg = arg_next;
  }
}

// Replace func-like macro parameters with given arguments.
// Consumes all input tokens.
static Token *subst(Token *tok, MacroArg *args) {
  Token head = {};
  Token *cur = &head;

  while (tok->kind != TK_EOF) {
    // "#" followed by a parameter is replaced with stringized actuals.
    if (equal(tok, "#")) {
      MacroArg *arg = find_arg(args, tok->next);
      if (!arg) {
        error_tok(tok->next, "'#' is not followed by a macro parameter");
      }
      // Stringize returns new string tok and new EOF tok.
      Token *str_tok = stringize(tok, arg->tok);
      free_token_list(str_tok->next);
      str_tok->next = NULL;

      cur = cur->next = str_tok;
      tok = advance(tok);
      tok = advance(tok);
      continue;
    }

    // [GNU] If __VA_ARG__ is empty, `,##__VA_ARGS__` is expanded
    // to the empty token list. Otherwise, its expanded to `,` and
    // __VA_ARGS__.
    if (equal(tok, ",") && equal(tok->next, "##")) {
      MacroArg *arg = find_arg(args, tok->next->next);
      if (arg && arg->is_va_args) {
        if (arg->tok->kind == TK_EOF) {
          tok = advance(tok);
          tok = advance(tok);
          tok = advance(tok);
        } else {
          cur = cur->next = copy_token(tok);
          tok = advance(tok);
          tok = advance(tok);
        }
        continue;
      }
    }

    if (equal(tok, "##")) {
      if (cur == &head) {
        error_tok(tok, "'##' cannot appear at start of macro expansion");
      }

      if (tok->next->kind == TK_EOF) {
        error_tok(tok, "'##' cannot appear at end of macro expansion");
      }

      MacroArg *arg = find_arg(args, tok->next);
      if (arg) {
        if (arg->tok->kind != TK_EOF) {
          Token *paste_arg_tok = paste(cur, arg->tok);
          clear_token(cur);
          *cur = *paste_arg_tok;
          paste_arg_tok->str = NULL;
          paste_arg_tok->hideset = NULL;
          // paste() only returns the joined token and EOF, so it is ok to free here after
          // the first token is copied.
          free_token_list(paste_arg_tok);
          for (Token *t = arg->tok->next; t->kind != TK_EOF; t = t->next) {
            cur = cur->next = copy_token(t);
          }
        }
        tok = advance(tok);
        tok = advance(tok);
        continue;
      }

      Token *paste_tok = paste(cur, tok->next);
      clear_token(cur);
      *cur = *paste_tok;
      paste_tok->str = NULL;
      paste_tok->hideset = NULL;
      free_token_list(paste_tok);
      tok = advance(tok);
      tok = advance(tok);
      continue;
    }

    MacroArg *arg = find_arg(args, tok);

    if (arg && equal(tok->next, "##")) {
      Token *rhs = tok->next->next;

      if (arg->tok->kind == TK_EOF) {
        MacroArg *arg2 = find_arg(args, rhs);
        if (arg2) {
          for (Token *t = arg2->tok; t->kind != TK_EOF; t = t->next) {
            cur = cur->next = copy_token(t);
          }
        } else {
          cur = cur->next = copy_token(rhs);
        }
        tok = advance(tok);
        tok = advance(tok);
        tok = advance(tok);
        continue;
      }

      for (Token *t = arg->tok; t->kind != TK_EOF; t = t->next) {
        cur = cur->next = copy_token(t);
      }
      tok = advance(tok);
      continue;
    }

    // If __VA_ARG__ is empty, __VA_OPT__(x) is expanded to the
    // empty token list. Otherwise, __VA_OPT__(x) is expanded to x.
    if (equal(tok, "__VA_OPT__") && equal(tok->next, "(")) {
      tok = advance(tok);
      tok = advance(tok);
      MacroArg *arg = read_macro_arg_one(&tok, tok, true);
      if (has_varargs(args)) {
        Token *t;
        for (t = arg->tok; t->kind != TK_EOF; t = t->next) {
          cur = cur->next = t;
        }
        arg->tok = NULL;
        free_token(t);
      }
      free_macro_arg(arg);
      tok = skip_and_free(tok, ")");
      continue;
    }

    // Handle a macro token. Macro arguments are completely macro-expanded
    // before they are substituted into a macro body.
    if (arg) {
      Token *t = preprocess2(copy_tokens(arg->tok));
      t->at_bol = tok->at_bol;
      t->has_space = tok->has_space;
      for (; t->kind != TK_EOF; t = t->next) {
        cur = cur->next = t;
      }
      free_token(t);
      tok = advance(tok);
      continue;
    }

    // Handle a non-macro token.
    cur = cur->next = tok;
    tok = tok->next;
    continue;
  }

  // Add EOF token.
  cur->next = tok;

  return head.next;
}

// If tok is a macro, expand it and return true.
// Otherwise, do nothing and return false.
static bool expand_macro(Token **rest, Token *tok) {
  if (hideset_contains(tok->hideset, tok->loc, tok->len)) {
    return false;
  }

  Macro *m = find_macro(tok);
  if (!m) {
    return false;
  }

  // Built-in dynamic macro application such as __LINE__
  if (m->handler) {
    Token *r = m->handler(tok);
    // Only append the first returned token.
    // Free everything else, including EOF.
    free_token_list(r->next);
    r->next = advance(tok);
    *rest = r;
    return true;
  }

  // Object-like macro application
  if (m->is_objlike) {
    Hideset *hs2 = new_hideset(m->name);
    Hideset *hs = hideset_union(tok->hideset, hs2);
    free_hideset(hs2);
    Token *body = copy_tokens(m->body);
    add_hideset(body, hs);
    free_hideset(hs);
    for (Token *t = body; t->kind != TK_EOF; t = t->next) {
      t->is_expanded = true;
      if (tok->origin_file != NULL) {
        t->origin_file = tok->origin_file;
        t->origin_line_no = tok->origin_line_no;
      } else {
        t->origin_file = tok->file;
        t->origin_line_no = tok->line_no;
      }
    }
    *rest = append(body, tok->next);
    (*rest)->at_bol = tok->at_bol;
    (*rest)->has_space = tok->has_space;
    free_token(tok);
    free_token_list(body);
    return true;
  }

  // If a funclike macro token is not followed by an argument list,
  // treat it as a normal identifier.
  if (!equal(tok->next, "(")) {
    return false;
  }

  // Function-like macro application
  Token *macro_token = copy_token(tok);
  MacroArg *args = read_macro_args(&tok, tok, m->params, m->va_args_name);
  Token *rparen = tok;

  // Tokens that consist a func-like macro invocation may have different
  // hidesets, and if that's the case, it's not clear what the hideset
  // for the new tokens should be. We take the interesection of the
  // macro token and the closing parenthesis and use it as a new hideset
  // as explained in the Dave Prossor's algorithm.
  Hideset *hs1 = hideset_intersection(macro_token->hideset, rparen->hideset);
  Hideset *hs2 = new_hideset(m->name);
  Hideset *hs = hideset_union(hs1, hs2);
  free_hideset(hs1);
  free_hideset(hs2);

  Token *body = copy_tokens(m->body);
  body = subst(body, args);
  free_macro_args_list(args);

  add_hideset(body, hs);
  free_hideset(hs);

  for (Token *t = body; t->kind != TK_EOF; t = t->next) {
    t->is_expanded = true;
    if (macro_token->origin_file != NULL) {
      t->origin_file = macro_token->origin_file;
      t->origin_line_no = macro_token->origin_line_no;
    } else {
      t->origin_file = macro_token->file;
      t->origin_line_no = macro_token->line_no;
    }
  }
  tok = advance(tok);
  *rest = append(body, tok);
  (*rest)->at_bol = macro_token->at_bol;
  (*rest)->has_space = macro_token->has_space;
  free_token(macro_token);
  free_token_list(body);
  return true;
}

char *search_include_paths(char *filename) {
  if (filename[0] == '/') {
    return filename;
  }

  char *cached = hashmap_get(&include_cache, filename);
  if (cached) {
    return cached;
  }

  // Search a file from the include paths.
  for (int i = 0; i < include_paths.len; i++) {
    char *path = format("%s/%s", include_paths.data[i], filename);
    if (!file_exists(path)) {
      free(path);
      continue;
    }
    hashmap_put(&include_cache, filename, path);
    include_next_idx = i + 1;
    return path;
  }
  return NULL;
}

static char *search_include_next(char *filename) {
  for (; include_next_idx < include_paths.len; include_next_idx++) {
    char *path = format("%s/%s", include_paths.data[include_next_idx], filename);
    if (file_exists(path)) {
      return path;
    }
    free(path);
  }
  return NULL;
}

// Read an #include argument.
static char *read_include_filename(Token **rest, Token *tok, bool *is_dquote) {
  // Pattern 1: #include "foo.h"
  if (tok->kind == TK_STR) {
    // A double-quoted filename for #include is a special kind of
    // token, and we don't want to interpret any escape sequences in it.
    // For example, "\f" in "C:\foo" is not a formfeed character but
    // just two non-control characters, backslash and f.
    // So we don't want to use token->str.
    *is_dquote = true;
    char *s = strndup(tok->loc + 1, tok->len - 2);
    tok = advance(tok);
    *rest = skip_line_and_free(tok);
    return s;
  }

  // Pattern 2: #include <foo.h>
  if (equal(tok, "<")) {
    // Reconstruct a filename from a sequence of tokens between
    // "<" and ">".
    Token *start = tok;

    // Find closing ">".
    for (; !equal(tok, ">"); tok = tok->next) {
      if (tok->at_bol || tok->kind == TK_EOF) {
        error_tok(tok, "expected '>'");
      }
    }

    *is_dquote = false;
    char *s = join_tokens(start->next, tok);
    tok = skip_and_free_until(start, tok);
    tok = advance(tok);
    *rest = skip_line_and_free(tok);
    return s;
  }

  // Pattern 3: #include FOO
  // In this case FOO must be macro-expanded to either
  // a single string token or a sequence of "<" ... ">".
  if (tok->kind == TK_IDENT) {
    Token *tok2 = preprocess2(read_line(rest, tok));
    char *filename = read_include_filename(&tok2, tok2, is_dquote);
    free_token_list(tok2);
    return filename;
  }

  error_tok(tok, "expected a filename");
}

// Detect the following "include guard" pattern.
//
//   #ifndef FOO_H
//   #define FOO_H
//   ...
//   #endif
static char *detect_include_guard(Token *tok) {
  // Detect the first two lines.
  if (!is_hash(tok) || !equal(tok->next, "ifndef")) {
    return NULL;
  }
  tok = tok->next->next;

  if (tok->kind != TK_IDENT) {
    return NULL;
  }

  char *macro = strndup(tok->loc, tok->len);
  tok = tok->next;

  if (!is_hash(tok) || !equal(tok->next, "define") || !equal(tok->next->next, macro)) {
    return NULL;
  }

  // Read until the end of the file.
  while (tok->kind != TK_EOF) {
    if (!is_hash(tok)) {
      tok = tok->next;
      continue;
    }

    if (equal(tok->next, "endif") && tok->next->next->kind == TK_EOF) {
      return macro;
    }

    if (equal(tok, "if") || equal(tok, "ifdef") || equal(tok, "ifndef")) {
      tok = skip_cond_incl_nofree(tok->next);
    } else {
      tok = tok->next;
    }
  }
  return NULL;
}

static Token *include_file(Token *tok, char *path, Token *filename_tok) {
  // Check for "#pragma once"
  if (hashmap_get(&pragma_once, path)) {
    return tok;
  }

  // If we read the same file before, and if the file was guarded
  // by the usual #ifndef ... #endif pattern, we may be able to
  // skip the file without opening it.
  char *guard_name = hashmap_get(&include_guards, path);
  if (guard_name && hashmap_get(&macros, guard_name)) {
    return tok;
  }

  Token *tok2 = tokenize_file(path);
  if (!tok2) {
    error_tok(filename_tok, "%s: cannot open file", path);
  }

  guard_name = detect_include_guard(tok2);
  if (guard_name) {
    hashmap_put(&include_guards, path, guard_name);
  }

  tok = append(tok2, tok);
  free_token_list(tok2);
  return tok;
}

// Read #line arguments
static void read_line_marker(Token **rest, Token *tok) {
  File *start_file = tok->file;
  int start_line_no = tok->line_no;
  tok = preprocess(read_line(rest, tok));
  Token *args = tok;

  if (tok->kind != TK_NUM || tok->ty->kind != TY_INT) {
    error_tok(tok, "invalid line marker");
  }
  start_file->line_delta = tok->val - start_line_no;

  tok = tok->next;
  if (tok->kind == TK_EOF) {
    free_token_list(args);
    return;
  }

  if (tok->kind != TK_STR) {
    error_tok(tok, "filename expected");
  }
  if (start_file->display_name != NULL) {
    free(start_file->display_name);
  }
  start_file->display_name = strdup(tok->str);
  free_token_list(args);
}

// Visit all tokens in `tok` while evaluating preprocessing
// macros and directives.
// The input token list `tok` should not be used after calling this
// as all processed tokens are freed.
// Returns a list of newly allocated tokens.
static Token *preprocess2(Token *tok) {
  Token head = {};
  Token *cur = &head;

  while (tok->kind != TK_EOF) {
    // If it is a macro, expand it.
    if (expand_macro(&tok, tok)) {
      continue;
    }

    // Pass through if it is not a "#".
    if (!is_hash(tok)) {
      tok->line_delta = tok->file->line_delta;
      tok->filename = tok->file->display_name;
      cur = cur->next = tok;
      tok = tok->next;
      continue;
    }

    Token *hash_tok = copy_token(tok);
    tok = advance(tok);

    if (equal(tok, "include")) {
      tok = advance(tok);
      Token *filename_tok = copy_token(tok);

      bool is_dquote;
      char *filename = read_include_filename(&tok, tok, &is_dquote);

      if (filename[0] != '/' && is_dquote) {
        char *d = dirname2(hash_tok->file->name);
        char *path = format("%s/%s", d, filename);
        free(d);
        if (file_exists(path)) {
          tok = include_file(tok, path, filename_tok);
          free(filename);
          free_token(filename_tok);
          free_token(hash_tok);
          free(path);
          continue;
        }
        free(path);
      }

      char *path = search_include_paths(filename);
      tok = include_file(tok, path ? path : filename, filename_tok);
      free(filename);
      free_token(filename_tok);
      free_token(hash_tok);
      continue;
    }

    if (equal(tok, "include_next")) {
      tok = advance(tok);
      Token *filename_tok = copy_token(tok);
      bool ignore;
      char *filename = read_include_filename(&tok, tok, &ignore);
      char *path = search_include_next(filename);
      tok = include_file(tok, path ? path : filename, filename_tok);
      if (path) {
        free(path);
      }
      free(filename);
      free_token(filename_tok);
      free_token(hash_tok);
      continue;
    }

    if (equal(tok, "define")) {
      tok = advance(tok);
      read_macro_definition(&tok, tok);
      free_token(hash_tok);
      continue;
    }

    if (equal(tok, "undef")) {
      tok = advance(tok);
      if (tok->kind != TK_IDENT) {
        error_tok(tok, "macro name must be an identifier");
      }
      char *name = strndup(tok->loc, tok->len);
      undef_macro(name);
      free(name);
      tok = advance(tok);
      tok = skip_line_and_free(tok);
      free_token(hash_tok);
      continue;
    }

    if (equal(tok, "if")) {
      tok = advance(tok);
      long val = eval_const_expr(&tok, tok);
      push_cond_incl(hash_tok, val);
      if (!val) {
        tok = skip_cond_incl(tok);
      }
      free_token(hash_tok);
      continue;
    }

    if (equal(tok, "ifdef")) {
      bool defined = find_macro(tok->next);
      push_cond_incl(tok, defined);
      tok = advance(tok);
      tok = advance(tok);
      tok = skip_line_and_free(tok);
      if (!defined) {
        tok = skip_cond_incl(tok);
      }
      free_token(hash_tok);
      continue;
    }

    if (equal(tok, "ifndef")) {
      bool defined = find_macro(tok->next);
      push_cond_incl(tok, !defined);
      tok = advance(tok);
      tok = advance(tok);
      tok = skip_line_and_free(tok);
      if (defined) {
        tok = skip_cond_incl(tok);
      }
      free_token(hash_tok);
      continue;
    }

    if (equal(tok, "elif")) {
      if (!cond_incl || cond_incl->ctx == IN_ELSE) {
        error_tok(hash_tok, "stray #elif");
      }
      cond_incl->ctx = IN_ELIF;

      tok = advance(tok);
      if (!cond_incl->included && eval_const_expr(&tok, tok)) {
        cond_incl->included = true;
      } else {
        tok = skip_cond_incl(tok);
      }
      free_token(hash_tok);
      continue;
    }

    if (equal(tok, "else")) {
      if (!cond_incl || cond_incl->ctx == IN_ELSE) {
        error_tok(hash_tok, "stray #else");
      }
      cond_incl->ctx = IN_ELSE;
      tok = advance(tok);
      tok = skip_line_and_free(tok);
      if (cond_incl->included) {
        tok = skip_cond_incl(tok);
      }
      free_token(hash_tok);
      continue;
    }

    if (equal(tok, "endif")) {
      if (!cond_incl) {
        error_tok(hash_tok, "stray #endif");
      }
      CondIncl *ci = cond_incl;
      cond_incl = ci->next;
      free_cond_incl(ci);
      tok = advance(tok);
      tok = skip_line_and_free(tok);
      free_token(hash_tok);
      continue;
    }

    if (equal(tok, "line")) {
      tok = advance(tok);
      read_line_marker(&tok, tok);
      free_token(hash_tok);
      continue;
    }

    if (tok->kind == TK_PP_NUM) {
      read_line_marker(&tok, tok);
      free_token(hash_tok);
      continue;
    }

    if (equal(tok, "pragma") && equal(tok->next, "once")) {
      hashmap_put(&pragma_once, tok->file->name, (void *)1);
      tok = advance(tok);
      tok = advance(tok);
      tok = skip_line_and_free(tok);
      free_token(hash_tok);
      continue;
    }

    if (equal(tok, "pragma")) {
      tok = advance(tok);
      while (!tok->at_bol) {
        tok = advance(tok);
      }
      free_token(hash_tok);
      continue;
    }

    if (equal(tok, "error")) {
      error_tok(tok, "error");
    }

    // `#`-only line is legal. It's called a null directive.
    if (tok->at_bol) {
      free_token(hash_tok);
      continue;
    }

    error_tok(tok, "invalid preprocessor directive");
  }

  cur->next = tok;
  return head.next;
}

void define_macro(char *name, char *buf) {
  Token *tok = tokenize(new_file("<built-in>", 1, buf));
  add_macro(name, true, tok);
}

void undef_macro(char *name) {
  Macro *m_prev = hashmap_get(&macros, name);
  if (m_prev != NULL) {
    free_macro(m_prev);
  }

  hashmap_delete(&macros, name);
}

static Macro *add_builtin(char *name, macro_handler_fn *fn) {
  Macro *m = add_macro(name, true, NULL);
  m->handler = fn;
  return m;
}

static Token *file_macro(Token *tmpl) {
  File *f = tmpl->origin_file ? tmpl->origin_file : tmpl->file;
  return new_str_token(f->display_name, tmpl);
}

static Token *line_macro(Token *tmpl) {
  int line_no;
  if (tmpl->origin_file != NULL) {
    line_no = tmpl->origin_line_no + tmpl->origin_file->line_delta;
  } else {
    line_no = tmpl->line_no + tmpl->file->line_delta;
  }
  return new_num_token(line_no, tmpl);
}

// __COUNTER__ is expanded to serial values starting from 0.
static Token *counter_macro(Token *tmpl) { return new_num_token(counter_macro_id_next++, tmpl); }

// __TIMESTAMP__ is expanded to a string describing the last
// modification time of the current file. E.g.
// "Fri Jul 24 01:32:50 2020"
// We do not support it.
static Token *timestamp_macro(Token *tmpl) {
  return new_str_token("??? ??? ?? ??:??:?? ????", tmpl);
}

static Token *base_file_macro(Token *tmpl) { return new_str_token(base_file, tmpl); }

void init_macros(void) {
  // Define predefined macros
  define_macro("_LP64", "1");
  define_macro("__C99_MACRO_WITH_VA_ARGS", "1");
  define_macro("__ELF__", "1");
  define_macro("__LP64__", "1");
  define_macro("__SIZEOF_DOUBLE__", "8");
  define_macro("__SIZEOF_FLOAT__", "4");
  define_macro("__SIZEOF_INT__", "4");
  define_macro("__SIZEOF_LONG_DOUBLE__", "8");
  define_macro("__SIZEOF_LONG_LONG__", "8");
  define_macro("__SIZEOF_LONG__", "4");
  define_macro("__SIZEOF_POINTER__", "4");
  define_macro("__SIZEOF_PTRDIFF_T__", "4");
  define_macro("__SIZEOF_SHORT__", "2");
  define_macro("__SIZEOF_SIZE_T__", "4");
  define_macro("__SIZE_TYPE__", "unsigned long");
  define_macro("__STDC_HOSTED__", "1");
  define_macro("__STDC_NO_COMPLEX__", "1");
  define_macro("__STDC_UTF_16__", "1");
  define_macro("__STDC_UTF_32__", "1");
  define_macro("__STDC_VERSION__", "201112L");
  define_macro("__STDC__", "1");
  define_macro("__USER_LABEL_PREFIX__", "");
  define_macro("__alignof__", "_Alignof");
  define_macro("__amd64", "1");
  define_macro("__amd64__", "1");
  define_macro("__chibicc__", "1");
  define_macro("__const__", "const");
  define_macro("__gnu_linux__", "1");
  define_macro("__inline__", "inline");
  define_macro("__linux", "1");
  define_macro("__linux__", "1");
  define_macro("__signed__", "signed");
  define_macro("__typeof__", "typeof");
  define_macro("__unix", "1");
  define_macro("__unix__", "1");
  define_macro("__volatile__", "volatile");
  define_macro("__x86_64", "1");
  define_macro("__x86_64__", "1");
  define_macro("linux", "1");
  define_macro("unix", "1");

  add_builtin("__FILE__", file_macro);
  add_builtin("__LINE__", line_macro);
  add_builtin("__COUNTER__", counter_macro);
  add_builtin("__TIMESTAMP__", timestamp_macro);
  add_builtin("__BASE_FILE__", base_file_macro);

  define_macro("__DATE__", "\"??? ?? ????\"");
  define_macro("__TIME__", "\"??:??:??\"");
}

typedef enum {
  STR_NONE,
  STR_UTF8,
  STR_UTF16,
  STR_UTF32,
  STR_WIDE,
} StringKind;

static StringKind get_string_kind(Token *tok) {
  if (!strcmp(tok->loc, "u8")) {
    return STR_UTF8;
  }

  switch (tok->loc[0]) {
  case '"':
    return STR_NONE;
  case 'u':
    return STR_UTF16;
  case 'U':
    return STR_UTF32;
  case 'L':
    return STR_WIDE;
  }
  unreachable();
}

// Concatenate adjacent string literals into a single string literal
// as per the C spec.
static void join_adjacent_string_literals(Token *tok) {
  // First pass: If regular string literals are adjacent to wide
  // string literals, regular string literals are converted to a wide
  // type before concatenation. In this pass, we do the conversion.
  for (Token *tok1 = tok; tok1->kind != TK_EOF;) {
    if (tok1->kind != TK_STR || tok1->next->kind != TK_STR) {
      tok1 = tok1->next;
      continue;
    }

    StringKind kind = get_string_kind(tok1);
    Type *basety = tok1->ty->base;

    for (Token *t = tok1->next; t->kind == TK_STR; t = t->next) {
      StringKind k = get_string_kind(t);
      if (kind == STR_NONE) {
        kind = k;
        basety = t->ty->base;
      } else if (k != STR_NONE && kind != k) {
        error_tok(t, "unsupported non-standard concatenation of string literals");
      }
    }

    if (basety->size > 1) {
      for (Token *t = tok1; t->kind == TK_STR; t = t->next) {
        if (t->ty->base->size == 1) {
          Token *t2 = tokenize_string_literal(t, basety);
          clear_token(t);
          *t = *t2;
          t2->str = NULL;
          t2->hideset = NULL;
          free_token(t2);
        }
      }
    }

    while (tok1->kind == TK_STR) {
      tok1 = tok1->next;
    }
  }

  // Second pass: concatenate adjacent string literals.
  for (Token *tok1 = tok; tok1->kind != TK_EOF;) {
    if (tok1->kind != TK_STR || tok1->next->kind != TK_STR) {
      tok1 = tok1->next;
      continue;
    }

    Token *tok2 = tok1->next;
    while (tok2->kind == TK_STR) {
      tok2 = tok2->next;
    }

    int len = tok1->ty->array_len;
    for (Token *t = tok1->next; t != tok2; t = t->next) {
      len = len + t->ty->array_len - 1;
    }

    char *buf = calloc(len, tok1->ty->base->size);

    int i = 0;
    for (Token *t = tok1; t != tok2; t = t->next) {
      memcpy(buf + i, t->str, t->ty->size);
      i = i + t->ty->size - t->ty->base->size;
    }

    tok1->ty = array_of(tok1->ty->base, len);
    if (tok1->str) {
      free(tok1->str);
    }
    tok1->str = buf;
    skip_and_free_until(tok1->next, tok2);
    tok1->next = tok2;
    tok1 = tok2;
  }
}

// Entry point function of the preprocessor.
Token *preprocess(Token *tok) {
  tok = preprocess2(tok);
  if (cond_incl) {
    error_tok(cond_incl->tok, "unterminated conditional directive");
  }
  convert_pp_tokens(tok);
  join_adjacent_string_literals(tok);

  for (Token *t = tok; t; t = t->next) {
    t->line_no += t->line_delta;
  }
  return tok;
}

void preprocess_init(void) {}

void preprocess_destroy(void) {}

void preprocess_end_unit(void) {
  int iter = 0;
  HashEntry *entry = NULL;
  while (hashmap_next(&macros, &iter, &entry)) {
    Macro *m = entry->val;
    free_macro(m);
  }
  hashmap_clear(&macros);

  for (CondIncl *ci = cond_incl; ci;) {
    CondIncl *ci_next = ci->next;
    free_cond_incl(ci);
    ci = ci_next;
  }
  cond_incl = NULL;

  hashmap_clear(&pragma_once);

  iter = 0;
  entry = NULL;
  while (hashmap_next(&include_cache, &iter, &entry)) {
    free(entry->val);
  }
  hashmap_clear(&include_cache);

  iter = 0;
  entry = NULL;
  while (hashmap_next(&include_guards, &iter, &entry)) {
    free(entry->val);
  }
  hashmap_clear(&include_guards);

  include_next_idx = 0;
  counter_macro_id_next = 0;
}

void preprocess_begin_unit(void) { preprocess_end_unit(); }
