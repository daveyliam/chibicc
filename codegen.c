#include "chibicc.h"

#define DEBUG_STACKIFIER 0

typedef struct Insn {
  struct Insn *next;
  char *op;
} Insn;

#define MAX_BB_OUTS 2

typedef struct BasicBlock {
  struct BasicBlock *next;
  int idx;
  int sorted_idx;
  Insn insns_head;
  Insn *insns_tail;
  bool is_terminated;
  int outs_count;
  struct BasicBlock *outs[MAX_BB_OUTS];
  int forward_outs_count;
  struct BasicBlock *forward_outs[MAX_BB_OUTS];
  int backward_outs_count;
  struct BasicBlock *backward_outs[MAX_BB_OUTS];
  int ins_count;
  struct BasicBlock **ins;
  uint8_t *dominators_set;
  uint8_t *reachable_set;
  bool is_loop_header;
} BasicBlock;

typedef enum ScopeKind { SCOPE_BLOCK, SCOPE_LOOP } ScopeKind;

typedef struct StackifierScope {
  struct StackifierScope *next;
  ScopeKind kind;
  int start;
  int end;
} StackifierScope;

typedef struct IntVector {
  int *data;
  int length;
  int capacity;
} IntVector;

typedef struct WasmLocal {
  struct WasmLocal *next;
  Type *ty;
  int idx;
} WasmLocal;

static FILE *current_out_file = NULL;
static Obj *current_fn = NULL;

static int local_counter = 0;
WasmLocal current_locals_head = {};
WasmLocal *current_locals_tail = NULL;

static int bb_counter = 0;
static BasicBlock current_bbs_head = {};
static BasicBlock *current_bbs_tail = NULL;
static BasicBlock *current_fn_return_bb = NULL;
static BasicBlock *current_bb = NULL;

static BasicBlock *current_break_bb = NULL;
static BasicBlock *current_continue_bb = NULL;

static void gen_expr(Node *node);
static void _gen_stmt(Node *node, bool should_drop_result);
static void gen_stmt(Node *node);

static void put_wasm(char *text) { fputs(text, current_out_file); }

__attribute__((format(printf, 1, 2))) static void fmt_wasm(char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(current_out_file, fmt, ap);
  va_end(ap);
}

static BasicBlock *append_new_bb(void) {
  BasicBlock *bb = calloc(1, sizeof(BasicBlock));

  bb->idx = bb_counter;
  bb_counter += 1;
  bb->sorted_idx = -1;
  bb->insns_head.next = NULL;
  bb->insns_tail = &bb->insns_head;

  current_bbs_tail->next = bb;
  current_bbs_tail = bb;
  return bb;
}

static void set_current_bb(BasicBlock *bb) { current_bb = bb; }

static void put_insn(char *op) {
  if (current_bb->is_terminated) {
    fprintf(stderr,
            "warning: function %s: adding insn '%s' to terminated bb bb_%d\n",
            current_fn->name, op, current_bb->idx);
  }

  Insn *insn = calloc(1, sizeof(Insn));
  insn->op = op;
  current_bb->insns_tail->next = insn;
  current_bb->insns_tail = insn;
}

__attribute__((format(printf, 1, 2))) static void fmt_insn(char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n <= 0) {
    return;
  }
  char *buf = calloc(n + 1, sizeof(char));
  va_start(ap, fmt);
  vsnprintf(buf, n + 1, fmt, ap);
  va_end(ap);
  put_insn(buf);
}

static void put_insn_return() {
  put_insn("return");
  current_bb->is_terminated = true;
}

static void put_insn_unreachable() {
  put_insn("unreachable");
  current_bb->is_terminated = true;
  current_bb->outs_count = 0;
}

static void put_insn_br(BasicBlock *bb) {
  fmt_insn("br $bb_%d", bb->idx);
  current_bb->is_terminated = true;
  current_bb->outs_count = 1;
  current_bb->outs[0] = bb;
}

static void put_insn_br_if(BasicBlock *then_bb, BasicBlock *else_bb) {
  fmt_insn("br_if $bb_%d $bb_%d", then_bb->idx, else_bb->idx);
  current_bb->is_terminated = true;
  current_bb->outs_count = 2;
  current_bb->outs[0] = then_bb;
  current_bb->outs[1] = else_bb;
}

static bool is_current_bb_terminated(void) { return current_bb->is_terminated; }

static void split_bb(void) {
  BasicBlock *next_bb = append_new_bb();
  if (!is_current_bb_terminated()) {
    put_insn_br(next_bb);
  }
  set_current_bb(next_bb);
}

static int alloc_local(Type *ty) {
  int idx = local_counter;
  local_counter += 1;
  WasmLocal *local = calloc(1, sizeof(WasmLocal));
  local->ty = ty;
  local->idx = idx;
  current_locals_tail->next = local;
  current_locals_tail = local;
  return idx;
}

// Round up `n` to the nearest multiple of `align`. For instance,
// align_to(5, 8) returns 8 and align_to(11, 8) returns 16.
int align_to(int n, int align) { return (n + align - 1) / align * align; }

static const char *get_wasm_type(Type *ty) {
  switch (ty->kind) {
  case TY_VOID:
    return "void";
  case TY_BOOL:
  case TY_CHAR:
  case TY_SHORT:
  case TY_INT:
  case TY_LONG:
    return "i32";
  case TY_LONGLONG:
    return "i64";
  case TY_FLOAT:
    return "f32";
  case TY_DOUBLE:
    return "f64";
  case TY_ENUM:
    return "i32";
  case TY_PTR:
    // WASM pointers are i32.
    return "i32";
  case TY_FUNC:
    // WASM function pointers are indexes into a function table.
    return "i32";
  case TY_ARRAY:
  case TY_VLA:
  case TY_STRUCT:
  case TY_UNION:
    // Aggregate types are always passed by reference.
    return "i32";
  }
  error("get_wasm_type: type kind %d unimplemented", ty->kind);
}

static void cmp_zero(Type *ty) {
  switch (ty->kind) {
  case TY_BOOL:
  case TY_CHAR:
  case TY_SHORT:
  case TY_INT:
  case TY_ENUM:
  case TY_LONG:
    put_insn("i32.eqz");
    return;
  case TY_LONGLONG:
    put_insn("i64.eqz");
    return;
  case TY_FLOAT:
    put_insn("f32.const 0");
    put_insn("f32.eq");
    return;
  case TY_DOUBLE:
    put_insn("f64.const 0");
    put_insn("f64.eq");
    return;
  case TY_PTR:
  case TY_FUNC:
  case TY_ARRAY:
  case TY_VLA:
    // or ref.is_null ?
    put_insn("i32.eqz");
    return;
  default:
    break;
  }
  error("gen_is_non_zero: bad type %d", ty->kind);
}

static void gen_lvar_addr(Obj *var) {
  put_insn("local.get $fp");
  if (var->offset) {
    fmt_insn("i32.const %d", var->offset);
    put_insn("i32.add");
  }
}

// Compute the absolute address of a given node.
// It's an error if a given node does not reside in memory.
static void gen_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR:
    // For VLA locals the address of the alloca for the array is stored in the
    // local var alloca. So we need to load the local to get the address of the
    // array.
    if (node->var->ty->kind == TY_VLA) {
      gen_lvar_addr(node->var);
      put_insn("i32.load");
      return;
    }
    // For local variables return the stack address of the var.
    if (node->var->is_local) {
      gen_lvar_addr(node->var);
      return;
    }
    // For functions return the function table index.
    if (node->ty->kind == TY_FUNC) {
      fmt_insn("i32.const %d", node->var->offset);
      return;
    }
    // For global variables return the memory address.
    fmt_insn("i32.const %d", node->var->offset);
    return;
  case ND_DEREF:
    gen_expr(node->lhs);
    return;
  case ND_COMMA:
    gen_expr(node->lhs);
    if (node->lhs->ty->kind != TY_VOID) {
      put_insn("drop");
    }
    gen_addr(node->rhs);
    return;
  case ND_MEMBER:
    gen_addr(node->lhs);
    fmt_insn("i32.const %d", node->member->offset);
    put_insn("i32.add");
    return;
  case ND_FUNCALL:
    // If the function returns a struct, then it should be copied to the local
    // var 'ret_buffer'.
    if (node->ret_buffer) {
      gen_expr(node);
      return;
    }
    break;
  case ND_ASSIGN:
  case ND_COND:
    if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION) {
      gen_expr(node);
      return;
    }
    break;
  case ND_VLA_PTR:
    // This node type wraps an ND_VAR node when a VLA is assigned to a local
    // var. Seems to be used to prevent the var from being loaded as would
    // normally be done to get the address of the contained VLA.
    gen_lvar_addr(node->var);
    return;
  default:
    break;
  }

  error_tok(node->tok, "not an lvalue");
}

// Load a value from where the top stack value is pointing to.
static void load(Type *ty) {
  switch (ty->kind) {
  case TY_ARRAY:
  case TY_STRUCT:
  case TY_UNION:
  case TY_FUNC:
  case TY_VLA:
    // If it is an array, do not attempt to load a value to the
    // register because in general we can't load an entire array to a
    // register. As a result, the result of an evaluation of an array
    // becomes not the array itself but the address of the array.
    // This is where "array is automatically converted to a pointer to
    // the first element of the array in C" occurs.
    return;
  case TY_FLOAT:
    put_insn("f32.load");
    return;
  case TY_DOUBLE:
    put_insn("f64.load");
    return;
  default:
    break;
  }

  const char *sx = ty->is_unsigned ? "u" : "s";
  if (ty->size == 1) {
    fmt_insn("i32.load8_%s", sx);
  } else if (ty->size == 2) {
    fmt_insn("i32.load16_%s", sx);
  } else if (ty->size == 4) {
    put_insn("i32.load");
  } else if (ty->size == 8) {
    put_insn("i64.load");
  } else {
    error("cannot load type");
  }
}

// Store top of stack to the address the second stack entry is pointing to.
static void store(Type *ty) {
  switch (ty->kind) {
  case TY_STRUCT:
  case TY_UNION:
    // [dst src n] -> []
    fmt_insn("i32.const %d", ty->size);
    put_insn("memory.copy");
    return;
  case TY_FLOAT:
    put_insn("f32.store");
    return;
  case TY_DOUBLE:
    put_insn("f64.store");
    return;
  default:
    break;
  }

  if (ty->size == 1) {
    put_insn("i32.store8");
  } else if (ty->size == 2) {
    put_insn("i32.store16");
  } else if (ty->size == 4) {
    put_insn("i32.store");
  } else if (ty->size == 8) {
    put_insn("i64.store");
  } else {
    error("cannot store type");
  }
}
enum { I8, I16, I32, I64, U8, U16, U32, U64, F32, F64 };

static int get_type_id(Type *ty) {
  switch (ty->kind) {
  case TY_CHAR:
    return ty->is_unsigned ? U8 : I8;
  case TY_SHORT:
    return ty->is_unsigned ? U16 : I16;
  case TY_INT:
  case TY_LONG:
    return ty->is_unsigned ? U32 : I32;
  case TY_LONGLONG:
    return ty->is_unsigned ? U64 : I64;
  case TY_FLOAT:
    return F32;
  case TY_DOUBLE:
    return F64;
  default:
    break;
  }

  // Pointer types.
  return U32;
}

// The table for type casts
static char i32i8[] = "i32.extend8_s";
static char i32u8[] = "(i32.and (i32.const 0xff))";
static char i32i16[] = "i32.extend16_s";
static char i32u16[] = "(i32.and (i32.const 0xffff))";
static char i32f32[] = "f32.convert_i32_s";
static char i32i64[] = "i64.extend32_s";
static char i32f64[] = "f64.convert_i32_s";

static char u32f32[] = "f32.convert_i32_u";
static char u32i64[] = "i64.extend_i32_u";
static char u32f64[] = "f64.convert_i32_u";

static char i64f32[] = "f32.convert_i64_s";
static char i64f64[] = "f64.convert_i64_s";

static char u64f32[] = "f32.convert_i64_u";
static char u64f64[] = "f64.convert_i64_u";

static char f32i8[] = "(i32.extend8_s (i32.trunc_f32_s))";
static char f32u8[] = "(i32.and (i32.trunc_f32_u) (i32.const 0xff))";
static char f32i16[] = "(i32.extend16_s (i32.trunc_f32_s))";
static char f32u16[] = "(i32.and (i32.trunc_f32_u) (i32.const 0xff))";
static char f32i32[] = "i32.trunc_f32_s";
static char f32u32[] = "i32.trunc_f32_u";
static char f32i64[] = "i64.trunc_f32_s";
static char f32u64[] = "i64.trunc_f32_u";
static char f32f64[] = "f64.promote_f32";

static char f64i8[] = "(i32.extend8_s (i32.trunc_f64_s))";
static char f64u8[] = "(i32.and (i32.trunc_f64_u) (i32.const 0xff))";
static char f64i16[] = "(i32.extend16_s (i32.trunc_f64_s))";
static char f64u16[] = "(i32.and (i32.trunc_f64_u) (i32.const 0xff))";
static char f64i32[] = "i32.trunc_f64_s";
static char f64u32[] = "i32.trunc_f64_u";
static char f64i64[] = "i64.trunc_f64_s";
static char f64u64[] = "i64.trunc_f64_u";
static char f64f32[] = "f32.demote_f64";

static char *cast_table[][10] = {
    // i8   i16     i32     i64     u8     u16     u32     u64     f32     f64
    {NULL, NULL, NULL, i32i64, i32u8, i32u16, NULL, i32i64, i32f32,
     i32f64}, // i8
    {i32i8, NULL, NULL, i32i64, i32u8, i32u16, NULL, i32i64, i32f32,
     i32f64}, // i16
    {i32i8, i32i16, NULL, i32i64, i32u8, i32u16, NULL, i32i64, i32f32,
     i32f64}, // i32
    {i32i8, i32i16, NULL, NULL, i32u8, i32u16, NULL, NULL, i64f32,
     i64f64}, // i64

    {i32i8, NULL, NULL, i32i64, NULL, NULL, NULL, i32i64, i32f32, i32f64}, // u8
    {i32i8, i32i16, NULL, i32i64, i32u8, NULL, NULL, i32i64, i32f32,
     i32f64}, // u16
    {i32i8, i32i16, NULL, u32i64, i32u8, i32u16, NULL, u32i64, u32f32,
     u32f64}, // u32
    {i32i8, i32i16, NULL, NULL, i32u8, i32u16, NULL, NULL, u64f32,
     u64f64}, // u64

    {f32i8, f32i16, f32i32, f32i64, f32u8, f32u16, f32u32, f32u64, NULL,
     f32f64}, // f32
    {
        f64i8,
        f64i16,
        f64i32,
        f64i64,
        f64u8,
        f64u16,
        f64u32,
        f64u64,
        f64f32,
        NULL,
    }, // f64
};

static void cast(Type *from, Type *to) {
  if (to->kind == TY_VOID) {
    put_insn("drop");
    return;
  }

  if (to->kind == TY_BOOL) {
    cmp_zero(from);
    put_insn("i32.eqz");
    return;
  }

  int t1 = get_type_id(from);
  int t2 = get_type_id(to);
  if (cast_table[t1][t2]) {
    put_insn(cast_table[t1][t2]);
  }
}

// Generate code for a given node.
static void gen_expr(Node *node) {
  switch (node->kind) {
  case ND_NULL_EXPR:
    return;
  case ND_NUM: {
    switch (node->ty->kind) {
    case TY_FLOAT:
      fmt_insn("f32.const %f", node->fval);
      return;
    case TY_DOUBLE:
      fmt_insn("f64.const %f", node->fval);
      return;
    case TY_LONGLONG:
      fmt_insn("i64.const %lld", node->val);
      return;
    default:
      break;
    }
    fmt_insn("i32.const %d", (int32_t)node->val);
    return;
  }
  case ND_NEG: {
    switch (node->ty->kind) {
    case TY_FLOAT:
      gen_expr(node->lhs);
      put_insn("f32.neg");
      return;
    case TY_DOUBLE:
      gen_expr(node->lhs);
      put_insn("f64.neg");
      return;
    case TY_LONGLONG:
      put_insn("i64.const 0");
      gen_expr(node->lhs);
      put_insn("i64.sub");
      return;
    default:
      put_insn("i32.const 0");
      gen_expr(node->lhs);
      put_insn("i32.sub");
    }
    return;
  }
  case ND_VAR:
    gen_addr(node);
    load(node->ty);
    return;
  case ND_MEMBER:
    gen_addr(node);
    load(node->ty);

    Member *mem = node->member;
    if (mem->is_bitfield) {
      fmt_insn("i32.const %d", 32 - mem->bit_width - mem->bit_offset);
      put_insn("i32.shl");
      fmt_insn("i32.const %d", 32 - mem->bit_width);
      if (mem->ty->is_unsigned) {
        put_insn("i32.shr_u");
      } else {
        put_insn("i32.shr_s");
      }
    }
    return;
  case ND_DEREF:
    gen_expr(node->lhs);
    load(node->ty);
    return;
  case ND_ADDR:
    gen_addr(node->lhs);
    return;
  case ND_ASSIGN: {
    gen_addr(node->lhs);
    gen_expr(node->rhs);

    // if (node->lhs->kind == ND_MEMBER && node->lhs->member->is_bitfield) {
    //   println("  mov %%rax, %%r8");

    //   // If the lhs is a bitfield, we need to read the current value
    //   // from memory and merge it with a new value.
    //   Member *mem = node->lhs->member;
    //   println("  mov %%rax, %%rdi");
    //   println("  and $%ld, %%rdi", (1L << mem->bit_width) - 1);
    //   println("  shl $%d, %%rdi", mem->bit_offset);

    //   println("  mov (%%rsp), %%rax");
    //   load(mem->ty);

    //   long mask = ((1L << mem->bit_width) - 1) << mem->bit_offset;
    //   println("  mov $%ld, %%r9", ~mask);
    //   println("  and %%r9, %%rax");
    //   println("  or %%rdi, %%rax");
    //   store(node->ty);
    //   println("  mov %%r8, %%rax");
    //   return;
    // }

    // XXX : Need to dup the RHS result so it is not consumed.
    // WASM doesn't have dup so need to use a local.
    int tmp = alloc_local(node->ty);
    fmt_insn("local.tee %d", tmp);
    store(node->ty);
    fmt_insn("local.get %d", tmp);
    return;
  }
  case ND_STMT_EXPR:
    for (Node *nd = node->body; nd; nd = nd->next) {
      bool should_drop_result = (nd->next != NULL);
      _gen_stmt(nd, should_drop_result);
    }
    return;
  case ND_COMMA:
    gen_expr(node->lhs);
    if (node->lhs->ty && (node->lhs->ty->kind != TY_VOID)) {
      put_insn("drop");
    }
    gen_expr(node->rhs);
    return;
  case ND_CAST:
    gen_expr(node->lhs);
    cast(node->lhs->ty, node->ty);
    return;
  case ND_MEMZERO:
    // Node is always a local var.
    gen_lvar_addr(node->var);
    if (node->var->ty->size == 4) {
      put_insn("i32.const 0");
      put_insn("i32.store");
    } else if (node->var->ty->size == 8) {
      put_insn("i64.const 0");
      put_insn("i64.store");
    } else {
      put_insn("i32.const 0");
      fmt_insn("i32.const %d", node->var->ty->size);
      put_insn("memory.fill");
    }
    return;
  case ND_COND: {
    // Ternary expression.
    BasicBlock *then_bb = append_new_bb();
    BasicBlock *else_bb = append_new_bb();
    BasicBlock *end_bb = append_new_bb();
    int result = -1;
    if (node->ty->kind != TY_VOID) {
      result = alloc_local(node->ty);
    }

    split_bb();
    gen_expr(node->cond);
    cmp_zero(node->cond->ty);
    put_insn_br_if(else_bb, then_bb);

    set_current_bb(then_bb);
    gen_expr(node->then);
    if (result >= 0) {
      fmt_insn("local.set %d", result);
    } else if (node->then->ty->kind != TY_VOID) {
      put_insn("drop");
    }
    put_insn_br(end_bb);

    set_current_bb(else_bb);
    gen_expr(node->els);
    if (result >= 0) {
      fmt_insn("local.set %d", result);
    } else if (node->els->ty->kind != TY_VOID) {
      put_insn("drop");
    }
    put_insn_br(end_bb);

    set_current_bb(end_bb);
    if (result >= 0) {
      fmt_insn("local.get %d", result);
    }
    return;
  }
  case ND_NOT:
    gen_expr(node->lhs);
    cmp_zero(node->lhs->ty);
    return;
  case ND_BITNOT:
    gen_expr(node->lhs);
    if (node->lhs->ty->kind == TY_LONGLONG) {
      put_insn("i64.const -1");
      put_insn("i64.xor");
    } else {
      put_insn("i32.const -1");
      put_insn("i32.xor");
    }
    return;
  case ND_LOGAND: {
    // Logical AND with short-circuit. Skip the RHS expression if the LHS is
    // false.
    BasicBlock *rhs_bb = append_new_bb();
    BasicBlock *end_bb = append_new_bb();
    int cond_local = alloc_local(node->ty);

    split_bb();
    gen_expr(node->lhs);
    cmp_zero(node->lhs->ty);
    fmt_insn("local.tee %d", cond_local);
    put_insn_br_if(end_bb, rhs_bb);

    set_current_bb(rhs_bb);
    gen_expr(node->rhs);
    cmp_zero(node->rhs->ty);
    put_insn("i32.eqz");
    fmt_insn("local.set %d", cond_local);
    put_insn_br(end_bb);

    set_current_bb(end_bb);
    if (node->ty->kind != TY_VOID) {
      fmt_insn("local.get %d", cond_local);
    }
    return;
  }
  case ND_LOGOR: {
    // Logical OR with short-circuit. Skip the RHS expression if the LHS is
    // true.
    BasicBlock *rhs_bb = append_new_bb();
    BasicBlock *end_bb = append_new_bb();
    int cond_local = alloc_local(node->ty);

    split_bb();
    gen_expr(node->lhs);
    cmp_zero(node->lhs->ty);
    put_insn("i32.eqz");
    fmt_insn("local.tee %d", cond_local);
    put_insn_br_if(end_bb, rhs_bb);

    set_current_bb(rhs_bb);
    gen_expr(node->rhs);
    cmp_zero(node->rhs->ty);
    put_insn("i32.eqz");
    fmt_insn("local.set %d", cond_local);
    put_insn_br(end_bb);

    set_current_bb(end_bb);
    if (node->ty->kind != TY_VOID) {
      fmt_insn("local.get %d", cond_local);
    }
    return;
  }
  case ND_FUNCALL: {
    if (node->lhs->kind == ND_VAR) {
      if (strcmp(node->lhs->var->name, "alloca") == 0) {
        error_tok(node->tok, "alloca builtin not supported");
        // gen_expr(node->args);
        // builtin_alloca();
        return;
      } else if (strcmp(node->lhs->var->name, "__builtin_memory_fill") == 0) {
        gen_expr(node->args);
        gen_expr(node->args->next);
        gen_expr(node->args->next->next);
        put_insn("memory.fill");
        return;
      } else if (strcmp(node->lhs->var->name, "__builtin_memory_copy") == 0) {
        gen_expr(node->args);
        gen_expr(node->args->next);
        gen_expr(node->args->next->next);
        put_insn("memory.copy");
        return;
      } else if (strcmp(node->lhs->var->name, "__builtin_memory_size") == 0) {
        put_insn("memory.size");
        return;
      } else if (strcmp(node->lhs->var->name, "__builtin_memory_grow") == 0) {
        gen_expr(node->args);
        put_insn("memory.grow");
        return;
      }
    }

    // Build args.
    Type *func_ty = node->func_ty;

    // If the return type is a struct/union, the caller passes
    // a pointer to a stack buffer as if it were the first argument.
    // Note that function Obj's have the ret buffer inserted into their
    // params list, but function Type's do not.
    bool returns_struct = node->ret_buffer;
    if (returns_struct) {
      gen_lvar_addr(node->ret_buffer);
    }

    // Push each non-variadic arg onto the stack in left to right order.
    Node *arg = node->args;
    for (Type *param_ty = func_ty->params; param_ty;
         param_ty = param_ty->next) {
      gen_expr(arg);
      arg = arg->next;
    }

    // Collect remaining variadic args into the 'va_arg_area' local for this
    // call-site.
    if (node->va_arg_area) {
      int offset = 0;
      for (; arg; arg = arg->next) {
        offset = align_to(offset, arg->ty->align);
        gen_lvar_addr(node->va_arg_area);
        fmt_insn("i32.const %d", offset);
        put_insn("i32.add");
        gen_expr(arg);
        store(arg->ty);
        offset += arg->ty->size;
      }
      gen_lvar_addr(node->va_arg_area);
    }

    // Try a direct call if the LHS is a function name.
    // Otherwise do an indirect call using the function table.
    if (node->lhs->kind == ND_VAR && node->lhs->ty->kind == TY_FUNC) {
      Obj *fn = node->lhs->var;
      if (fn->is_static) {
        fmt_insn("call $%s.%d", fn->name, fn->prog->index);
      } else {
        fmt_insn("call $%s", fn->name);
      }
    } else {
      // TODO : Indirect calls. Need to add types for all functions, and add
      // all functions to a table.
      if (!func_ty->wasm_idx) {
        error_tok(node->tok,
                  "bug, no wasm type emitted for indirect function call");
      }
      gen_expr(node->lhs);
      fmt_insn("(call_indirect (type %d))", func_ty->wasm_idx - 1);
    }

    if (returns_struct) {
      gen_lvar_addr(node->ret_buffer);
    }

    // If this is a non-returning function, then we should add an unreachable
    // instruction. How do we get the function obj (global) from the function
    // call node? Or can we add is_noreturn to the type at parse time? if
    // (func_var->is_noreturn) {
    //   put_insn_unreachable();
    // }
    return;
  }
  case ND_LABEL_VAL:
    // println("  lea %s(%%rip), %%rax", node->unique_label);
    error_tok(node->tok, "label-as-values not supported");
  case ND_CAS: {
    // gen_expr(node->cas_addr);
    // push();
    // gen_expr(node->cas_new);
    // push();
    // gen_expr(node->cas_old);
    // println("  mov %%rax, %%r8");
    // load(node->cas_old->ty->base);
    // pop("%rdx"); // new
    // pop("%rdi"); // addr

    // int sz = node->cas_addr->ty->base->size;
    // println("  lock cmpxchg %s, (%%rdi)", reg_dx(sz));
    // println("  sete %%cl");
    // println("  je 1f");
    // println("  mov %s, (%%r8)", reg_ax(sz));
    // println("1:");
    // println("  movzbl %%cl, %%eax");
    // return;
    error_tok(node->tok, "compare and swap builtin not supported");
  }
  case ND_EXCH: {
    // gen_expr(node->lhs);
    // push();
    // gen_expr(node->rhs);
    // pop("%rdi");

    // int sz = node->lhs->ty->base->size;
    // println("  xchg %s, (%%rdi)", reg_ax(sz));
    // return;
    error_tok(node->tok, "exchange builtin not supported");
  }
  default:
    break;
  }

  // All remaining node types should be simple binary expressions.
  if (node->lhs == NULL) {
    error_tok(node->tok, "unhandled expression: kind=%d", node->kind);
  }

  switch (node->lhs->ty->kind) {
  case TY_FLOAT: {
    // LHS is 32-bit float.

    gen_expr(node->lhs);
    gen_expr(node->rhs);

    switch (node->kind) {
    case ND_ADD:
      put_insn("f32.add");
      return;
    case ND_SUB:
      put_insn("f32.sub");
      return;
    case ND_MUL:
      put_insn("f32.mul");
      return;
    case ND_DIV:
      put_insn("f32.div");
      return;
    case ND_EQ:
      put_insn("f32.eq");
      return;
    case ND_NE:
      put_insn("f32.ne");
      return;
    case ND_LT:
      put_insn("f32.lt");
      return;
    case ND_LE:
      put_insn("f32.le");
      return;
    default:
      break;
    }
    error_tok(node->tok, "invalid floating point expression");
  }
  case TY_DOUBLE: {
    // LHS is 64-bit float.

    gen_expr(node->lhs);
    gen_expr(node->rhs);

    switch (node->kind) {
    case ND_ADD:
      put_insn("f64.add");
      return;
    case ND_SUB:
      put_insn("f64.sub");
      return;
    case ND_MUL:
      put_insn("f64.mul");
      return;
    case ND_DIV:
      put_insn("f64.div");
      return;
    case ND_EQ:
      put_insn("f64.eq");
      return;
    case ND_NE:
      put_insn("f64.ne");
      return;
    case ND_LT:
      put_insn("f64.lt");
      return;
    case ND_LE:
      put_insn("f64.le");
      return;
    default:
      break;
    }
    error_tok(node->tok, "invalid floating point expression");
  }
  case TY_PTR:
  case TY_FUNC:
  case TY_ARRAY:
  case TY_VLA: {
    // LHS is pointer.
    // Parser always makes the pointer the LHS.

    gen_expr(node->lhs);
    gen_expr(node->rhs);

    switch (node->kind) {
    case ND_ADD:
      switch (node->rhs->ty->kind) {
      case TY_LONG:
        // ptr + int.
        // RHS should have been cast to a pointer sized int by the
        // usual arithmetic conversions.
        put_insn("i32.add");
        return;
      default:
        break;
      }
      error_tok(node->tok, "invalid pointer addition");
    case ND_SUB: {
      switch (node->rhs->ty->kind) {
      case TY_LONG:
        // ptr - int.
        // RHS should have been cast to a pointer sized int by the
        // usual arithmetic conversions.
        put_insn("i32.sub");
        return;
      case TY_PTR:
      case TY_FUNC:
      case TY_ARRAY:
      case TY_VLA:
        // ptr - ptr.
        put_insn("i32.sub");
        return;
      default:
        break;
      }
      error_tok(node->tok, "invalid pointer subtraction");
    }
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE:
      // ptr comparison.
      switch (node->kind) {
      case ND_EQ:
        put_insn("i32.eq");
        return;
      case ND_NE:
        put_insn("i32.ne");
        return;
      case ND_LT:
        put_insn("i32.lt_u");
        return;
      case ND_LE:
        put_insn("i32.le_u");
        return;
      default:
        break;
      }
      error("unreachable pointer comparison");
    default:
      break;
    }
    error_tok(node->tok, "invalid pointer expression");
  }
  default:
    break;
  }

  // integer binary ops.

  gen_expr(node->lhs);
  gen_expr(node->rhs);

  switch (node->kind) {
  case ND_ADD:
    put_insn("i32.add");
    return;
  case ND_SUB:
    put_insn("i32.sub");
    return;
  case ND_MUL:
    put_insn("i32.mul");
    return;
  case ND_DIV:
    if (node->ty->is_unsigned) {
      put_insn("i32.div_u");
    } else {
      put_insn("i32.div_s");
    }
    return;
  case ND_MOD:
    if (node->ty->is_unsigned) {
      put_insn("i32.rem_u");
    } else {
      put_insn("i32.rem_s");
    }
    return;
  case ND_BITAND:
    put_insn("i32.and");
    return;
  case ND_BITOR:
    put_insn("i32.or");
    return;
  case ND_BITXOR:
    put_insn("i32.xor");
    return;
  case ND_EQ:
    put_insn("i32.eq");
    return;
  case ND_NE:
    put_insn("i32.ne");
    return;
  case ND_LT:
    if (node->lhs->ty->is_unsigned) {
      put_insn("i32.lt_u");
    } else {
      put_insn("i32.lt_s");
    }
    return;
  case ND_LE:
    if (node->lhs->ty->is_unsigned) {
      put_insn("i32.le_u");
    } else {
      put_insn("i32.le_s");
    }
    return;
  case ND_SHL:
    put_insn("i32.shl");
    return;
  case ND_SHR:
    if (node->lhs->ty->is_unsigned) {
      put_insn("i32.shr_u");
    } else {
      put_insn("i32.shr_s");
    }
    return;
  default:
    break;
  }
  error_tok(node->tok, "invalid expression");
}

static void _gen_stmt(Node *node, bool should_drop_result) {
  // The stack size should be 0 after every statement
  // EXCEPT if we are within a [GNU] statement expression.
  // In that case the value of the last expression statement in
  // the statement expression body should be left on the stack.

  switch (node->kind) {
  case ND_IF: {
    BasicBlock *then_bb = append_new_bb();
    BasicBlock *else_bb = append_new_bb();
    BasicBlock *end_bb = append_new_bb();

    split_bb();
    gen_expr(node->cond);
    cmp_zero(node->cond->ty);
    put_insn_br_if(else_bb, then_bb);

    set_current_bb(then_bb);
    if (node->then) {
      gen_stmt(node->then);
    }

    put_insn_br(end_bb);

    set_current_bb(else_bb);
    if (node->els) {
      gen_stmt(node->els);
    }
    put_insn_br(end_bb);

    set_current_bb(end_bb);
    return;
  }
  case ND_FOR: {
    BasicBlock *cond_bb = append_new_bb();
    BasicBlock *inc_bb = append_new_bb();
    BasicBlock *loop_bb = append_new_bb();
    BasicBlock *end_bb = append_new_bb();

    if (node->init) {
      split_bb();
      gen_stmt(node->init);
    }
    put_insn_br(cond_bb);

    set_current_bb(cond_bb);
    if (node->cond) {
      gen_expr(node->cond);
      cmp_zero(node->cond->ty);
      put_insn_br_if(end_bb, loop_bb);
    } else {
      put_insn_br(loop_bb);
    }

    set_current_bb(loop_bb);
    if (node->then) {
      BasicBlock *prev_break_bb = current_break_bb;
      BasicBlock *prev_continue_bb = current_continue_bb;
      current_break_bb = end_bb;
      current_continue_bb = inc_bb;
      gen_stmt(node->then);
      current_break_bb = prev_break_bb;
      current_continue_bb = prev_continue_bb;
    }
    put_insn_br(inc_bb);

    set_current_bb(inc_bb);
    if (node->inc) {
      gen_expr(node->inc);
    }
    put_insn_br(cond_bb);

    set_current_bb(end_bb);
    return;
  }
  case ND_DO: {
    BasicBlock *cond_bb = append_new_bb();
    BasicBlock *loop_bb = append_new_bb();
    BasicBlock *end_bb = append_new_bb();

    // no need to split here.
    put_insn_br(loop_bb);

    set_current_bb(loop_bb);
    if (node->then) {
      BasicBlock *prev_break_bb = current_break_bb;
      BasicBlock *prev_continue_bb = current_continue_bb;
      current_break_bb = end_bb;
      current_continue_bb = cond_bb;
      gen_stmt(node->then);
      current_break_bb = prev_break_bb;
      current_continue_bb = prev_continue_bb;
    }
    put_insn_br(cond_bb);

    set_current_bb(cond_bb);
    gen_expr(node->cond);
    cmp_zero(node->cond->ty);

    put_insn_br_if(end_bb, loop_bb);

    set_current_bb(end_bb);
    return;
  }
  case ND_SWITCH: {
    // Add basic blocks that we will need to refer to.
    // However if there are GNU case ranges then we need to check those, and if
    // they don't match go to the default block.

    split_bb();

    // Switch condition value.
    gen_expr(node->cond);
    int cond_local = alloc_local(node->cond->ty);
    fmt_insn("local.set %d", cond_local);

    // Generate branches to case labels.
    BasicBlock *next_bb = NULL;
    for (Node *nd = node->case_next; nd; nd = nd->case_next) {
      bool is_i64 = (node->cond->ty->size > 4);

      nd->goto_bb = append_new_bb();

      fmt_insn("local.get %d", cond_local);
      if (nd->begin == nd->end) {
        // Normal single integer case.
        if (is_i64) {
          fmt_insn("i64.const %ld", nd->begin);
          put_insn("i64.eq");
        } else {
          fmt_insn("i32.const %ld", nd->begin);
          put_insn("i32.eq");
        }
      } else {
        // [GNU] case ranges.
        if (is_i64) {
          fmt_insn("i64.const %ld", nd->begin);
          put_insn("i64.sub");
          fmt_insn("i64.const %ld", nd->end - nd->begin);
          put_insn("i64.le_s");
        } else {
          fmt_insn("i32.const %ld", nd->begin);
          put_insn("i32.sub");
          fmt_insn("i32.const %ld", nd->end - nd->begin);
          put_insn("i32.le_s");
        }
      }
      next_bb = append_new_bb();
      put_insn_br_if(nd->goto_bb, next_bb);
      set_current_bb(next_bb);
    }

    BasicBlock *end_bb = append_new_bb();

    if (node->default_case) {
      node->default_case->goto_bb = append_new_bb();
      put_insn_br(node->default_case->goto_bb);
    } else {
      put_insn_br(end_bb);
    }

    // We terminated the current BB above, so need a new BB for the first
    // switch body statement.
    next_bb = append_new_bb();
    set_current_bb(next_bb);

    BasicBlock *prev_break_bb = current_break_bb;
    current_break_bb = end_bb;

    // Generate code for switch body.
    // Includes case labels and all case bodies.
    gen_stmt(node->then);

    // If the last case does not end with a break, then it will not be
    // terminated.
    if (!is_current_bb_terminated()) {
      put_insn_br(end_bb);
    }

    current_break_bb = prev_break_bb;

    set_current_bb(end_bb);
    return;
  }
  case ND_CASE: {
    // TODO : Can we move the case BBs closer to the switch?
    put_insn_br(node->goto_bb);
    set_current_bb(node->goto_bb);
    gen_stmt(node->lhs);
    return;
  }
  case ND_BLOCK: {
    for (Node *nd = node->body; nd; nd = nd->next) {
      gen_stmt(nd);
    }
    return;
  }
  case ND_BREAK: {
    // Need to add a new BB just in case we write (unreachable) instructions
    // after the goto. WASM validator doesn't like unreachable instructions.
    put_insn_br(current_break_bb);
    BasicBlock *next_bb = append_new_bb();
    set_current_bb(next_bb);
    return;
  }
  case ND_CONTINUE: {
    // Same as above, add a new BB after the branch.
    put_insn_br(current_continue_bb);
    BasicBlock *next_bb = append_new_bb();
    set_current_bb(next_bb);
    return;
  }
  case ND_GOTO: {
    // The terminating branch instruction will be added to this basic block
    // after all labels have been processed.
    // Add a new basic block (will be unreachable unless it begins with a
    // label).
    node->goto_bb = current_bb;
    BasicBlock *next_bb = append_new_bb();
    set_current_bb(next_bb);
    return;
  }
  case ND_GOTO_EXPR:
    // gen_expr(node->lhs);
    // println("  jmp *%%rax");
    error_tok(node->tok, "goto expr not supported");
    return;
  case ND_LABEL: {
    // TODO : only append BB if current BB is not empty?
    BasicBlock *label_bb = append_new_bb();
    node->goto_bb = label_bb;
    put_insn_br(label_bb);
    set_current_bb(label_bb);
    gen_stmt(node->lhs);
    return;
  }
  case ND_RETURN: {
    // Need to add a basic block in case something tries to generate code after
    // the return. It will most likely be unreachable.
    // XXX: This is a bad solution :(
    BasicBlock *next_bb = append_new_bb();
    if (!node->lhs) {
      put_insn_br(current_fn_return_bb);
      set_current_bb(next_bb);
      return;
    }
    Type *ty = node->lhs->ty;
    bool returns_struct = (ty->kind == TY_STRUCT) || (ty->kind == TY_UNION);
    if (returns_struct) {
      // Structure returns are written to the buffer pointed to by the first
      // param. (the hidden first param is inserted by the parser).
      Obj *ret_buffer_var = current_fn->params;
      gen_lvar_addr(ret_buffer_var);
      gen_addr(node->lhs);
      fmt_insn("i32.const %d", ty->size);
      put_insn("memory.copy");
      put_insn_br(current_fn_return_bb);
      set_current_bb(next_bb);
      return;
    }
    gen_expr(node->lhs);
    put_insn("local.set $result");
    put_insn_br(current_fn_return_bb);
    set_current_bb(next_bb);
    return;
  }
  case ND_EXPR_STMT:
    gen_expr(node->lhs);
    if (should_drop_result) {
      if (node->lhs->ty && (node->lhs->ty->kind != TY_VOID)) {
        put_insn("drop");
      }
    }
    return;
  case ND_ASM:
    error_tok(node->tok, "inline asm not supported");
    return;
  default:
    break;
  }

  error_tok(node->tok, "invalid statement");
}

static void gen_stmt(Node *node) { _gen_stmt(node, true); }

static int get_bitset_size(int count) { return (count + 7) / 8; }

static uint8_t *bitset_new(int count) {
  return calloc(get_bitset_size(count), sizeof(uint8_t));
}

static void bitset_free(uint8_t *bitset) { free(bitset); }

static void bitset_fill(uint8_t *bitset, bool val, int count) {
  int size = get_bitset_size(count);
  if (size == 0) {
    return;
  }
  int n = count % 8;
  if (n == 0 || !val) {
    memset(bitset, (val) ? 0xff : 0, size);
    return;
  }
  if (size > 1) {
    memset(bitset, 0xff, size - 1);
  }
  bitset[size - 1] = 0xff >> (8 - n);
}

static void bitset_copy(uint8_t *bitset1, uint8_t *bitset2, int count) {
  memmove(bitset1, bitset2, get_bitset_size(count));
}

static bool bitset_get(uint8_t *bitset, unsigned int index) {
  return ((bitset[index / 8] & (1 << (index % 8))) != 0);
}

static void bitset_set(uint8_t *bitset, unsigned int index, bool val) {
  if (val) {
    bitset[index / 8] |= 1 << (index % 8);
  } else {
    bitset[index / 8] &= ~(1 << (index % 8));
  }
}

static void bitset_intersect(uint8_t *bitset1, uint8_t *bitset2, int count) {
  int size = get_bitset_size(count);
  for (int i = 0; i < size; i++) {
    bitset1[i] &= bitset2[i];
  }
}

static void bitset_union(uint8_t *bitset1, uint8_t *bitset2, int count) {
  int size = get_bitset_size(count);
  for (int i = 0; i < size; i++) {
    bitset1[i] |= bitset2[i];
  }
}

static bool is_bitset_equal(uint8_t *bitset1, uint8_t *bitset2, int count) {
  int size = get_bitset_size(count);
  for (int i = 0; i < size; i++) {
    if (bitset1[i] != bitset2[i]) {
      return false;
    };
  }
  return true;
}

static IntVector *intvector_new() {
  IntVector *vec = calloc(1, sizeof(IntVector));
  vec->data = NULL;
  vec->length = 0;
  vec->capacity = 0;
  return vec;
}

static void intvector_push(IntVector *vec, int val) {
  if (vec->length >= vec->capacity) {
    int capacity = vec->capacity;
    if (capacity < 8) {
      capacity = 8;
    }
    while (vec->length >= capacity) {
      capacity *= 2;
    }
    int *data = calloc(capacity, sizeof(int));
    if (vec->data && (vec->length > 0)) {
      memcpy(data, vec->data, vec->length * sizeof(int));
    }
    if (vec->data) {
      free(vec->data);
    }
    vec->data = data;
    vec->capacity = capacity;
  }
  vec->data[vec->length++] = val;
}

static int intvector_get(IntVector *vec, int index) {
  if (index >= vec->length) {
    return 0;
  }
  return vec->data[index];
}

static int intvector_pop(IntVector *vec) {
  if (vec->length <= 0) {
    return 0;
  }
  return vec->data[vec->length--];
}

static void intvector_free(IntVector *vec) {
  if (vec->data) {
    free(vec->data);
  }
  free(vec);
}

static void compute_cfg_ins(BasicBlock **bbs, int bb_count) {
  // Compute predecessors for each BB.
  uint8_t *tmp = bitset_new(bb_count);
  for (int i = 0; i < bb_count; i++) {
    bitset_fill(tmp, false, bb_count);
    BasicBlock *bb = bbs[i];
    bb->ins_count = 0;
    bb->ins = NULL;
    for (int j = 0; j < bb_count; j++) {
      BasicBlock *bb2 = bbs[j];
      for (int k = 0; k < bb2->outs_count; k++) {
        BasicBlock *bb3 = bb2->outs[k];
        if (bb3->idx == bb->idx) {
          if (!bitset_get(tmp, j)) {
            bb->ins_count += 1;
            bitset_set(tmp, j, true);
          }
        }
      }
    }
    if (bb->ins_count <= 0) {
      continue;
    }
    bb->ins = calloc(bb->ins_count, sizeof(BasicBlock *));
    int ins_pos = 0;
    for (int j = 0; j < bb_count; j++) {
      if (bitset_get(tmp, j)) {
        bb->ins[ins_pos] = bbs[j];
        ins_pos += 1;
      }
    }
  }
  bitset_free(tmp);
}

static void compute_cfg_dominators(BasicBlock **bbs, int bb_count) {
  // Compute dominator set for each BB.
  // Algorithm based on pseudocode at:
  // https://sbaziotis.com/compilers/visualizing-dominators.html

  // First initialize dominator sets.
  // The entry point only ever has itself as a dominator.
  for (int i = 0; i < bb_count; i++) {
    BasicBlock *bb = bbs[i];
    bb->dominators_set = bitset_new(bb_count);
    if (i == 0) {
      bitset_fill(bb->dominators_set, false, bb_count);
      bitset_set(bb->dominators_set, 0, true);
    } else {
      bitset_fill(bb->dominators_set, true, bb_count);
    }
  }

  // The dominator set of each node is the intersection of all its
  // predecessor's dominator sets. However, as the CFG can have cycles, there
  // is no easy way to compute all predecessor dominator sets in one pass.
  // To solve iteratively compute the tentative dominator sets of each node
  // until the sets don't change anymore.
  uint8_t *tmp = bitset_new(bb_count);
  while (1) {
    bool change = false;
    for (int i = 1; i < bb_count; i++) {
      BasicBlock *bb = bbs[i];
      bitset_fill(tmp, true, bb_count);
      for (int j = 0; j < bb->ins_count; j++) {
        BasicBlock *bb2 = bb->ins[j];
        bitset_intersect(tmp, bb2->dominators_set, bb_count);
      }
      bitset_set(tmp, i, true);
      if (!is_bitset_equal(bb->dominators_set, tmp, bb_count)) {
        change = true;
        bitset_copy(bb->dominators_set, tmp, bb_count);
      }
    }
    if (!change) {
      break;
    }
  }
  bitset_free(tmp);
}

static void compute_cfg_reachability(BasicBlock **bbs, int bb_count) {
  // Compute the set of reachable BB's for each BB.
  // Similar algorithm to computing the dominator tree above.
  for (int i = 0; i < bb_count; i++) {
    BasicBlock *bb = bbs[i];
    bb->reachable_set = bitset_new(bb_count);
    bitset_fill(bb->reachable_set, false, bb_count);
  }
  uint8_t *tmp = bitset_new(bb_count);
  while (1) {
    bool change = false;
    for (int i = 1; i < bb_count; i++) {
      BasicBlock *bb = bbs[i];
      bitset_fill(tmp, false, bb_count);
      for (int j = 0; j < bb->outs_count; j++) {
        BasicBlock *bb2 = bb->outs[j];
        bitset_union(tmp, bb2->reachable_set, bb_count);
      }
      bitset_set(tmp, i, true);
      if (!is_bitset_equal(bb->reachable_set, tmp, bb_count)) {
        change = true;
        bitset_copy(bb->reachable_set, tmp, bb_count);
      }
    }
    if (!change) {
      break;
    }
  }
  bitset_free(tmp);
}

static void _dfs_backward_edges(uint8_t *visited, uint8_t *visiting,
                                BasicBlock *bb) {
  bitset_set(visited, bb->idx, true);
  bitset_set(visiting, bb->idx, true);
  for (int i = 0; i < bb->outs_count; i++) {
    BasicBlock *bb2 = bb->outs[i];
    if (bitset_get(visiting, bb2->idx)) {
      bb->backward_outs[bb->backward_outs_count] = bb2;
      bb->backward_outs_count += 1;
      bb2->is_loop_header = true;
      continue;
    }
    bb->forward_outs[bb->forward_outs_count] = bb2;
    bb->forward_outs_count += 1;
    if (bitset_get(visited, bb2->idx)) {
      continue;
    }
    _dfs_backward_edges(visited, visiting, bb2);
  }
  bitset_set(visiting, bb->idx, false);
}

static void compute_cfg_backward_edges(BasicBlock **bbs, int bb_count) {
  uint8_t *visited = bitset_new(bb_count);
  uint8_t *visiting = bitset_new(bb_count);
  _dfs_backward_edges(visited, visiting, bbs[0]);
  bitset_free(visiting);
  bitset_free(visited);
}

static bool bb_is_dominated_by(BasicBlock *bb1, BasicBlock *bb2) {
  return bitset_get(bb1->dominators_set, bb2->idx);
}

static bool bb_can_reach(BasicBlock *bb1, BasicBlock *bb2) {
  return bitset_get(bb1->reachable_set, bb2->idx);
}

static bool compute_cfg_is_reducible(BasicBlock **bbs, int bb_count) {
  // Reducible if for every backward edge A->B, B dominates A.
  // In other words, all loops only have a single entry.
  for (int i = 0; i < bb_count; i++) {
    for (int j = 0; j < bbs[i]->backward_outs_count; j++) {
      if (!bb_is_dominated_by(bbs[i], bbs[j])) {
        return false;
      }
    }
  }
  return true;
}

static void _dfs_topological_sort(BasicBlock **bbs_out, int *bbs_out_count,
                                  uint8_t *visited, BasicBlock *bb) {
  bitset_set(visited, bb->idx, true);
  for (int i = 0; i < bb->forward_outs_count; i++) {
    BasicBlock *bb2 = bb->forward_outs[i];
    if (bitset_get(visited, bb2->idx)) {
      continue;
    }
    _dfs_topological_sort(bbs_out, bbs_out_count, visited, bb2);
  }
  bbs_out[*bbs_out_count] = bb;
  *bbs_out_count += 1;
}

static void topological_sort(BasicBlock **bbs_out, int *bbs_out_count,
                             BasicBlock **bbs, int bb_count) {
  // Sort BBs such that for every forward edge A->B, A comes before B in the
  // ordering.
  // To achieve this, depth-first-search the forward edge tree. When the DFS
  // reaches a BB with no forward edges, or all forward edges already
  // visited, then add it to a stack. This BB should be after all other
  // BBs in the DFS path that reached it. As the DFS unwinds prior nodes in
  // the path are added to the stack.
  // Finally the stack is reversed to get the topological ordering.
  uint8_t *visited = bitset_new(bb_count);
  *bbs_out_count = 0;
  _dfs_topological_sort(bbs_out, bbs_out_count, visited, bbs[0]);
  bitset_free(visited);

  // Reverse bbs_out.
  if (*bbs_out_count > 1) {
    int left = 0;
    int right = *bbs_out_count - 1;
    while (left < right) {
      BasicBlock *tmp = bbs_out[left];
      bbs_out[left] = bbs_out[right];
      bbs_out[right] = tmp;
      left += 1;
      right -= 1;
    }
  }
}

#if DEBUG_STACKIFIER
static void dump_bbs(BasicBlock **bbs, int bb_count) {
  for (int i = 0; i < bb_count; i++) {
    BasicBlock *bb = bbs[i];
    fprintf(stderr, "bb_%d: outs=", bb->idx);
    if (bb->outs_count == 1) {
      fprintf(stderr, "[bb_%d] ", bb->outs[0]->idx);
    } else if (bb->outs_count == 2) {
      fprintf(stderr, "[bb_%d, bb_%d] ", bb->outs[0]->idx, bb->outs[1]->idx);
    } else {
      fprintf(stderr, "[] ");
    }
    fprintf(stderr, "ins=[");
    for (int j = 0; j < bb->ins_count; j++) {
      fprintf(stderr, "bb_%d, ", bb->ins[j]->idx);
    }
    fprintf(stderr, "] doms=[");
    if (bb->dominators_set != NULL) {
      for (int j = 0; j < bb_count; j++) {
        if (bitset_get(bb->dominators_set, j)) {
          fprintf(stderr, "bb_%d, ", j);
        }
      }
    }
    fprintf(stderr, "] is_loop_header=%d sorted_idx=%d\n",
            (int)bb->is_loop_header, bb->sorted_idx);
  }
}
#endif

static int qsort_cmp_scope(const void *p1, const void *p2) {
  // Comparison callback for qsort.
  // Sort in descending order of 'end' position.
  StackifierScope *scope1 = *(StackifierScope **)p1;
  StackifierScope *scope2 = *(StackifierScope **)p2;
  if (scope1->end < scope2->end) {
    return 1;
  } else if (scope1->end > scope2->end) {
    return -1;
  }
  return 0;
}

static void stackify(BasicBlock *bb_list_head) {
  // Stackifier algorithm.
  // Converts an arbitrary Control Flow Graph to structured loops and blocks.
  // Currently only works for reducible CFG's (does not attempt to split or
  // merge nodes in an irreducible CFG to make it reducible)
  // Based on description of algorithm at:
  //   https://medium.com/leaningtech/solving-the-structured-control-flow-problem-once-and-for-all-5123117b1ee2

  // First need to convert BB linked list to an array of pointers.
  int bb_count = 0;
  for (BasicBlock *bb = bb_list_head; bb; bb = bb->next) {
    bb_count += 1;
  }
  BasicBlock **bbs = calloc(bb_count, sizeof(BasicBlock *));
  for (BasicBlock *bb = bb_list_head; bb; bb = bb->next) {
    bbs[bb->idx] = bb;
  }

  compute_cfg_ins(bbs, bb_count);
  compute_cfg_dominators(bbs, bb_count);
  compute_cfg_reachability(bbs, bb_count);
  compute_cfg_backward_edges(bbs, bb_count);
  bool is_reducible = compute_cfg_is_reducible(bbs, bb_count);

  if (!is_reducible) {
    fprintf(stderr, "warning: stackifier: CFG for %s is not reducible!\n",
            current_fn->name);
  }

  // fprintf(stderr, "%s: bbs unsorted:\n", current_fn->name);
  // dump_bbs(bbs, bb_count);

  // Topological sort.
  // Note that unreachable nodes will be left out of the sorted set.
  BasicBlock **bbs_sorted = calloc(bb_count, sizeof(BasicBlock *));
  int bb_sorted_count = 0;
  topological_sort(bbs_sorted, &bb_sorted_count, bbs, bb_count);

  // Move all BBs that are part of a loop to just beneath their loop header
  // in the topologically sorted BB list.
  // TODO : additionally check that the block is part of the loop cycle.
  // i.e. the loop header is reachable from this BB.
  for (int i = 0; i < bb_sorted_count; i++) {
    BasicBlock *bb = bbs_sorted[i];
    if (!bb->is_loop_header) {
      continue;
    }
    // Find all BBs that are part of this loop (including any nested loops)
    // and move them to just below the loop header.
    // Nested loop headers will be handled in later iterations.
    // Note that nested loops are always dominated by the outer loop header too,
    // as all loops are single entry.
    int k = i + 1;
    for (int j = i + 1; j < bb_sorted_count; j++) {
      BasicBlock *bb2 = bbs_sorted[j];
      if (!bb_can_reach(bb2, bb) || !bb_is_dominated_by(bb2, bb)) {
        continue;
      }
      // bb2 is part of loop. Need to move bb2 to bbs[k].
      // However first need to move bbs[k:j] to bbs[k + 1 : j + 1] to free
      // the slot at bbs[k].
      if ((j - k) > 0) {
        memmove(&bbs_sorted[k + 1], &bbs_sorted[k],
                sizeof(BasicBlock *) * (j - k));
      }
      bbs_sorted[k] = bb2;
      k += 1;
    }
  }

  // Set 'sorted_idx' for each BB in the sorted output.
  for (int i = 0; i < bb_count; i++) {
    bbs[i]->sorted_idx = -1;
  }
  for (int i = 0; i < bb_sorted_count; i++) {
    bbs_sorted[i]->sorted_idx = i;
  }

  // Check sort conditions satisfied.
  for (int i = 0; i < bb_sorted_count; i++) {
    BasicBlock *bb = bbs_sorted[i];
    for (int j = 0; j < bb->forward_outs_count; j++) {
      BasicBlock *bb2 = bb->forward_outs[j];
      if (bb2->sorted_idx <= i) {
        fprintf(stderr,
                "error: function %s: bb_%d should be before bb_%d in the "
                "sorted order",
                current_fn->name, bb->idx, bb2->idx);
      }
    }
    if (!bb->is_loop_header) {
      continue;
    }
    bool should_be_loop = true;
    for (int j = i + 1; j < bb_sorted_count; j++) {
      BasicBlock *bb2 = bbs_sorted[j];
      bool is_bb2_in_loop =
          bb_can_reach(bb2, bb) && bb_is_dominated_by(bb2, bb);
      if (should_be_loop) {
        if (!is_bb2_in_loop) {
          should_be_loop = false;
        }
      } else {
        if (is_bb2_in_loop) {
          fprintf(stderr,
                  "error: function %s: bb_%d is part of loop bb_%d, but is not "
                  "contiguous with "
                  "other loop blocks\n",
                  current_fn->name, bb2->idx, bb->idx);
        }
      }
    }
  }

#if DEBUG_STACKIFIER
  fprintf(stderr, "%s: bbs sorted:\n", current_fn->name);
  dump_bbs(bbs_sorted, bb_sorted_count);
#endif

  // Enclose each loop in a loop scope (while(1) { /*...*/ }).
  // All back edges become 'continue' statements.

  // Now we are left with the forward edges. If the source and destination
  // blocks are consecutive in the topological order, we don’t need to do
  // anything.

  // Otherwise, we need to place a block scope (do{/*...*/} while(0)), such that
  // the destination block is just after the end of the scope.

  // Note that only one loop can start on a node, but multiple loops can end on
  // a node. Multiple block scopes can start on a node, but only one block scope
  // can end on a node.

  StackifierScope scopes_head = {};
  StackifierScope *scopes_tail = &scopes_head;

  // Find loop scopes.
  IntVector *loop_stack = intvector_new();
  for (int i = 0; i < bb_sorted_count; i++) {
    BasicBlock *bb = bbs_sorted[i];
    // Check for end of loop.
    // TODO : should the end of loop be at the last back edge?
    while (loop_stack->length > 0) {
      int loop_header_idx = intvector_get(loop_stack, loop_stack->length - 1);
      BasicBlock *loop_header_bb = bbs_sorted[loop_header_idx];
      if (bb_can_reach(bb, loop_header_bb) &&
          bb_is_dominated_by(bb, loop_header_bb)) {
        // Still in loop.
        break;
      }
      // fprintf(stderr, "bb_%d not part of loop bb_%d\n", bb->idx,
      // loop_header_bb->idx); This is the first block that is not part of the
      // loop.
      StackifierScope *scope = calloc(1, sizeof(StackifierScope));
      scope->kind = SCOPE_LOOP;
      scope->start = loop_header_idx;
      scope->end = i;
      scopes_tail->next = scope;
      scopes_tail = scope;
      intvector_pop(loop_stack);
    }

    // Check for start of loop.
    if (bb->is_loop_header) {
      intvector_push(loop_stack, i);
    }
  }

  // Add remaining loop scopes.
  while (loop_stack->length > 0) {
    int loop_header_idx = intvector_get(loop_stack, loop_stack->length - 1);
    StackifierScope *scope = calloc(1, sizeof(StackifierScope));
    scope->kind = SCOPE_LOOP;
    scope->start = loop_header_idx;
    scope->end = bb_sorted_count;
    scopes_tail->next = scope;
    scopes_tail = scope;
    intvector_pop(loop_stack);
  }

  intvector_free(loop_stack);

  // Find all forward edges that we need to transform into 'breaks'.
  // Will need a block scope that ends just before the destination block.
  // Find the earliest break for each destination. The block scope
  // will need to start at or before this BB.
  int *block_first_breaks = calloc(bb_sorted_count, sizeof(int));
  for (int i = 0; i < bb_sorted_count; i++) {
    block_first_breaks[i] = -1;
  }
  for (int i = 0; i < bb_sorted_count; i++) {
    BasicBlock *bb = bbs_sorted[i];
    for (int j = 0; j < bb->forward_outs_count; j++) {
      BasicBlock *bb2 = bb->forward_outs[j];
      // Ignore forward edges where the destination is the next block in
      // the sorted BB list.
      if (bb2->sorted_idx == (i + 1)) {
        continue;
      }
      // Set this as the earliest break if it is the first break we have
      // seen for this block scope.
      int block_end = bb2->sorted_idx;
      if (block_first_breaks[block_end] < 0) {
        block_first_breaks[block_end] = i;
      }
    }
  }

  // Compute the starts of block scopes by moving them backward from the first
  // 'break' until they do not interleave with any other scopes.
  int *block_starts = calloc(bb_sorted_count, sizeof(int));
  for (int i = 0; i < bb_sorted_count; i++) {
    block_starts[i] = block_first_breaks[i];
  }
  while (1) {
    bool change = false;
    for (int i = 0; i < bb_sorted_count; i++) {
      int first_break = block_first_breaks[i];
      if (first_break < 0) {
        continue;
      }
      int new_start = first_break;
      for (StackifierScope *scope = scopes_head.next; scope;
           scope = scope->next) {
        // The loop scope ends before the BB at scope->end.
        // The block scope ends before the BB at i.
        if ((scope->end > first_break) && (scope->end <= i)) {
          if (scope->start < new_start) {
            new_start = scope->start;
          }
        }
      }
      for (int end = 0; end < bb_sorted_count; end++) {
        int start = block_starts[end];
        if (start < 0) {
          continue;
        }
        if ((end > first_break) && (end <= i)) {
          if (start < new_start) {
            new_start = start;
          }
        }
      }
      if (new_start != block_starts[i]) {
        block_starts[i] = new_start;
        change = true;
      }
    }
    if (!change) {
      break;
    }
  }

  // Add block scopes.
  for (int end = 0; end < bb_sorted_count; end++) {
    int start = block_starts[end];
    if (start < 0) {
      continue;
    }
    StackifierScope *scope = calloc(1, sizeof(StackifierScope));
    scope->kind = SCOPE_BLOCK;
    scope->start = start;
    scope->end = end;
    scopes_tail->next = scope;
    scopes_tail = scope;
  }

  free(block_starts);
  free(block_first_breaks);

  // Sort scopes in descending order of end position.
  // We want to enter scopes that end later first.
  // TODO : Instead of using qsort, just insert into the scopes linked list
  // in the correct order.
  int scope_count = 0;
  for (StackifierScope *scope = scopes_head.next; scope; scope = scope->next) {
    scope_count += 1;
  }
  StackifierScope **scopes = calloc(scope_count, sizeof(StackifierScope *));
  int scope_index = 0;
  for (StackifierScope *scope = scopes_head.next; scope; scope = scope->next) {
    scopes[scope_index++] = scope;
  }
  qsort(scopes, scope_count, sizeof(StackifierScope *), qsort_cmp_scope);

#if DEBUG_STACKIFIER
  fprintf(stderr, "bbs: %s: unsorted=%d sorted=%d\n", current_fn->name,
          bb_count, bb_sorted_count);
  fprintf(stderr, "scopes: %s:\n", current_fn->name);
  for (int i = 0; i < scope_count; i++) {
    StackifierScope *scope = scopes[i];
    fprintf(stderr, "  [%d] %s %d->%d (bb_%d->bb_%d)\n", i,
            (scope->kind == SCOPE_LOOP) ? "loop" : "block", scope->start,
            scope->end, bbs_sorted[scope->start]->idx,
            (scope->end >= bb_sorted_count) ? -1 : bbs_sorted[scope->end]->idx);
  }
#endif

  // Output WASM.
  // int depth = 0;
  IntVector *scope_stack = intvector_new();
  for (int i = 0; i < bb_sorted_count; i++) {

    // fprintf(stderr, "  %d: scope_stack=%d\n", i, scope_stack->length);

    while (scope_stack->length > 0) {
      int scope_index = intvector_get(scope_stack, scope_stack->length - 1);
      StackifierScope *scope = scopes[scope_index];
      if (scope->end != i) {
        break;
      }
      // depth -= 1;
      put_wasm("  )\n");
      // fprintf(stderr, "  %d: pop scope %d %d\n", i, scope->start,
      // scope->end);
      intvector_pop(scope_stack);
    }

    for (int scope_index = 0; scope_index < scope_count; scope_index++) {
      StackifierScope *scope = scopes[scope_index];
      if (scope->start != i) {
        continue;
      }
      if (scope->kind == SCOPE_LOOP) {
        fmt_wasm("  (loop $bb_%d\n", bbs_sorted[scope->start]->idx);
      } else {
        fmt_wasm("  (block $bb_%d\n", bbs_sorted[scope->end]->idx);
      }
      // Check if scopes are interleaved.
      if (scope_stack->length > 0) {
        int parent_scope_index =
            intvector_get(scope_stack, scope_stack->length - 1);
        StackifierScope *parent_scope = scopes[parent_scope_index];
        if (scope->end > parent_scope->end) {
          fprintf(stderr,
                  "warning: function %s: scopes interleaved parent=%d->%d "
                  "new=%d->%d\n",
                  current_fn->name, parent_scope->start, parent_scope->end,
                  scope->start, scope->end);
        }
      }
      // depth += 1;
      // fprintf(stderr, "  %d: push scope %d %d\n", i, scope->start,
      // scope->end);
      intvector_push(scope_stack, scope_index);
    }

    BasicBlock *bb = bbs_sorted[i];
    fmt_wasm("    ;; bb_%d\n", bb->idx);
    for (Insn *insn = bb->insns_head.next; insn; insn = insn->next) {
      if (insn->next == NULL) {
        // Terminator instruction.
        if (bb->outs_count == 0) {
          fmt_wasm("    %s\n", insn->op);
          continue;
        }
        // For unconditional branches skip if this is an unconditional
        // branch to the next BB.
        if (bb->outs_count == 1) {
          if (bb->forward_outs_count == 1 &&
              bb->forward_outs[0]->sorted_idx == (i + 1)) {
            continue;
          }
          fmt_wasm("    %s\n", insn->op);
          continue;
        }
        // For conditional branches convert the intermediate 'br_if' with
        // 'then' and 'else' dests to a 'br_if' and 'br'.
        // Eliminate one or the other if possible.
        BasicBlock *bb_then = bb->outs[0];
        bool bb_then_is_next = (bb_then->sorted_idx == (i + 1));
        BasicBlock *bb_else = bb->outs[1];
        bool bb_else_is_next = (bb_else->sorted_idx == (i + 1));
        if (bb_else_is_next) {
          // Simple case, output br_if, fallthrough to next BB.
          fmt_wasm("    br_if $bb_%d\n", bb_then->idx);
        } else if (bb_then_is_next) {
          // If the 'then' dest is the fallthrough need to reverse the
          // condition and br_if to the 'else' dest.
          put_wasm("    i32.eqz\n");
          fmt_wasm("    br_if $bb_%d\n", bb_else->idx);
        } else {
          // If neither dest is the fallthrough then need to output a
          // br_if and a br.
          fmt_wasm("    br_if $bb_%d\n", bb_then->idx);
          fmt_wasm("    br $bb_%d\n", bb_else->idx);
        }
      } else {
        // Non-terminator instruction.
        fmt_wasm("    %s\n", insn->op);
      }
    }
  }

  // Close remaining scopes (should only ever be loop scopes remaining).
  if (scope_stack->length > 0) {
    while (scope_stack->length > 0) {
      int scope_index = intvector_get(scope_stack, scope_stack->length - 1);
      StackifierScope *scope = scopes[scope_index];
      if (scope->end != bb_sorted_count) {
        fprintf(
            stderr,
            "error: function %s: unclosed scope %d->%d at end of function\n",
            current_fn->name, scope->start, scope->end);
      }
      // depth -= 1;
      put_wasm("  )\n");
      intvector_pop(scope_stack);
    }
    put_wasm("    unreachable\n");
  }

  // Cleanup.
  intvector_free(scope_stack);
  for (int i = 0; i < bb_count; i++) {
    if (bbs[i]->ins != NULL) {
      free(bbs[i]->ins);
    }
    bitset_free(bbs[i]->dominators_set);
    bitset_free(bbs[i]->reachable_set);
  }
  StackifierScope *scope = scopes_head.next;
  while (scope) {
    StackifierScope *scope_next = scope->next;
    free(scope);
    scope = scope_next;
  }
  free(scopes);
  free(bbs_sorted);
  free(bbs);
}

#define WASM_PAGE_SIZE 65536
#define WASM_DATA_START 1024
#define WASM_STACK_SIZE 65536

static void calculate_gvar_offsets(Obj *prog_obj, int *data_offset) {
  // First calculate offsets and indexes of global vars.
  for (Obj *var = prog_obj; var; var = var->next) {
    if (var->is_function) {
      continue;
    }
    if (!var->is_definition) {
      continue;
    }

    *data_offset = align_to(*data_offset, var->ty->align);
    var->offset = *data_offset;

    *data_offset += var->ty->size;
  }
}

static void get_exports(Obj *prog_obj, StringArray *exports) {
  // Collect all exported function and global var names.
  for (Obj *var = prog_obj; var; var = var->next) {
    if (!var->is_definition || var->is_static) {
      continue;
    }
    strarray_push(exports, var->name);
  }
}

static void emit_types(Obj *prog_obj, int *type_idx) {
  for (Obj *fn = prog_obj; fn; fn = fn->next) {
    if (!fn->is_live || !fn->is_definition) {
      continue;
    }
    for (Node *node = fn->funcalls; node; node = node->funcall_next) {
      Type *ty = node->func_ty;
      if (ty->wasm_idx) {
        continue;
      }

      fmt_wasm("(type (;%d;) (func (param", *type_idx);
      for (Type *param_ty = ty->params; param_ty; param_ty = param_ty->next) {
        fmt_wasm(" %s", get_wasm_type(param_ty));
      }
      // If variadic add a pointer to the va_arg_area buffer.
      if (fn->va_area) {
        fmt_wasm(" i32");
      }
      if (ty->return_ty->kind != TY_VOID) {
        fmt_wasm(") (result %s)))\n", get_wasm_type(ty->return_ty));
      } else {
        put_wasm(")))\n");
      }

      ty->wasm_idx = *type_idx + 1;
      *type_idx += 1;
    }
  }
}

static void emit_imports(Obj *prog_obj, StringArray *exports, int *type_idx) {
  for (Obj *fn = prog_obj; fn; fn = fn->next) {
    // Functions without a definition are imports.
    // All static funtions should have definitions.
    if (!fn->is_live || fn->is_static || fn->is_definition) {
      continue;
    }

    // Check if this is a function exported by another translation unit.
    bool skip = false;
    for (int i = 0; i < exports->len; i++) {
      if (strcmp(exports->data[i], fn->name) == 0) {
        skip = true;
        break;
      }
    }
    if (skip) {
      break;
    }

    // Imported function. Add function type and import.
    // TODO : add extra param for functions returning struct.
    if (fn->is_function) {
      Type *return_ty = fn->ty->return_ty;
      // bool returns_struct = (return_ty->kind == TY_STRUCT || return_ty->kind
      // == TY_UNION);

      fmt_wasm("(type (;%d;) (func (param", *type_idx);
      for (Type *param_ty = fn->ty->params; param_ty;
           param_ty = param_ty->next) {
        fmt_wasm(" %s", get_wasm_type(param_ty));
      }
      // If variadic add a pointer to the va_arg_area buffer.
      if (fn->va_area) {
        fmt_wasm(" i32");
      }
      if (return_ty->kind != TY_VOID) {
        fmt_wasm(") (result %s)))\n", get_wasm_type(return_ty));
      } else {
        put_wasm(")))\n");
      }
      fmt_wasm(
          "(import \"wasi_snapshot_preview1\" \"%s\" (func $%s (type %d)))\n",
          fn->name, fn->name, *type_idx);
      *type_idx += 1;
    } else {
      // TODO : imported global vars.
      error("global var imports not supported");
    }
  }
}

static void emit_funcs(Obj *prog_obj) {
  for (Obj *fn = prog_obj; fn; fn = fn->next) {
    if (!fn->is_function || !fn->is_live || !fn->is_definition) {
      continue;
    }

    current_fn = fn;
    local_counter = 0;
    bb_counter = 0;
    current_bbs_head.next = NULL;
    current_bbs_tail = &current_bbs_head;
    current_locals_head.next = NULL;
    current_locals_tail = &current_locals_head;

    BasicBlock *entry_bb = append_new_bb();
    current_bb = entry_bb;

    current_fn_return_bb = append_new_bb();

    int frame_offset = 0;

    Type *return_ty = fn->ty->return_ty;
    bool returns_struct =
        (return_ty->kind == TY_STRUCT || return_ty->kind == TY_UNION);
    bool has_result = ((return_ty->kind != TY_VOID) && !returns_struct);

    // Count params.
    int param_count = 0;
    for (Obj *var = fn->params; var; var = var->next) {
      param_count += 1;
    }

    // Create stack space and assign local indexes for all locals (including
    // named local vars, function params, and anon locals created by the
    // parser).
    for (Obj *var = fn->locals; var; var = var->next) {
      frame_offset = align_to(frame_offset, var->ty->align);
      var->offset = frame_offset;
      frame_offset += var->ty->size;
    }

    // Function prologue.
    // Create stack frame by adjusting global stack pointer.
    // Alloc local for the frame pointer.
    put_insn("global.get $__stack_pointer");
    fmt_insn("i32.const %d", frame_offset);
    put_insn("i32.sub");
    put_insn("local.set $fp");
    put_insn("local.get $fp");
    put_insn("global.set $__stack_pointer");

    // Increment local counter for params, hidden va_arg_area param, $fp, and
    // $result.
    local_counter += param_count;
    if (fn->va_area) {
      local_counter += 1;
    }
    local_counter += 1;
    if (has_result) {
      local_counter += 1;
    }

    // Copy params to stack frame.
    param_count = 0;
    for (Obj *var = fn->params; var; var = var->next) {
      put_insn("local.get $fp");
      fmt_insn("i32.const %d", var->offset);
      put_insn("i32.add");
      fmt_insn("local.get $p%d", param_count);
      store(var->ty);
      param_count++;
    }

    // For variadic functions setup the hidden __va_area__ local var.
    // The last parameter (also hidden) is a pointer to a stack buffer in the
    // callers stack frame that contains the variadic args.
    if (fn->va_area) {
      gen_lvar_addr(fn->va_area);
      fmt_insn("local.get $p%d", param_count);
      put_insn("i32.store");
    }

    // Emit function body.
    gen_stmt(fn->body);

    BasicBlock *last_bb = current_bb;

    // Add branches to the end of goto basic blocks now that the basic blocks
    // for all labels have been created.
    for (Node *goto_nd = fn->gotos; goto_nd; goto_nd = goto_nd->goto_next) {
      for (Node *label_nd = fn->labels; label_nd;
           label_nd = label_nd->goto_next) {
        if (!strcmp(goto_nd->label, label_nd->label)) {
          set_current_bb(goto_nd->goto_bb);
          if (!is_current_bb_terminated()) {
            put_insn_br(label_nd->goto_bb);
          }
        }
      }
    }

    set_current_bb(last_bb);

    // Need to add a return if there wasn't one.
    // TODO : remove the current BB rather than adding a return if it is
    // unreachable.
    if (!is_current_bb_terminated()) {
      if (strcmp(fn->name, "main") == 0) {
        // [https://www.sigbus.info/n1570#5.1.2.2.3p1] The C spec defines
        // a special rule for the main function. Reaching the end of the
        // main function is equivalent to returning 0, even though the
        // behavior is undefined for the other functions.
        put_insn("i32.const 0");
      } else if (fn->is_noreturn) {
        put_insn_unreachable();
      } else if (return_ty->kind == TY_VOID) {
        // return nothing.
      } else if (returns_struct) {
        // return nothing.
      } else {
        // Return uninitialized value.
        // TODO : ???
      }
      put_insn_br(current_fn_return_bb);
    }

    // Emit function epilogue.
    set_current_bb(current_fn_return_bb);
    put_insn("local.get $fp");
    fmt_insn("i32.const %d", frame_offset);
    put_insn("i32.add");
    put_insn("global.set $__stack_pointer");
    if (has_result) {
      put_insn("local.get $result");
    }
    put_insn_return();

    if (fn->is_static) {
      fmt_wasm("(func $%s.%d\n", fn->name, fn->prog->index);
    } else {
      fmt_wasm("(func $%s\n", fn->name);
    }
    // ret_buffer hidden parameter for returning structs is included in
    // fn->params.
    param_count = 0;
    for (Obj *var = fn->params; var; var = var->next) {
      fmt_wasm("  (param $p%d %s)\n", param_count, get_wasm_type(var->ty));
      param_count += 1;
    }

    if (fn->va_area) {
      fmt_wasm("  (param $p%d i32)\n", param_count);
      param_count += 1;
    }

    if (has_result) {
      fmt_wasm("  (result %s)\n", get_wasm_type(return_ty));
    }

    put_wasm("  (local $fp i32)\n");
    if (has_result) {
      fmt_wasm("  (local $result %s)\n", get_wasm_type(return_ty));
    }
    for (WasmLocal *local = current_locals_head.next; local;
         local = local->next) {
      fmt_wasm("  (local (;%d;) %s)\n", local->idx, get_wasm_type(local->ty));
    }

    stackify(current_bbs_head.next);

    put_wasm(")\n");

    current_fn = NULL;
    local_counter = 0;
    bb_counter = 0;
    current_bbs_head.next = NULL;
    current_bbs_tail = NULL;
    current_fn_return_bb = NULL;
    current_locals_head.next = NULL;
    current_locals_tail = NULL;
  }
}

static void calculate_func_table_indices(Obj *prog_obj, int *table_idx) {
  for (Obj *fn = prog_obj; fn; fn = fn->next) {
    if (!fn->is_function || !fn->is_live || !fn->is_definition) {
      continue;
    }
    fn->offset = *table_idx;
    *table_idx += 1;
  }
}

static void emit_func_table_elems(Obj *prog_obj) {
  for (Obj *fn = prog_obj; fn; fn = fn->next) {
    if (!fn->is_function || !fn->is_live || !fn->is_definition) {
      continue;
    }
    if (fn->is_static) {
      fmt_wasm("(elem (i32.const %d) $%s.%d)\n", fn->offset, fn->name,
               fn->prog->index);
    } else {
      fmt_wasm("(elem (i32.const %d) $%s)\n", fn->offset, fn->name);
    }
  }
}

static void emit_exports(Obj *prog_obj) {
  // Function exports for non-static, defined functions.
  // TODO : global var exports.
  for (Obj *fn = prog_obj; fn; fn = fn->next) {
    if (!fn->is_function || !fn->is_live || !fn->is_definition ||
        fn->is_static) {
      continue;
    }
    fmt_wasm("(export \"%s\" (func $%s))\n", fn->name, fn->name);
  }
}

static int encode_name(char *buf, size_t buf_size, const char *s) {
  int pos = 0;
  while (*s) {
    int c = *s;
    s++;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') || (c == '_') || (c == '.')) {
      if (pos < buf_size) {
        buf[pos] = c;
      }
      pos += 1;
    } else {
      for (int i = 0; i < 2; i++) {
        int n = (c >> 4) & 0xf;
        c <<= 4;
        char hex;
        if (n < 10) {
          hex = '0' + n;
        } else {
          hex = 'a' + n - 10;
        }
        if (pos < buf_size) {
          buf[pos] = hex;
        }
        pos += 1;
      }
    }
  }
  if (pos < buf_size) {
    buf[pos] = 0;
  } else if (buf_size > 0) {
    buf[buf_size - 1] = 0;
  }
  return pos;
}

static void emit_globals(Obj *prog_obj) {
  // Add '(global ...)' definitions.
  for (Obj *var = prog_obj; var; var = var->next) {
    if (var->is_function) {
      continue;
    }

    // For global vars is_definition is false if the var has the 'extern'
    // attribute.
    if (!var->is_definition) {
      // TODO : add import.
      continue;
    }

    // TODO : add export for non-static vars.
    if (!var->is_static) {
    }

    // TODO : is_const should be part of the type?
    // TODO : can't really mark memory locations constant?
    if (var->is_const) {
    }

    // TODO : thread local global vars.
    if (var->is_tls) {
    }

    // TODO : need to encode names to safe characters (no unicode).
    // The names get lost when converting to binary anyway, so can remove? Use
    // numeric IDs?
    int n = encode_name(NULL, 0, var->name);
    char *encoded_name = calloc(n + 1, sizeof(char));
    encode_name(encoded_name, n + 1, var->name);
    if (var->is_static) {
      fmt_wasm("(global $%s.%d i32 (i32.const %d))\n", encoded_name,
               var->prog->index, var->offset);
    } else {
      fmt_wasm("(global $%s i32 (i32.const %d))\n", encoded_name, var->offset);
    }
    free(encoded_name);
  }
}

static void emit_data(Obj *prog_obj) {
  // Add '(data ...)' memory initialization data for global vars with
  // initializers.
  for (Obj *var = prog_obj; var; var = var->next) {
    if (var->is_function || !var->is_definition || !var->init_data) {
      continue;
    }

    fmt_wasm("(data (i32.const %d) \"", var->offset);
    int pos = 0;
    Relocation *rel = var->rel;
    while (pos < var->ty->size) {
      if (rel && rel->offset == pos) {
        uint32_t rel_var_offset = rel->var->offset;
        fmt_wasm("\\%02x\\%02x\\%02x\\%02x", (rel_var_offset >> 0) & 0xff,
                 (rel_var_offset >> 8) & 0xff, (rel_var_offset >> 16) & 0xff,
                 (rel_var_offset >> 24) & 0xff);
        pos += 4;
      } else {
        uint8_t c = var->init_data[pos] & 0xff;
        if (c >= 0x20 && c <= 0x7e && c != '\\' && c != '"') {
          fmt_wasm("%c", c);
        } else {
          fmt_wasm("\\%02x", var->init_data[pos]);
        }
        pos += 1;
      }
    }
    put_wasm("\")\n");
  }
}

void codegen(Prog *progs, FILE *out) {
  current_out_file = out;

  int data_offset = WASM_DATA_START;
  for (Prog *prog = progs; prog; prog = prog->next) {
    calculate_gvar_offsets(prog->obj, &data_offset);
  }
  int data_end_offset = data_offset;

  StringArray exports = {};
  for (Prog *prog = progs; prog; prog = prog->next) {
    get_exports(prog->obj, &exports);
  }

  put_wasm("(module $module0\n");

  // Canonical order is:
  //  types, imports, funcs, tables, memory, globals, exports, start, elems,
  //  datacounts, code, data

  int type_idx = 0;

  for (Prog *prog = progs; prog; prog = prog->next) {
    emit_types(prog->obj, &type_idx);
  }

  for (Prog *prog = progs; prog; prog = prog->next) {
    emit_imports(prog->obj, &exports, &type_idx);
  }

  for (Prog *prog = progs; prog; prog = prog->next) {
    emit_funcs(prog->obj);
  }

  int table_idx = 0;
  for (Prog *prog = progs; prog; prog = prog->next) {
    calculate_func_table_indices(prog->obj, &table_idx);
  }

  fmt_wasm("(table %d funcref)\n", table_idx);

  for (Prog *prog = progs; prog; prog = prog->next) {
    emit_func_table_elems(prog->obj);
  }

  // Stack starts after global variables.
  int stack_start_offset = align_to(data_end_offset, WASM_PAGE_SIZE);
  int stack_end_offset = stack_start_offset + WASM_STACK_SIZE;
  int memory_size = align_to(stack_end_offset, WASM_PAGE_SIZE);

  // Memory size in 4096 B pages.
  fmt_wasm("(memory %d)\n", memory_size / WASM_PAGE_SIZE);

  // Stack pointer is always the first global.
  fmt_wasm("(global $__stack_pointer (mut i32) (i32.const %d))\n",
           stack_end_offset);

  for (Prog *prog = progs; prog; prog = prog->next) {
    emit_globals(prog->obj);
  }

  // Add "memory" export (WASI requirement).
  put_wasm("(export \"memory\" (memory 0))\n");

  for (Prog *prog = progs; prog; prog = prog->next) {
    emit_exports(prog->obj);
  }

  for (Prog *prog = progs; prog; prog = prog->next) {
    emit_data(prog->obj);
  }

  put_wasm(")\n");

  current_out_file = NULL;
}
