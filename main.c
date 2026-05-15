#include "chibicc.h"

StringArray include_paths;

static StringArray opt_include;
static StringArray opt_define;
static char *opt_o;

char *base_file;
Prog *current_prog;

static StringArray input_paths;

static void usage(int status) {
  fprintf(stderr, "chibicc [ -o <path> ] <file>\n");
  exit(status);
}

static bool take_arg(char *arg) {
  char *x[] = {"-o", "-I", "-idirafter", "-include"};
  for (int i = 0; i < sizeof(x) / sizeof(*x); i++) {
    if (!strcmp(arg, x[i])) {
      return true;
    }
  }
  return false;
}

static void add_default_include_paths(char *argv0) {
  // We expect that chibicc-specific include files are installed
  // to ./include relative to argv[0].
  strarray_push(&include_paths, format("%s/include", dirname(strdup(argv0))));
}

static void define(char *str) {
  char *eq = strchr(str, '=');
  if (eq) {
    define_macro(strndup(str, eq - str), eq + 1);
  } else {
    define_macro(str, "1");
  }
}

static void parse_args(int argc, char **argv) {
  // Make sure that all command line options that take an argument
  // have an argument.
  for (int i = 1; i < argc; i++) {
    if (take_arg(argv[i])) {
      if (!argv[++i]) {
        usage(1);
      }
    }
  }

  StringArray idirafter = {};

  for (int i = 1; i < argc; i++) {

    if (!strcmp(argv[i], "--help")) {
      usage(0);
    }

    if (!strcmp(argv[i], "-o")) {
      opt_o = argv[++i];
      continue;
    }

    if (!strncmp(argv[i], "-o", 2)) {
      opt_o = argv[i] + 2;
      continue;
    }

    if (!strncmp(argv[i], "-I", 2)) {
      strarray_push(&include_paths, argv[i] + 2);
      continue;
    }

    if (!strcmp(argv[i], "-D")) {
      strarray_push(&opt_define, "D");
      strarray_push(&opt_define, argv[++i]);
      continue;
    }

    if (!strncmp(argv[i], "-D", 2)) {
      strarray_push(&opt_define, "D");
      strarray_push(&opt_define, argv[i] + 2);
      continue;
    }

    if (!strcmp(argv[i], "-U")) {
      strarray_push(&opt_define, "U");
      strarray_push(&opt_define, argv[++i]);
      continue;
    }

    if (!strncmp(argv[i], "-U", 2)) {
      strarray_push(&opt_define, "U");
      strarray_push(&opt_define, argv[i] + 2);
      continue;
    }

    if (!strcmp(argv[i], "-include")) {
      strarray_push(&opt_include, argv[++i]);
      continue;
    }

    if (!strcmp(argv[i], "-idirafter")) {
      strarray_push(&idirafter, argv[i++]);
      continue;
    }

    // These options are ignored for now.
    if (!strncmp(argv[i], "-O", 2) || !strncmp(argv[i], "-W", 2) || !strncmp(argv[i], "-g", 2) ||
        !strncmp(argv[i], "-std=", 5) || !strcmp(argv[i], "-ffreestanding") ||
        !strcmp(argv[i], "-fno-builtin") || !strcmp(argv[i], "-fno-omit-frame-pointer") ||
        !strcmp(argv[i], "-fno-stack-protector") || !strcmp(argv[i], "-fno-strict-aliasing") ||
        !strcmp(argv[i], "-m64") || !strcmp(argv[i], "-mno-red-zone") || !strcmp(argv[i], "-w")) {
      continue;
    }

    if (argv[i][0] == '-' && argv[i][1] != '\0') {
      error("unknown argument: %s", argv[i]);
    }

    strarray_push(&input_paths, argv[i]);
  }

  for (int i = 0; i < idirafter.len; i++) {
    strarray_push(&include_paths, idirafter.data[i]);
  }

  strarray_free(&idirafter);

  if (input_paths.len == 0) {
    error("no input files");
  }
}

// Returns true if a given file exists.
bool file_exists(char *path) {
  struct stat st;
  return !stat(path, &st);
}

static Token *must_tokenize_file(char *path) {
  Token *tok = tokenize_file(path);
  if (!tok) {
    error("%s: %s", path, strerror(errno));
  }
  return tok;
}

static Token *append_tokens(Token *tok1, Token *tok2) {
  if (!tok1 || tok1->kind == TK_EOF) {
    return tok2;
  }

  Token *t = tok1;
  while (t->next->kind != TK_EOF) {
    t = t->next;
  }
  t->next = tok2;
  return tok1;
}

static Obj *cc1(void) {
  Token *tok = NULL;

  // Process -D and -U options
  for (int i = 0; (i + 1) < opt_define.len; i += 2) {
    if (opt_define.data[i][0] == 'D') {
      define(opt_define.data[i + 1]);
    } else if (opt_define.data[i][0] == 'U') {
      undef_macro(opt_define.data[i + 1]);
    } else {
      unreachable();
    }
  }

  // Process -include option
  for (int i = 0; i < opt_include.len; i++) {
    char *incl = opt_include.data[i];

    char *path;
    if (file_exists(incl)) {
      path = incl;
    } else {
      path = search_include_paths(incl);
      if (!path) {
        error("-include: %s: %s", incl, strerror(errno));
      }
    }

    Token *tok2 = must_tokenize_file(path);
    tok = append_tokens(tok, tok2);
  }

  // Tokenize and parse.
  Token *tok2 = must_tokenize_file(base_file);
  tok = append_tokens(tok, tok2);
  tok = preprocess(tok);

  return parse(tok);
}

static FILE *open_file(const char *path) {
  if (!path || strcmp(path, "-") == 0) {
    return stdout;
  }

  FILE *out = fopen(path, "w");
  if (!out) {
    error("cannot open output file: %s: %s", path, strerror(errno));
  }
  return out;
}

int main(int argc, char **argv) {
  parse_args(argc, argv);

  Prog progs_head = {};
  Prog *progs_tail = &progs_head;
  for (int i = 0; i < input_paths.len; i++) {
    char *input = input_paths.data[i];

    reset_tokenize();
    reset_preprocess();
    reset_parse();

    base_file = input;
    init_macros();
    add_default_include_paths(argv[0]);

    Prog *prog = calloc(1, sizeof(Prog));
    prog->base_file = input;
    prog->index = i;

    current_prog = prog;

    Obj *obj = cc1();

    current_prog = NULL;

    prog->obj = obj;
    progs_tail->next = prog;
    progs_tail = prog;
  }

  // Codegen to temporary output buffer in case we need to
  // seek and the output file is stdout.
  ByteArray buf = {0};
  codegen(progs_head.next, &buf);

  // Write codegen output to output file.
  const char *out_path = opt_o ? opt_o : "a.out";
  FILE *out = open_file(out_path);
  fwrite(buf.data, buf.len, 1, out);
  fclose(out);
  chmod(out_path, 0755);

  bytearray_free(&buf);

  strarray_free(&include_paths);
  strarray_free(&opt_define);
  strarray_free(&opt_include);

  return 0;
}
