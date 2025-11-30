#include "chibicc.h"

typedef struct Insn {
  struct Insn *next;
  char *op;
} Insn;

#define MAX_BB_OUTS 2

typedef struct BasicBlock {
  struct BasicBlock *next;
  int idx;
  int sorted_idx;
  Insn *insns_head;
  Insn *insns_tail;
  bool is_terminated;
  int outs_count;
  struct BasicBlock *outs[MAX_BB_OUTS];
  int forward_outs_count;
  struct BasicBlock *forward_outs[MAX_BB_OUTS];
  int backward_outs_count;
  struct BasicBlock *backward_outs[MAX_BB_OUTS];
  // int ins_count;
  // struct BasicBlock **ins;
  uint8_t *ins_set;
  uint8_t *dominators_set;
  bool is_loop_header;
} BasicBlock;

typedef enum ScopeKind {
  SCOPE_BLOCK,
  SCOPE_LOOP
} ScopeKind;

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

static FILE *current_out_file = NULL;
static Obj *current_function = NULL;

static int current_function_local_counter = 0;
static int current_function_bb_counter = 0;
static BasicBlock *current_function_bbs_head = NULL;
static BasicBlock *current_function_bbs_tail = NULL;
static BasicBlock *current_function_return_bb = NULL;
static BasicBlock *current_bb = NULL;

static BasicBlock *current_break_bb = NULL;
static BasicBlock *current_continue_bb = NULL;

static void gen_expr(Node *node);
static void gen_stmt(Node *node);

static void put_wasm(char *text) {
  fputs(text, current_out_file);
}

__attribute__((format(printf, 1, 2)))
static void fmt_wasm(char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(current_out_file, fmt, ap);
  va_end(ap);
}

static BasicBlock *append_new_bb(void) {
  BasicBlock *bb = calloc(1, sizeof(BasicBlock));

  bb->idx = current_function_bb_counter;
  bb->sorted_idx = -1;
  current_function_bb_counter += 1;

  if (current_function_bbs_head == NULL) {
    current_function_bbs_head = bb;
  } else {
    current_function_bbs_tail->next = bb;
  }
  current_function_bbs_tail = bb;
  return bb;
}

static void set_current_bb(BasicBlock *bb) {
  current_bb = bb;
}

static void put_insn(char *op) {
  if (current_bb->is_terminated) {
    fprintf(stderr, "warning: adding insn to terminated bb\n");
  }

  Insn *insn = calloc(1, sizeof(Insn));
  insn->op = op;
  if (current_bb->insns_head == NULL) {
    current_bb->insns_head = insn;
  } else {
    current_bb->insns_tail->next = insn;
  }
  current_bb->insns_tail = insn;
}

__attribute__((format(printf, 1, 2)))
static void fmt_insn(char *fmt, ...) {
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

static int alloc_local(void) {
  int idx = current_function_local_counter;
  current_function_local_counter += 1;
  return idx;
}

// Round up `n` to the nearest multiple of `align`. For instance,
// align_to(5, 8) returns 8 and align_to(11, 8) returns 16.
int align_to(int n, int align) {
  return (n + align - 1) / align * align;
}

static bool is_current_bb_terminated(void) {
  return current_bb->is_terminated;
}

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
  case TY_LDOUBLE:
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
    case TY_LDOUBLE:
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
  fmt_insn("i32.const %d", var->offset);
  put_insn("i32.add");
}

// Compute the absolute address of a given node.
// It's an error if a given node does not reside in memory.
static void gen_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR:
    // For VLA locals the address of the alloca for the array is stored in the local
    // var alloca. So we need to load the local to get the address of the array.
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
    put_insn("drop");
    gen_addr(node->rhs);
    return;
  case ND_MEMBER:
    gen_addr(node->lhs);
    fmt_insn("i32.const %d", node->member->offset);
    put_insn("i32.add");
    return;
  case ND_FUNCALL:
    // If the function returns a struct, then it should be copied to the local var 'ret_buffer'.
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
    // This node type wraps an ND_VAR node when a VLA is assigned to a local var.
    // Seems to be used to prevent the var from being loaded as would normally be done to
    // get the address of the contained VLA.
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
  case TY_LDOUBLE:
    put_insn("f64.load");
    return;
  default:
    break;
  }

  const char *sx = ty->is_unsigned ? "u" : "s";
  if (ty->size == 1) {
    fmt_insn("i32.load8_%s", sx);
  }
  else if (ty->size == 2) {
    fmt_insn("i32.load16_%s", sx);
  }
  else if (ty->size == 4) {
    put_insn("i32.load");
  }
  else if (ty->size == 8) {
    put_insn("i64.load");
  }
  else {
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
  case TY_LDOUBLE:
    put_insn("f64.store");
    return;
  default:
    break;
  }

  if (ty->size == 1) {
    put_insn("i32.store8");
  }
  else if (ty->size == 2) {
    put_insn("i32.store16");
  }
  else if (ty->size == 4) {
    put_insn("i32.store");
  }
  else if (ty->size == 8) {
    put_insn("i64.store");
  }
  else {
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
  case TY_LDOUBLE:
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
  {NULL,  NULL,   NULL,   i32i64, i32u8, i32u16, NULL,   i32i64, i32f32, i32f64}, // i8
  {i32i8, NULL,   NULL,   i32i64, i32u8, i32u16, NULL,   i32i64, i32f32, i32f64}, // i16
  {i32i8, i32i16, NULL,   i32i64, i32u8, i32u16, NULL,   i32i64, i32f32, i32f64}, // i32
  {i32i8, i32i16, NULL,   NULL,   i32u8, i32u16, NULL,   NULL,   i64f32, i64f64}, // i64

  {i32i8, NULL,   NULL,   i32i64, NULL,  NULL,   NULL,   i32i64, i32f32, i32f64}, // u8
  {i32i8, i32i16, NULL,   i32i64, i32u8, NULL,   NULL,   i32i64, i32f32, i32f64}, // u16
  {i32i8, i32i16, NULL,   u32i64, i32u8, i32u16, NULL,   u32i64, u32f32, u32f64}, // u32
  {i32i8, i32i16, NULL,   NULL,   i32u8, i32u16, NULL,   NULL,   u64f32, u64f64}, // u64

  {f32i8, f32i16, f32i32, f32i64, f32u8, f32u16, f32u32, f32u64, NULL,   f32f64}, // f32
  {f64i8, f64i16, f64i32, f64i64, f64u8, f64u16, f64u32, f64u64, f64f32, NULL, }, // f64
};

static void cast(Type *from, Type *to) {
  if (to->kind == TY_VOID) {
    return;
  }

  if (to->kind == TY_BOOL) {
    cmp_zero(from);
    cmp_zero(from);
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
    case TY_LDOUBLE:
      fmt_insn("f64.const %f", node->fval);
      return;
    case TY_LONGLONG:
      fmt_insn("i64.const %ld", node->val);
      return;
    default:
      break;
    }
    fmt_insn("i32.const %ld", node->val);
    return;
  }
  case ND_NEG: {
    gen_expr(node->lhs);
    switch (node->ty->kind) {
    case TY_FLOAT:
      put_insn("f32.neg");
      return;
    case TY_DOUBLE:
    case TY_LDOUBLE:
      put_insn("f64.neg");
      return;
    case TY_LONGLONG:
      put_insn("i64.neg");
      return;
    default:
      break;
    }
    put_insn("i32.neg");
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

    store(node->ty);
    return;
  }
  case ND_STMT_EXPR:
    for (Node *n = node->body; n; n = n->next) {
      gen_stmt(n);
    }
    return;
  case ND_COMMA:
    gen_expr(node->lhs);
    put_insn("drop");
    gen_expr(node->rhs);
    return;
  case ND_CAST:
    gen_expr(node->lhs);
    cast(node->lhs->ty, node->ty);
    return;
  case ND_MEMZERO:
    // Node is always a local var.
    gen_lvar_addr(node->var);
    put_insn("i32.const 0");
    fmt_insn("i32.const %d", node->var->ty->size);
    put_insn("memory.fill");
    return;
  case ND_COND: {
    // Ternary expression.
    // TODO : need to alloc a local for result?
    BasicBlock *then_bb = append_new_bb();
    BasicBlock *else_bb = append_new_bb();
    BasicBlock *end_bb = append_new_bb();
    gen_expr(node->cond);
    cmp_zero(node->cond->ty);
    put_insn_br_if(else_bb, then_bb);

    set_current_bb(then_bb);
    gen_expr(node->then);
    put_insn_br(end_bb);
  
    set_current_bb(else_bb);
    gen_expr(node->els);
    put_insn_br(end_bb);

    set_current_bb(end_bb);
    return;
  }
  case ND_NOT:
    gen_expr(node->lhs);
    cmp_zero(node->lhs->ty);
    return;
  case ND_BITNOT:
    gen_expr(node->lhs);
    if (node->lhs->ty->kind == TY_LONGLONG) {
        put_insn("i64.const 0");
        put_insn("i64.xor");
    } else {
        put_insn("i32.const 0");
        put_insn("i32.xor");
    }
    return;
  case ND_LOGAND: {
    // Logical AND with short-circuit. Skip the RHS expression if the LHS is false.
    BasicBlock *rhs_bb = append_new_bb();
    BasicBlock *end_bb = append_new_bb();
    int cond_local = alloc_local();
    gen_expr(node->lhs);
    cmp_zero(node->lhs->ty);
    fmt_insn("local.set %d", cond_local);
    fmt_insn("local.get %d", cond_local);
    put_insn_br_if(end_bb, rhs_bb);

    set_current_bb(rhs_bb);
    gen_expr(node->rhs);
    cmp_zero(node->rhs->ty);
    put_insn("i32.eqz");
    fmt_insn("local.set %d", cond_local);
    put_insn_br(end_bb);

    set_current_bb(end_bb);
    fmt_insn("local.get %d", cond_local);
    // if (node->ty->kind == TY_VOID) {
    //   put_insn("drop");
    // }
    return;
  }
  case ND_LOGOR: {
    // Logical OR with short-circuit. Skip the RHS expression if the LHS is true.
    BasicBlock *rhs_bb = append_new_bb();
    BasicBlock *end_bb = append_new_bb();
    int cond_local = alloc_local();
    gen_expr(node->lhs);
    cmp_zero(node->lhs->ty);
    put_insn("i32.eqz");
    fmt_insn("local.set %d", cond_local);
    fmt_insn("local.get %d", cond_local);
    put_insn_br_if(end_bb, rhs_bb);

    set_current_bb(rhs_bb);
    put_insn("drop");
    gen_expr(node->rhs);
    cmp_zero(node->rhs->ty);
    put_insn("i32.eqz");
    fmt_insn("local.set %d", cond_local);
    put_insn_br(end_bb);

    set_current_bb(end_bb);
    fmt_insn("local.get %d", cond_local);
    // if (node->ty->kind == TY_VOID) {
    //   put_insn("drop");
    // }
    return;
  }
  case ND_FUNCALL: {
    // Build args.
    // If the return type is a struct/union, the caller passes
    // a pointer to a stack buffer as if it were the last argument.
    // TODO : Make it the first argument to match clang behavior.
    bool returns_struct = node->ret_buffer;
    if (returns_struct) {
      gen_lvar_addr(node->ret_buffer);
    }

    // Try a direct call if the LHS is a function name.
    // Otherwise do an indirect call using the function table.
    if (node->lhs->kind == ND_VAR && node->lhs->ty->kind == TY_FUNC) {
      fmt_insn("call $%s", node->lhs->var->name);
    } else {
      gen_expr(node->lhs);
      put_insn("call_indirect");
    }
  
    if (returns_struct) {
      gen_lvar_addr(node->ret_buffer);
    }

    // If this is a non-returning function, then we should add an unreachable instruction.
    // How do we get the function obj (global) from the function call node?
    // Or can we add is_noreturn to the type at parse time?
    // if (func_var->is_noreturn) {
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
  case ND_ALLOCA: {
    // Get alloca size.
    // gen_expr(node->args);
    // Adjust stack pointer.
    error_tok(node->tok, "alloca builtin not supported");
  }
  case ND_VA_START: {
    // gen_expr(node->args);
    error_tok(node->tok, "va_start builtin not supported");
  }
  case ND_VA_COPY: {
    // gen_expr(node->args);
    // gen_expr(node->args->next);
    error_tok(node->tok, "va_copy builtin not supported");
  }
  case ND_VA_END: {
    // gen_expr(node->args);
    error_tok(node->tok, "va_end builtin not supported");
  }
  case ND_VA_ARG: {
    // gen_expr(node->args);
    error_tok(node->tok, "va_arg builtin not supported");
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
  case TY_DOUBLE:
  case TY_LDOUBLE: {
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
  case TY_VLA:
  {
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
        put_insn("i32.add");
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
          put_insn("i32.lt");
          return;
        case ND_LE:
          put_insn("i32.le");
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
      put_insn("i32.mod_u");
    } else {
      put_insn("i32.mod_a");
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

static void gen_stmt(Node *node) {
  switch (node->kind) {
  case ND_IF: {
    BasicBlock *then_bb = append_new_bb();
    BasicBlock *else_bb = append_new_bb();
    BasicBlock *end_bb = append_new_bb();
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
    // However if there are GNU case ranges then we need to check those, and if they don't match
    // go to the default block.

    // Switch condition value.
    gen_expr(node->cond);
    int cond_local = alloc_local();
    fmt_insn("set.local %d", cond_local);

    // Generate branches to case labels.
    BasicBlock *next_bb = NULL;
    for (Node *nd = node->case_next; nd; nd = nd->case_next) {
      bool is_i64 = (node->cond->ty->size > 4);
      
      nd->goto_bb = append_new_bb();

      fmt_insn("get.local %d", cond_local);
      if (nd->begin == nd->end) {
        // Normal single integer case.
        if (is_i64) {
          fmt_insn("i64.const %ld", nd->begin);
          put_insn("i64.eq");
        } else {
          fmt_insn("i32.const %ld", nd->begin);
          put_insn("i32.eq");
        }
      }
      else {
        // [GNU] case ranges.
        if (is_i64) {
          fmt_insn("i64.const %ld", nd->begin);
          put_insn("i64.sub");
          fmt_insn("i64.const %ld", nd->end - nd->begin);
          put_insn("i64.le");
        } else {
          fmt_insn("i32.const %ld", nd->begin);
          put_insn("i32.sub");
          fmt_insn("i32.const %ld", nd->end - nd->begin);
          put_insn("i32.le");
        }
      }
      next_bb = append_new_bb();
      put_insn_br_if(node->goto_bb, next_bb);
      set_current_bb(next_bb);
    }

    BasicBlock *end_bb = append_new_bb();

    if (node->default_case) {
      node->default_case->goto_bb = append_new_bb();
      put_insn_br(node->default_case->goto_bb);
    } else {
      put_insn_br(end_bb);
    }

    BasicBlock *prev_break_bb = current_break_bb;
    current_break_bb = end_bb;

    // Generate code for switch body.
    // Includes case labels and all case bodies.
    gen_stmt(node->then);

    // If the last case does not end with a break, then it will not be terminated.
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
    // Need to add a new BB just in case we write (unreachable) instructions after the
    // goto. WASM validator doesn't like unreachable instructions.
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
    // Add a new basic block (will be unreachable unless it begins with a label).
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
    // Need to add a basic block in case something tries to generate code after the return.
    // It will most likely be unreachable.
    // XXX: This is a bad solution :(
    BasicBlock *next_bb = append_new_bb();
    if (!node->lhs) {
      put_insn_br(current_function_return_bb);
      set_current_bb(next_bb);
      return;
    }
    Type *ty = node->lhs->ty;
    bool returns_struct = (ty->kind == TY_STRUCT) || (ty->kind == TY_UNION);
    if (returns_struct) {
      // Structure returns are written to the buffer pointed to by the last arg.
      Obj *ret_buffer_var = NULL;
      for (Obj *param = current_function->params; param; param = param->next) {
        ret_buffer_var = param;
      }
      gen_lvar_addr(ret_buffer_var);
      gen_addr(node->lhs);
      fmt_insn("i32.const %d", ty->size);
      put_insn("memory.copy");
      put_insn_br(current_function_return_bb);
      set_current_bb(next_bb);
      return;
    }
    gen_expr(node->lhs);
    put_insn("local.set $result");
    put_insn_br(current_function_return_bb);
    set_current_bb(next_bb);
    return;
  }
  case ND_EXPR_STMT:
    gen_expr(node->lhs);
    return;
  case ND_ASM:
    error_tok(node->tok, "inline asm not supported");
    return;
  default:
    break;
  }

  error_tok(node->tok, "invalid statement");
}

static int get_bitset_size(int count) {
  return (count + 7) / 8;
}

static uint8_t *bitset_new(int count) {
  return calloc(get_bitset_size(count), sizeof(uint8_t));
}

static void bitset_free(uint8_t *bitset) {
  free(bitset);
}

static void bitset_fill(uint8_t *bitset, bool val, int count) {
  int size = get_bitset_size(count);
  if (size > 1) {
    memset(bitset, val ? 0xff : 0, size - 1);
  }
  int n = count % 8;
  if (n != 0) {
    if (val) {
      bitset[size - 1] |= 0xff >> (8 - n);
    } else {
      bitset[size - 1] = 0;
    }
  }
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

static int bitset_popcnt(uint8_t *bitset, int count) {
  // TODO : optimize.
  int popcnt = 0;
  for (int i = 0; i < count; i++) {
    if (bitset_get(bitset, i)) {
      popcnt += 1;
    }
  }
  return popcnt;
}

static void bitset_intersect(uint8_t *bitset1, uint8_t *bitset2, int count) {
  int size = get_bitset_size(count);
  for (int i = 0; i < size; i++) {
    bitset1[i] &= bitset2[i];
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
    int capacity = vec->capacity * 2;
    if (capacity < 8) {
      capacity = 8;
    }
    int *data = calloc(capacity, sizeof(int));
    if (vec->data && (vec->length > 0)) {
      memcpy(data, vec->data, vec->length);
    }
    if (vec->data) {
      free(vec->data);
    }
    vec->data = data;
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
  for (int i = 0; i < bb_count; i++) {
    bbs[i]->ins_set = bitset_new(bb_count);
  }
  for (int i = 0; i < bb_count; i++) {
    BasicBlock *bb = bbs[i];
    for (int j = 0; j < bb->outs_count; j++) {
      BasicBlock *bb2 = bb->outs[j];
      bitset_set(bb2->ins_set, i, true);
    }
  }
}

static void compute_cfg_dominators(BasicBlock **bbs, int bb_count) {
  // Compute dominator set for each BB.
  // Algorithm based on pseudocode at:
  // https://sbaziotis.com/compilers/visualizing-dominators.html

  // First initialize dominator sets.
  // The entry point only ever has itself as a dominator.
  for (int i = 0; i < bb_count; i++) {
    bbs[i]->dominators_set = bitset_new(bb_count);
    if (i == 0) {
      bitset_fill(bbs[i]->dominators_set, false, bb_count);
      bitset_set(bbs[i]->dominators_set, 0, true);
    } else {
      bitset_fill(bbs[i]->dominators_set, true, bb_count);
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
      bitset_fill(tmp, true, bb_count);
      for (int j = 0; j < bb_count; j++) {
        if (bitset_get(bbs[i]->ins_set, j)) {
          bitset_intersect(tmp, bbs[j]->dominators_set, bb_count);
        }
      }
      bitset_set(tmp, i, true);
      if (!is_bitset_equal(bbs[i]->dominators_set, tmp, bb_count)) {
        change = true;
        bitset_copy(bbs[i]->dominators_set, tmp, bb_count);
      }
    }
    if (!change) {
      break;
    }
  }
  bitset_free(tmp);
}

static void _dfs_backward_edges(uint8_t *visited, uint8_t *visiting, BasicBlock *bb) {
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

static void _dfs_topological_sort(
  BasicBlock **bbs_out, int *bbs_out_count, uint8_t *visited, BasicBlock *bb
) {
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

static void topological_sort(
  BasicBlock **bbs_out, int *bbs_out_count, BasicBlock **bbs, int bb_count
) {
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

static void dump_bbs(BasicBlock **bbs, int bb_count) {
  for (int i = 0; i < bb_count; i++) {
    BasicBlock *bb = bbs[i];
    fprintf(stderr, "bb_%d: outs=", bb->idx);
    if (bb->outs_count == 1) {
      fprintf(stderr, "[bb_%d] ", bb->outs[0]->idx);
    }
    else if (bb->outs_count == 2) {
      fprintf(stderr, "[bb_%d, bb_%d] ", bb->outs[0]->idx, bb->outs[1]->idx);
    }
    else {
      fprintf(stderr, "[] ");
    }
    int ins_count = 0;
    if (bb->ins_set) {
      ins_count = bitset_popcnt(bb->ins_set, bb_count);
    }
    int doms_count = 0;
    if (bb->dominators_set) {
      doms_count = bitset_popcnt(bb->dominators_set, bb_count);
    }
    fprintf(
      stderr, "ins=%d doms=%d is_loop_header=%d sorted_idx=%d\n",
      ins_count, doms_count, (int) bb->is_loop_header, bb->sorted_idx
    );
  }
}

static int qsort_cmp_scope(const void *p1, const void *p2) {
  // Comparison callback for qsort.
  // Sort in descending order of 'end' position.
  StackifierScope *scope1 = *(StackifierScope**)p1;
  StackifierScope *scope2 = *(StackifierScope**)p2;
  if (scope1->end < scope2->end) {
    return 1;
  }
  else if (scope1->end > scope2->end) {
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
  BasicBlock **bbs = calloc(bb_count, sizeof(BasicBlock*));
  for (BasicBlock *bb = bb_list_head; bb; bb = bb->next) {
    bbs[bb->idx] = bb;
  }

  compute_cfg_ins(bbs, bb_count);
  compute_cfg_dominators(bbs, bb_count);
  compute_cfg_backward_edges(bbs, bb_count);
  bool is_reducible = compute_cfg_is_reducible(bbs, bb_count);

  if (!is_reducible) {
    fprintf(stderr, "warning: stackifier: CFG is not reducible!\n");
  }

  fprintf(stderr, "stackifier: bbs unsorted:\n");
  dump_bbs(bbs, bb_count);

  // Topological sort.
  // Note that unreachable nodes will be left out of the sorted set.
  BasicBlock **bbs_sorted = calloc(bb_count, sizeof(BasicBlock*));
  int bb_sorted_count = 0;
  topological_sort(bbs_sorted, &bb_sorted_count, bbs, bb_count);

  // Move all BBs that are part of a loop to just beneath their loop header
  // in the topologically sorted BB list.
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
      if (!bb_is_dominated_by(bb2, bb)) {
        continue;
      }
      // bb2 is part of loop. Need to move bb2 to bbs[k].
      // However first need to move bbs[k:j] to bbs[k + 1 : j + 1] to free
      // the slot at bbs[k].
      if ((j - k) > 0) {
        memmove(&bbs_sorted[k + 1], &bbs_sorted[k], sizeof(BasicBlock*) * (j - k));
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

  fprintf(stderr, "stackifier: bbs sorted:\n");
  dump_bbs(bbs_sorted, bb_sorted_count);

  // Enclose each loop in a loop scope (while(1) { /*...*/ }).
  // All back edges become 'continue' statements.

  // Now we are left with the forward edges. If the source and destination blocks are 
  // consecutive in the topological order, we don’t need to do anything.

  // Otherwise, we need to place a block scope (do{/*...*/} while(0)), such that the
  // destination block is just after the end of the scope.

  // Note that only one loop can start on a node, but multiple loops can end on a node.
  // Multiple block scopes can start on a node, but only one block scope can end on a node.

  StackifierScope *scopes_head = NULL;
  StackifierScope *scopes_tail = NULL;
  
  // Find loop scopes.
  IntVector *loop_stack = intvector_new();
  for (int i = 0; i < bb_sorted_count; i++) {
    BasicBlock *bb = bbs_sorted[i];
    // Check for end of loop.
    while (loop_stack->length > 0) {
      int loop_header_idx = intvector_get(loop_stack, loop_stack->length - 1);
      BasicBlock *loop_header_bb = bbs_sorted[loop_header_idx];
      if (bb_is_dominated_by(bb, loop_header_bb)) {
        // Still in loop.
        break;
      }
      // This is the first block that is not part of the loop.
      fprintf(stderr, "add loop scope: start=%d end=%d\n", loop_header_idx, i);
      StackifierScope *scope = calloc(1, sizeof(StackifierScope));
      scope->kind = SCOPE_LOOP; 
      scope->start = loop_header_idx;
      scope->end = i;
      if (scopes_head) {
        scopes_tail->next = scope;
      } else {
        scopes_head = scope;
      }
      scopes_tail = scope;
      intvector_pop(loop_stack);
    }

    // Check for start of loop.
    if (bb->is_loop_header) {
      intvector_push(loop_stack, i);
    }
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
        fprintf(stderr, "block first break: break=%d end=%d\n", i, block_end);
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
      for (StackifierScope *scope = scopes_head; scope; scope = scope->next) {
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
    fprintf(stderr, "add block scope: start=%d end=%d\n", start, end);
    StackifierScope *scope = calloc(1, sizeof(StackifierScope));
    scope->kind = SCOPE_BLOCK; 
    scope->start = start;
    scope->end = end;
    if (scopes_head) {
      scopes_tail->next = scope;
    } else {
      scopes_head = scope;
    }
    scopes_tail = scope;
  }

  free(block_starts);
  free(block_first_breaks);

  // Sort scopes in descending order of end position.
  // We want to enter scopes that end later first.
  int scope_count = 0;
  for (StackifierScope *scope = scopes_head; scope; scope = scope->next) {
    scope_count += 1;
  }
  StackifierScope **scopes = calloc(scope_count, sizeof(StackifierScope*));
  int scope_index = 0;
  for (StackifierScope *scope = scopes_head; scope; scope = scope->next) {
    scopes[scope_index++] = scope;
  }
  qsort(scopes, scope_count, sizeof(StackifierScope*), qsort_cmp_scope);

  // Output WASM.
  // int depth = 0;
  IntVector *scope_stack = intvector_new();
  for (int i = 0; i < bb_sorted_count; i++) {
    while (scope_stack->length > 0) {
      int scope_index = intvector_get(scope_stack, scope_stack->length - 1);
      StackifierScope *scope = scopes[scope_index];
      if (scope->end != i) {
        break;
      }
      // depth -= 1;
      put_wasm("  )\n");
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
      // depth += 1;
      intvector_push(scope_stack, scope_index);
    }

    BasicBlock *bb = bbs_sorted[i];
    fmt_wasm("    ;; bb_%d\n", bb->idx);
    for (Insn *insn = bb->insns_head; insn; insn = insn->next) {
      if (insn->next == NULL) {
        // Terminator instruction.
        if (bb->outs_count == 0) {
          fmt_wasm("    %s\n", insn->op);
          continue;
        }
        // For unconditional branches skip if this is an unconditional
        // branch to the next BB.
        if (bb->outs_count == 1) {
          if (
            bb->forward_outs_count == 1 &&
            bb->forward_outs[0]->sorted_idx == (i + 1)
          ) {
            continue;
          }
          fmt_wasm("    %s\n", insn->op);
          continue;
        }
        // For conditional branches convert the intermediate 'br_if' with
        // 'then' and 'else' dests to a 'br_if' and 'br'.
        // Eliminate one or the other if possible.
        BasicBlock* bb_then = bb->outs[0];
        bool bb_then_is_next = (bb_then->sorted_idx == (i + 1));
        BasicBlock* bb_else = bb->outs[1];
        bool bb_else_is_next = (bb_else->sorted_idx == (i + 1));
        if (bb_else_is_next) {
          // Simple case, output br_if, fallthrough to next BB.
          fmt_wasm("    br_if $bb_%d\n", bb_then->idx);
        }
        else if (bb_then_is_next) {
          // If the 'then' dest is the fallthrough need to reverse the
          // condition and br_if to the 'else' dest.
          put_wasm("    i32.eqz\n");
          fmt_wasm("    br_if $bb_%d\n", bb_else->idx);
        }
        else {
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

  // Cleanup.
  for (int i = 0; i < bb_count; i++) {
    bitset_free(bbs[i]->ins_set);
    bitset_free(bbs[i]->dominators_set);
  }
  StackifierScope *scope = scopes_head;
  while (scope) {
    StackifierScope *scope_next = scope->next;
    free(scope);
    scope = scope_next;
  }
  free(scopes);
  free(bbs_sorted);
  free(bbs);
}

#define WASM_DATA_START 1024
#define WASM_STACK_SIZE 65536

static void emit_gvars(Obj *prog) {
  int data_offset = WASM_DATA_START;

  // First calculate offsets and indexes of global vars.
  for (Obj *var = prog; var; var = var->next) {
    if (var->is_function) {
      continue;
    }
    if (!var->is_definition) {
      continue;
    }

    data_offset = align_to(data_offset, var->ty->align);
    var->offset = data_offset;

    data_offset += var->ty->size;
  }

  // Do we need this? Should have one linear memory by default.
  put_wasm("(memory 1)\n");

  // Stack pointer is always the first global.
  // Stack starts after global variables.
  data_offset = align_to(data_offset, 4);
  fmt_wasm("(global $__stack_pointer (mut i32) (i32.const %d))\n", data_offset + WASM_STACK_SIZE);

  // Add '(global ...)' definitions.
  for (Obj *var = prog; var; var = var->next) {
    if (var->is_function) {
      continue;
    }
  
    // For global vars is_definition is false if the var has the 'extern' attribute.
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

    fmt_wasm("(global $%s i32 (i32.const %d))\n", var->name, var->offset);
  }

  // Add '(data ...)' memory initialization data for global vars with initializers.
  for (Obj *var = prog; var; var = var->next) {
    if (var->is_function || !var->is_definition || !var->init_data) {
      continue;
    }

    fmt_wasm("(data (i32.const %d) \"", var->offset);
    int pos = 0;
    Relocation *rel = var->rel;
    while (pos < var->ty->size) {
      if (rel && rel->offset == pos) {
        uint32_t rel_var_offset = rel->var->offset;
        fmt_wasm(
          "\\%02x\\%02x\\%02x\\%02x",
          (rel_var_offset >> 0) & 0xff,
          (rel_var_offset >> 8) & 0xff,
          (rel_var_offset >> 16) & 0xff,
          (rel_var_offset >> 24) & 0xff
        );
        pos += 4;
      } else {
        fmt_wasm("\\%02x", var->init_data[pos]);
        pos += 1;
      }
    }
    put_wasm("\")\n");
  }
}

static void emit_funcs(Obj *prog) {
  for (Obj *fn = prog; fn; fn = fn->next) {
    if (!fn->is_function) {
      continue;
    }
  
    // No code is emitted for "static" functions if no one is referencing them.
    if (!fn->is_live) {
      continue;
    }

    if (!fn->is_definition) {
      // TODO : imported functions.
      continue;
    }

    if (!fn->is_static) {
      // TODO : add export if not static.
    }

    current_function = fn;
    current_function_local_counter = 0;
    current_function_bb_counter = 0;
    current_function_bbs_head = NULL;
    current_function_bbs_tail = NULL;

    BasicBlock *entry_bb = append_new_bb();
    current_bb = entry_bb;

    current_function_return_bb = append_new_bb();

    int frame_offset = 0;

    Type *return_ty = fn->ty->return_ty;
    bool returns_struct = (return_ty->kind == TY_STRUCT || return_ty->kind == TY_UNION);

    // Count params.
    int param_count = 0;
    for (Obj *var = fn->params; var; var = var->next) {
      param_count += 1;
    }

    // Create stack space and assign local indexes for all locals (including named local vars,
    // function params, and anon locals created by the parser).
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
    
    current_function_local_counter += 1;
    if ((return_ty->kind != TY_VOID) && !returns_struct) {
      // put_insn("local.set $result 0");
      current_function_local_counter += 1;
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

    // Emit function body.
    gen_stmt(fn->body);

    BasicBlock *last_bb = current_bb;

    // Add branches to the end of goto basic blocks now that the basic blocks for
    // all labels have been created.
    for (Node *goto_nd = fn->gotos; goto_nd; goto_nd = goto_nd->goto_next) {
      for (Node *label_nd = fn->labels; label_nd; label_nd = label_nd->goto_next) {
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
    // TODO : remove the current BB rather than adding a return if it is unreachable.
    if (!is_current_bb_terminated()) {
      if (strcmp(fn->name, "main") == 0) {
        // [https://www.sigbus.info/n1570#5.1.2.2.3p1] The C spec defines
        // a special rule for the main function. Reaching the end of the
        // main function is equivalent to returning 0, even though the
        // behavior is undefined for the other functions.
        put_insn("i32.const 0");
      }
      else if (fn->is_noreturn) {
        put_insn_unreachable();
      }
      else if (return_ty->kind == TY_VOID) {
        // return nothing.
      }
      else if (returns_struct) {
        // return nothing.
      }
      else {
        // Return uninitialized value.
        // TODO : ???
      }
      put_insn_br(current_function_return_bb);
    }

    // Emit function epilogue.
    set_current_bb(current_function_return_bb);
    put_insn("local.get $fp");
    fmt_insn("i32.const %d", frame_offset);
    put_insn("i32.add");
    put_insn("global.set $__stack_pointer");
    if ((return_ty->kind != TY_VOID) && !returns_struct) {
      put_insn("local.get $result");
    }
    put_insn_return();

    fmt_wasm("(func $%s\n", fn->name);
    param_count = 0;
    for (Obj *var = fn->params; var; var = var->next) {
      fmt_wasm("  (param $p%d %s)\n", param_count, get_wasm_type(var->ty));
      param_count += 1;
    }
    if (returns_struct) {
      fmt_wasm("  (param $p%d i32)\n", param_count);
    }
    else if (return_ty->kind != TY_VOID) {
      fmt_wasm("  (result %s)\n", get_wasm_type(return_ty));
    }

    int local_index = 0;
    put_wasm("  (local $fp i32)\n");
    local_index += 1;
    if ((return_ty->kind != TY_VOID) && !returns_struct) {
      fmt_wasm("  (local $result %s)\n", get_wasm_type(return_ty));
      local_index += 1;
    }
    for (; local_index < current_function_local_counter; local_index++) {
      fmt_wasm("  (local (;%d;) i32)\n", param_count + local_index);
    }

    stackify(current_function_bbs_head);

    put_wasm(")\n");

    current_function = NULL;
    current_function_local_counter = 0;
    current_function_bb_counter = 0;
    current_function_bbs_head = NULL;
    current_function_bbs_tail = NULL;
    current_function_return_bb = NULL;
  }
}

void codegen(Obj *prog, FILE *out) {
  current_out_file = out;
  put_wasm("(module $module0\n");
  emit_gvars(prog);
  emit_funcs(prog);
  put_wasm(")\n");
  current_out_file = NULL;
}
