#include "chibicc.h"

#define SEG_ALIGN 4096

typedef enum {
  LABEL_CODE,
  LABEL_DATA,
} LabelKind;

typedef struct LabelRef {
  struct LabelRef *next;
  LabelKind kind;
  int offset;
  int vaddr;
} LabelRef;

typedef struct Label {
  struct Label *next;
  LabelKind kind;
  int offset;
  int vaddr;
  struct LabelRef *refs;
} Label;

static ByteArray *current_out = NULL;
static int current_offset = 0;
static int current_vaddr = 0;
static Obj *current_fn = NULL;

static Label *current_return_label = NULL;
static Label *current_break_label = NULL;
static Label *current_continue_label = NULL;

static Label *labels = NULL;

static void gen_expr(Node *node);
static void _gen_stmt(Node *node, bool should_drop_result);
static void gen_stmt(Node *node);

static Label *new_label(LabelKind kind) {
  Label *label = calloc(1, sizeof(Label));
  label->kind = kind;
  if (labels) {
    label->next = labels;
  }
  labels = label;
  return label;
}

static void label_set_dest(Label *label) {
  label->offset = current_offset;
  label->vaddr = current_vaddr;
}

static LabelRef *label_add_ref(Label *label, LabelKind ref_kind) {
  LabelRef *ref = calloc(1, sizeof(LabelRef));
  ref->kind = ref_kind;
  ref->offset = current_offset;
  ref->vaddr = current_vaddr;
  if (label->refs) {
    ref->next = label->refs;
  }
  label->refs = ref;
  return ref;
}

static void emit_bytes(char *data, int n) {
  bytearray_extend(current_out, (uint8_t*)data, n);
  current_offset += n;
  current_vaddr += n;
}

static void emit_u8(uint8_t val) {
  bytearray_append(current_out, val);
  current_offset += 1;
  current_vaddr += 1;
}

static void emit_i16(int16_t val) {
  emit_u8((val >> 0) & 0xff);
  emit_u8((val >> 8) & 0xff);
}

static void emit_i32(int32_t val) {
  emit_u8((val >> 0) & 0xff);
  emit_u8((val >> 8) & 0xff);
  emit_u8((val >> 16) & 0xff);
  emit_u8((val >> 24) & 0xff);
}

static void emit_i64(int64_t val) {
  emit_u8((val >> 0) & 0xff);
  emit_u8((val >> 8) & 0xff);
  emit_u8((val >> 16) & 0xff);
  emit_u8((val >> 24) & 0xff);
  emit_u8((val >> 32) & 0xff);
  emit_u8((val >> 40) & 0xff);
  emit_u8((val >> 48) & 0xff);
  emit_u8((val >> 56) & 0xff);
}

static void patch_i32(int offset, int32_t val) {
  uint8_t *buf = current_out->data;
  buf[offset + 0] = (val >> 0) & 0xff;
  buf[offset + 1] = (val >> 8) & 0xff;
  buf[offset + 2] = (val >> 16) & 0xff;
  buf[offset + 3] = (val >> 24) & 0xff;
}

static void patch_i64(int offset, int64_t val) {
  uint8_t *buf = current_out->data;
  buf[offset + 0] = (val >> 0) & 0xff;
  buf[offset + 1] = (val >> 8) & 0xff;
  buf[offset + 2] = (val >> 16) & 0xff;
  buf[offset + 3] = (val >> 24) & 0xff;
  buf[offset + 4] = (val >> 32) & 0xff;
  buf[offset + 5] = (val >> 40) & 0xff;
  buf[offset + 6] = (val >> 48) & 0xff;
  buf[offset + 7] = (val >> 56) & 0xff;
}

#define DEF_EMIT_INSN(name, opcode) \
  static void name() { \
    emit_u8(opcode); \
  }

DEF_EMIT_INSN(nop, BC_NOP);
DEF_EMIT_INSN(emit_push_r0, BC_PUSH_R0);
DEF_EMIT_INSN(emit_push_r1, BC_PUSH_R1);
DEF_EMIT_INSN(emit_pop_r0, BC_POP_R0);
DEF_EMIT_INSN(emit_pop_r1, BC_POP_R1);
DEF_EMIT_INSN(emit_push_fp, BC_PUSH_FP);
DEF_EMIT_INSN(emit_mov_r0_r1, BC_MOV_R0_R1);
DEF_EMIT_INSN(emit_mov_r1_r0, BC_MOV_R1_R0);
DEF_EMIT_INSN(emit_add, BC_ADD);
DEF_EMIT_INSN(emit_sub, BC_SUB);
DEF_EMIT_INSN(emit_mul, BC_MUL);
DEF_EMIT_INSN(emit_div_u, BC_DIV_U);
DEF_EMIT_INSN(emit_div_s, BC_DIV_S);
DEF_EMIT_INSN(emit_rem_u, BC_REM_U);
DEF_EMIT_INSN(emit_rem_s, BC_REM_S);
DEF_EMIT_INSN(emit_and, BC_AND);
DEF_EMIT_INSN(emit_or, BC_OR);
DEF_EMIT_INSN(emit_xor, BC_XOR);
DEF_EMIT_INSN(emit_shl, BC_SHL);
DEF_EMIT_INSN(emit_shr_u, BC_SHR_U);
DEF_EMIT_INSN(emit_shr_s, BC_SHR_S);
DEF_EMIT_INSN(emit_not, BC_NOT);
DEF_EMIT_INSN(emit_neg, BC_NEG);
DEF_EMIT_INSN(emit_trunc_8, BC_TRUNC_8);
DEF_EMIT_INSN(emit_trunc_16, BC_TRUNC_16);
DEF_EMIT_INSN(emit_trunc_32, BC_TRUNC_32);
DEF_EMIT_INSN(emit_sext_8, BC_SEXT_8);
DEF_EMIT_INSN(emit_sext_16, BC_SEXT_16);
DEF_EMIT_INSN(emit_sext_32, BC_SEXT_32);
DEF_EMIT_INSN(emit_eq_zero, BC_EQ_ZERO);
DEF_EMIT_INSN(emit_eq, BC_EQ);
DEF_EMIT_INSN(emit_ne, BC_NE);
DEF_EMIT_INSN(emit_lt_u, BC_LT_U);
DEF_EMIT_INSN(emit_lt_s, BC_LT_S);
DEF_EMIT_INSN(emit_le_u, BC_LE_U);
DEF_EMIT_INSN(emit_le_s, BC_LE_S);
DEF_EMIT_INSN(emit_load_u64, BC_LOAD_U64);
DEF_EMIT_INSN(emit_load_u32, BC_LOAD_U32);
DEF_EMIT_INSN(emit_load_u16, BC_LOAD_U16);
DEF_EMIT_INSN(emit_load_u8, BC_LOAD_U8);
DEF_EMIT_INSN(emit_store_u64, BC_STORE_U64);
DEF_EMIT_INSN(emit_store_u32, BC_STORE_U32);
DEF_EMIT_INSN(emit_store_u16, BC_STORE_U16);
DEF_EMIT_INSN(emit_store_u8, BC_STORE_U8);
DEF_EMIT_INSN(emit_call, BC_CALL);
DEF_EMIT_INSN(emit_syscall6, BC_SYSCALL6);
DEF_EMIT_INSN(emit_leave, BC_LEAVE);
DEF_EMIT_INSN(emit_memset, BC_MEMSET);
DEF_EMIT_INSN(emit_memcpy, BC_MEMCPY);

static void emit_mov_r0_imm(int64_t val) {
  emit_u8(BC_MOV_R0_IMM);
  emit_i64(val);
}

static void emit_mov_r1_imm(int64_t val) {
  emit_u8(BC_MOV_R1_IMM);
  emit_i64(val);
}

static void emit_jmp_rel32(int dst) {
  emit_u8(BC_JMP);
  emit_i32(dst - (current_vaddr + 4));
}

static void emit_jz_rel32(int dst) {
  emit_u8(BC_JZ);
  emit_i32(dst - (current_vaddr + 4));
}

static void emit_jnz_rel32(int dst) {
  emit_u8(BC_JNZ);
  emit_i32(dst - (current_vaddr + 4));
}

static void emit_jmp(Label *label) {
  emit_jmp_rel32(0);
  label_add_ref(label, LABEL_CODE);
}

static void emit_jz(Label *label) {
  emit_jz_rel32(0);
  label_add_ref(label, LABEL_CODE);
}

static void emit_jnz(Label *label) {
  emit_jnz_rel32(0);
  label_add_ref(label, LABEL_CODE);
}

static void emit_lea_pc_rel(Label *label) {
  emit_u8(BC_LEA_PC_REL);
  emit_i32(0);
  label_add_ref(label, LABEL_CODE);
}

static void emit_lea_fp_rel(int offset) {
  emit_u8(BC_LEA_FP_REL);
  emit_i32(offset);
}

static void emit_pick(int offset) {
  emit_u8(BC_PICK);
  emit_i32(offset);
}

static void emit_enter(int frame_size) {
  emit_u8(BC_ENTER);
  emit_i32(frame_size);
}

// Round up `n` to the nearest multiple of `align`. For instance,
// align_to(5, 8) returns 8 and align_to(11, 8) returns 16.
int align_to(int n, int align) { return (n + align - 1) / align * align; }

static void gen_is_eq_zero(Type *ty) {
  switch (ty->kind) {
  case TY_BOOL:
  case TY_CHAR:
  case TY_SHORT:
  case TY_INT:
  case TY_ENUM:
  case TY_LONG:
  case TY_LONGLONG:
  case TY_PTR:
  case TY_FUNC:
  case TY_ARRAY:
  case TY_VLA:
    emit_eq_zero();
    break;
  default:
    error("cmp_zero: bad type %d", ty->kind);
    break;
  }
}

static void gen_is_ne_zero(Type *ty) {
  gen_is_eq_zero(ty);
  emit_eq_zero();
}

static void gen_lvar_addr(Obj *var) {
  emit_lea_fp_rel(var->offset);
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
      emit_load_u64();
      return;
    }
    // For local variables return the stack address of the var.
    if (node->var->is_local) {
      gen_lvar_addr(node->var);
      return;
    }
    // For functions and global vars return the absolute address.
    emit_lea_pc_rel(node->var->label);
    return;
  case ND_DEREF:
    gen_expr(node->lhs);
    return;
  case ND_COMMA:
    gen_expr(node->lhs);
    gen_addr(node->rhs);
    return;
  case ND_MEMBER:
    gen_addr(node->lhs);
    emit_mov_r1_imm(node->member->offset);
    emit_add();
    return;
  case ND_FUNCALL:
    // If the callee returns a struct, it is output to the hidden local
    // var 'ret_buffer' in the calling function's scope.
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
  case TY_DOUBLE:
    error("cannot load type");
    return;
  default:
    break;
  }

  if (ty->size == 1 && ty->is_unsigned) {
    emit_load_u8();
  } else if (ty->size == 8) {
    emit_load_u64();
  } else {
    error("cannot load type");
  }
}

// Store r0 to address pointed to by r1.
static void store(Type *ty) {
  switch (ty->kind) {
  case TY_STRUCT:
  case TY_UNION:
    // [dst src n] -> []
    emit_push_r1();
    emit_push_r0();
    emit_mov_r0_imm(ty->size);
    emit_push_r0();
    emit_memcpy();
    return;
  case TY_FLOAT:
  case TY_DOUBLE:
    error("cannot store type");
    return;
  default:
    break;
  }

  if (ty->size == 1) {
    emit_store_u8();
  } else if (ty->size == 8) {
    emit_store_u64();
  } else {
    error("cannot store type");
  }
}

static void cast(Type *from, Type *to) {
  if (to->kind == TY_VOID) {
    return;
  }

  if (to->kind == TY_BOOL) {
    gen_is_ne_zero(from);
    return;
  }

  if (from->kind == TY_FLOAT || from->kind == TY_DOUBLE) {
    error("casts from floating point types unsupported");
  }
  if (to->kind == TY_FLOAT || to->kind == TY_DOUBLE) {
    error("casts to floating point types unsupported");
  }

  int from_size = from->size;
  switch (from->kind) {
  case TY_BOOL:
  case TY_CHAR:
  case TY_SHORT:
  case TY_INT:
  case TY_ENUM:
  case TY_LONG:
  case TY_LONGLONG:
    from_size = from->size;
    break;
  default:
    from_size = PTR_SIZE;
    break;
  }

  int to_size;
  switch (to->kind) {
  case TY_BOOL:
  case TY_CHAR:
  case TY_SHORT:
  case TY_INT:
  case TY_ENUM:
  case TY_LONG:
  case TY_LONGLONG:
    to_size = to->size;
    break;
  default:
    to_size = PTR_SIZE;
    break;
  }

  // Truncate to target size first.
  switch (to_size) {
  case 1:
    emit_trunc_8();
    break;
  case 2:
    emit_trunc_16();
    break;
  case 4:
    emit_trunc_32();
    break;
  default:
    break;
  }

  // Sign extend if target is a signed type.
  if (!to->is_unsigned) {
    switch (to_size) {
    case 1:
      emit_sext_8();
      break;
    case 2:
      emit_sext_16();
      break;
    case 4:
      emit_sext_32();
      break;
    default:
      break;
    }
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
    case TY_DOUBLE:
      error_tok(node->tok, "float literals unsupported");
      return;
    default:
      // TODO : is this correct? do we need to cast node->val at compile time?
      emit_mov_r0_imm(node->val);
      break;
    }
    return;
  }
  case ND_NEG: {
    switch (node->ty->kind) {
    case TY_FLOAT:
    case TY_DOUBLE:
      error_tok(node->tok, "float negation unsupported");
      return;
    default:
      gen_expr(node->lhs);
      emit_neg();
    }
    return;
  }
  case ND_VAR:
    gen_addr(node);
    load(node->ty);
    return;
  case ND_MEMBER: {
    Member *mem = node->member;
    if (mem->is_bitfield) {
      error_tok(node->tok, "bitfields unsupported");
      return;
    }
    gen_addr(node);
    load(node->ty);
    return;
  }
  case ND_DEREF:
    gen_expr(node->lhs);
    load(node->ty);
    return;
  case ND_ADDR:
    gen_addr(node->lhs);
    return;
  case ND_ASSIGN: {
    if (node->lhs->kind == ND_MEMBER && node->lhs->member->is_bitfield) {
      error_tok(node->tok, "bitfields unsupported");
      return;
    }
    gen_addr(node->lhs);
    emit_push_r0();
    gen_expr(node->rhs);
    emit_pop_r1();
    store(node->ty);
    return;
  }
  case ND_STMT_EXPR:
    error_tok(node->tok, "statement expressions unsupported");
    return;
  case ND_COMMA:
    gen_expr(node->lhs);
    gen_expr(node->rhs);
    return;
  case ND_CAST:
    gen_expr(node->lhs);
    cast(node->lhs->ty, node->ty);
    return;
  case ND_MEMZERO:
    // Node is always a local var.
    gen_lvar_addr(node->var);
    if (node->var->ty->size == 8) {
      emit_mov_r1_r0();
      emit_mov_r0_imm(0);
      emit_store_u64();
    } else {
      emit_push_r0();
      emit_mov_r0_imm(0);
      emit_push_r0();
      emit_mov_r0_imm(node->var->ty->size);
      emit_push_r0();
      emit_memset();
    }
    return;
  case ND_COND: {
    // Ternary expression.
    Label *else_label = new_label(LABEL_CODE);
    Label *end_label = new_label(LABEL_CODE);
    gen_expr(node->cond);
    gen_is_eq_zero(node->cond->ty);
    emit_jz(else_label);
    gen_expr(node->then);
    emit_jmp(end_label);
    label_set_dest(else_label);
    gen_expr(node->els);
    label_set_dest(end_label);
    return;
  }
  case ND_NOT:
    gen_expr(node->lhs);
    gen_is_eq_zero(node->lhs->ty);
    return;
  case ND_BITNOT:
    gen_expr(node->lhs);
    emit_not();
    return;
  case ND_LOGAND: {
    // Logical AND with short-circuit. Skip the RHS expression if the LHS is
    // false.
    Label *end_label = new_label(LABEL_CODE);
    gen_expr(node->lhs);
    gen_is_ne_zero(node->lhs->ty);
    emit_jz(end_label);
    gen_expr(node->rhs);
    gen_is_ne_zero(node->rhs->ty);
    label_set_dest(end_label);
    return;
  }
  case ND_LOGOR: {
    // Logical OR with short-circuit. Skip the RHS expression if the LHS is
    // true.
    Label *end_label = new_label(LABEL_CODE);
    gen_expr(node->lhs);
    gen_is_ne_zero(node->lhs->ty);
    emit_jnz(end_label);
    gen_expr(node->rhs);
    gen_is_ne_zero(node->rhs->ty);
    label_set_dest(end_label);
    return;
  }
  case ND_FUNCALL: {
    if (node->lhs->kind == ND_VAR) {
      if (strcmp(node->lhs->var->name, "alloca") == 0) {
        error_tok(node->tok, "alloca builtin not supported");
        return;
      }
      else if (strcmp(node->lhs->var->name, "__builtin_syscall3") == 0) {
        gen_expr(node->args);
        emit_push_r0();
        gen_expr(node->args->next);
        emit_push_r0();
        gen_expr(node->args->next->next);
        emit_push_r0();
        gen_expr(node->args->next->next->next);
        emit_push_r0();
        emit_mov_r0_imm(0);
        emit_push_r0();
        emit_push_r0();
        emit_push_r0();
        emit_syscall6();
        return;
      } else if (strcmp(node->lhs->var->name, "__builtin_memory_fill") == 0) {
        gen_expr(node->args);
        emit_push_r0();
        gen_expr(node->args->next);
        emit_push_r0();
        gen_expr(node->args->next->next);
        emit_push_r0();
        emit_memset();
        return;
      } else if (strcmp(node->lhs->var->name, "__builtin_memory_copy") == 0) {
        gen_expr(node->args);
        emit_push_r0();
        gen_expr(node->args->next);
        emit_push_r0();
        gen_expr(node->args->next->next);
        emit_push_r0();
        emit_memcpy();
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
      emit_push_r0();
    }

    // Push each non-variadic arg onto the stack in left to right order.
    for (Node *arg = node->args; arg; arg = arg->next) {
      if (arg->ty->size > 8) {
        error_tok(arg->tok, "args larger than 8 bytes unsupported");
      }
      gen_expr(arg);
      emit_push_r0();
    }

    // Collect remaining variadic args into the 'va_arg_area' local for this
    // call-site.
    if (node->va_arg_area) {
      error_tok(node->tok, "variadic function calls unsupported");
      // int offset = 0;
      // for (; arg; arg = arg->next) {
      //   offset = align_to(offset, arg->ty->align);
      //   gen_lvar_addr(node->va_arg_area);
      //   fmt_insn("i32.const %d", offset);
      //   put_insn("i32.add");
      //   gen_expr(arg);
      //   store(arg->ty);
      //   offset += arg->ty->size;
      // }
      // gen_lvar_addr(node->va_arg_area);
    }

    // Call the function.
    gen_expr(node->lhs);
    emit_call();

    // If the function returns a struct get the address of the local var it is
    // stored in.
    if (returns_struct) {
      gen_lvar_addr(node->ret_buffer);
    }

    return;
  }
  case ND_LABEL_VAL:
    error_tok(node->tok, "label-as-value not supported");
    return;
  case ND_CAS:
    error_tok(node->tok, "atomic compare and swap builtin not supported");
    return;
  case ND_EXCH:
    error_tok(node->tok, "atomic exchange builtin not supported");
    return;
  default:
    break;
  }

  // All remaining node types should be simple binary expressions.
  if (node->lhs == NULL) {
    error_tok(node->tok, "unhandled expression: kind=%d", node->kind);
  }

  switch (node->lhs->ty->kind) {
  case TY_FLOAT:
  case TY_DOUBLE:
    error_tok(node->tok, "invalid floating point expression");
    return;
  case TY_PTR:
  case TY_FUNC:
  case TY_ARRAY:
  case TY_VLA: {
    // LHS is pointer.
    // Parser always makes the pointer the LHS.

    gen_expr(node->lhs);
    emit_push_r0();
    gen_expr(node->rhs);
    emit_mov_r1_r0();
    emit_pop_r0();

    switch (node->kind) {
    case ND_ADD:
      switch (node->rhs->ty->kind) {
      case TY_LONG:
        // ptr + int.
        // RHS should have been cast to a pointer sized int by the
        // usual arithmetic conversions.
        emit_add();
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
        emit_sub();
        return;
      case TY_PTR:
      case TY_FUNC:
      case TY_ARRAY:
      case TY_VLA:
        // ptr - ptr.
        emit_sub();
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
        emit_eq();
        return;
      case ND_NE:
        emit_ne();
        return;
      case ND_LT:
        emit_lt_u();
        return;
      case ND_LE:
        emit_le_u();
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
  emit_push_r0();
  gen_expr(node->rhs);
  emit_mov_r1_r0();
  emit_pop_r0();

  switch (node->kind) {
  case ND_ADD:
    emit_add();
    return;
  case ND_SUB:
    emit_sub();
    return;
  case ND_MUL:
    emit_mul();
    return;
  case ND_DIV:
    if (node->ty->is_unsigned) {
      emit_div_u();
    } else {
      emit_div_s();
    }
    return;
  case ND_MOD:
    if (node->ty->is_unsigned) {
      emit_rem_u();
    } else {
      emit_rem_s();
    }
    return;
  case ND_BITAND:
    emit_and();
    return;
  case ND_BITOR:
    emit_or();
    return;
  case ND_BITXOR:
    emit_xor();
    return;
  case ND_EQ:
    emit_eq();
    return;
  case ND_NE:
    emit_ne();
    return;
  case ND_LT:
    if (node->lhs->ty->is_unsigned) {
      emit_lt_u();
    } else {
      emit_lt_s();
    }
    return;
  case ND_LE:
    if (node->lhs->ty->is_unsigned) {
      emit_le_u();
    } else {
      emit_le_s();
    }
    return;
  case ND_SHL:
    emit_shl();
    return;
  case ND_SHR:
    if (node->lhs->ty->is_unsigned) {
      emit_shr_u();
    } else {
      emit_shr_s();
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
    Label *else_label = new_label(LABEL_CODE);
    Label *end_label = new_label(LABEL_CODE);

    gen_expr(node->cond);
    gen_is_eq_zero(node->cond->ty);
    emit_jz(else_label);
    if (node->then) {
      gen_stmt(node->then);
    }
    emit_jmp(end_label);
    label_set_dest(else_label);
    if (node->els) {
      gen_stmt(node->els);
    }
    label_set_dest(end_label);
    return;
  }
  case ND_FOR: {
    Label *cond_label = new_label(LABEL_CODE);
    Label *inc_label = new_label(LABEL_CODE);
    Label *end_label = new_label(LABEL_CODE);

    if (node->init) {
      gen_stmt(node->init);
    }
    label_set_dest(cond_label);
    if (node->cond) {
      gen_expr(node->cond);
      gen_is_eq_zero(node->cond->ty);
      emit_jz(end_label);
    }
    if (node->then) {
      Label *prev_break_label = current_break_label;
      Label *prev_continue_label = current_continue_label;
      current_break_label = end_label;
      current_continue_label = inc_label;
      gen_stmt(node->then);
      current_break_label = prev_break_label;
      current_continue_label = prev_continue_label;
    }
    label_set_dest(inc_label);
    if (node->inc) {
      gen_expr(node->inc);
    }
    emit_jmp(cond_label);
    label_set_dest(end_label);
    return;
  }
  case ND_DO: {
    Label *start_label = new_label(LABEL_CODE);
    Label *cond_label = new_label(LABEL_CODE);
    Label *end_label = new_label(LABEL_CODE);

    label_set_dest(start_label);
    if (node->then) {
      Label *prev_break_label = current_break_label;
      Label *prev_continue_label = current_continue_label;
      current_break_label = end_label;
      current_continue_label = cond_label;
      gen_stmt(node->then);
      current_break_label = prev_break_label;
      current_continue_label = prev_continue_label;
    }
    label_set_dest(cond_label);
    gen_expr(node->cond);
    gen_is_eq_zero(node->cond->ty);
    emit_jnz(start_label);
    label_set_dest(end_label);
    return;
  }
  case ND_SWITCH: {
    // Switch condition value.
    gen_expr(node->cond);
    emit_mov_r1_r0();

    // Generate branches to case labels.
    for (Node *nd = node->case_next; nd; nd = nd->case_next) {
      nd->case_label = new_label(LABEL_CODE);
      if (nd->begin == nd->end) {
        // Normal single integer case.
        emit_mov_r0_imm(nd->begin);
        emit_eq();
      } else {
        // [GNU] case ranges.
        emit_push_r1();
        emit_mov_r0_r1();
        emit_mov_r1_imm(nd->begin);
        emit_sub();
        emit_mov_r1_imm(nd->end - nd->begin);
        emit_le_u();
        emit_pop_r1();
      }
      emit_jnz(nd->case_label);
    }

    Label *end_label = new_label(LABEL_CODE);

    if (node->default_case) {
      node->default_case->case_label = new_label(LABEL_CODE);
      emit_jmp(node->default_case->case_label);
    }

    Label *prev_break_label = current_break_label;
    current_break_label = end_label;

    // Generate code for switch body.
    // Includes case labels and all case bodies.
    gen_stmt(node->then);

    current_break_label = prev_break_label;

    label_set_dest(end_label);
    return;
  }
  case ND_CASE: {
    label_set_dest(node->case_label);
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
    if (current_break_label == NULL) {
      error_tok(node->tok, "cannot 'break' here");
    }
    emit_jmp(current_break_label);
    return;
  }
  case ND_CONTINUE: {
    if (current_continue_label == NULL) {
      error_tok(node->tok, "cannot 'continue' here");
    }
    emit_jmp(current_continue_label);
    return;
  }
  case ND_GOTO: {
    node->goto_label = new_label(LABEL_CODE);
    emit_jmp(node->goto_label);
    return;
  }
  case ND_GOTO_EXPR:
    error_tok(node->tok, "goto expr not supported");
    return;
  case ND_LABEL: {
    // node->goto_label should have been initialized in emit_funcs before code is emitted.
    label_set_dest(node->goto_label);
    gen_stmt(node->lhs);
    return;
  }
  case ND_RETURN: {
    if (node->lhs) {
      Type *ty = node->lhs->ty;
      bool returns_struct = (ty->kind == TY_STRUCT) || (ty->kind == TY_UNION);
      if (returns_struct) {
        // Structure returns are written to the buffer pointed to by the first
        // param (the hidden first param is inserted by the parser).
        Obj *ret_buffer_var = current_fn->params;
        gen_lvar_addr(ret_buffer_var);
        emit_push_r0();
        gen_addr(node->lhs);
        emit_push_r0();
        emit_mov_r0_imm(ty->size);
        emit_push_r0();
        emit_memcpy();
      } else {
        gen_expr(node->lhs);
      }
    }
    emit_jmp(current_return_label);
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

static Obj *find_defined_obj(Prog *prog, const char *name) {
  for (Obj *var = prog->obj; var; var = var->next) {
    if (!var->is_definition) {
      continue;
    }
    if (strcmp(name, var->name) == 0) {
      return var;
    }
  }
  return NULL;
}

static Obj *find_exported_obj(Prog *progs, const char *name) {
  for (Prog *prog = progs; prog; prog = prog->next) {
    for (Obj *var = prog->obj; var; var = var->next) {
      if (!var->is_definition || var->is_static) {
        continue;
      }
      if (strcmp(name, var->name) == 0) {
        return var;
      }
    }
  }
  return NULL;
}

static void resolve_names(Prog *progs) {
  // Create a label for each defined global var and function.
  for (Prog *prog = progs; prog; prog = prog->next) {
    for (Obj *var = prog->obj; var; var = var->next) {
      if (!var->is_definition || var->label != NULL) {
        continue;
      }
      if (var->is_function) {
        var->label = new_label(LABEL_CODE);
      } else {
        var->label = new_label(LABEL_DATA);
      }
    }
  }
  // Find definitions for remaining global vars and functions.
  for (Prog *prog = progs; prog; prog = prog->next) {
    for (Obj *var = prog->obj; var; var = var->next) {
      if (var->is_definition || var->label != NULL) {
        continue;
      }
      // First try to look up in current translation unit.
      Obj *var_def = find_defined_obj(prog, var->name);
      if (var_def == NULL) {
        var_def = find_exported_obj(progs, var->name);
      }
      if (var_def == NULL) {
        error_tok(var->tok, "unable to resolve");
      }
      var->label = var_def->label;
    }
  }
}

static void calculate_gvar_offsets(Prog *progs, int *data_filesz, int *data_memsz) {
  // Calculate offsets of global vars with initialization data.
  int offset = 0;
  for (Prog *prog = progs; prog; prog = prog->next) {
    for (Obj *var = prog->obj; var; var = var->next) {
      if (var->is_function || !var->is_definition || !var->init_data) {
        continue;
      }
      offset = align_to(offset, var->ty->align);
      var->offset = offset;
      offset += var->ty->size;
    }
  }
  offset = align_to(offset, SEG_ALIGN);
  *data_filesz = offset;
  // Then calculate offsets for global vars without initialization data.
  for (Prog *prog = progs; prog; prog = prog->next) {
    for (Obj *var = prog->obj; var; var = var->next) {
      if (var->is_function || !var->is_definition || var->init_data) {
        continue;
      }
      offset = align_to(offset, var->ty->align);
      var->offset = offset;
      offset += var->ty->size;
    }
  }
  offset = align_to(offset, SEG_ALIGN);
  *data_memsz = offset;
}

static void emit_funcs(Obj *prog_obj) {
  for (Obj *fn = prog_obj; fn; fn = fn->next) {
    if (!fn->is_function || !fn->is_live || !fn->is_definition) {
      continue;
    }
    current_fn = fn;
    current_return_label = new_label(LABEL_CODE);

    Type *return_ty = fn->ty->return_ty;
    bool returns_struct = (return_ty->kind == TY_STRUCT || return_ty->kind == TY_UNION);
    bool has_result = ((return_ty->kind != TY_VOID) && !returns_struct);

    // The last pushed param resides at fp+16.
    // Params are added left-to-right so this is the rightmost param.
    int top = 16;
    int bottom = 0;

    // Assign offsets to pass-by-stack parameters.
    for (Obj *var = fn->params; var; var = var->next) {
      top += align_to(var->ty->size, 8);
    }
    int param_offset = top;
    for (Obj *var = fn->params; var; var = var->next) {
      param_offset -= align_to(var->ty->size, 8);
      var->offset = param_offset;
    }

    // Create stack space and assign local offsets for all locals (including
    // named local vars, function params, and anon locals created by the
    // parser).
    for (Obj *var = fn->locals; var; var = var->next) {
      if (var->offset) {
        continue;
      }
      bottom += align_to(var->ty->size, var->align);
      var->offset = -bottom;
    }

    // Set label destination.
    label_set_dest(fn->label);

    // Function prologue.
    // The 'enter' instruction does roughly:
    //   push fp
    //   mov fp, sp
    //   sub sp, frame_size
    emit_enter(bottom);

    // Count params.
    int param_count = 0;
    for (Obj *var = fn->params; var; var = var->next) {
      param_count++;
    }

    // For variadic functions setup the hidden __va_area__ local var.
    // The last parameter (also hidden) is a pointer to a stack buffer in the
    // callers stack frame that contains the variadic args.
    // TODO : implement.
    if (fn->va_area) {
      // gen_lvar_addr(fn->va_area);
      // fmt_insn("local.get $p%d", param_count);
      // put_insn("i32.store");
      error("variadic functions not supported");
    }

    // Create label for each label node, and add that label to each goto node
    // that refers to the label.
    // TODO : Could move this to resolve_goto_labels in the parser.
    for (Node *label_nd = fn->labels; label_nd; label_nd = label_nd->goto_next) {
      if (label_nd->goto_label == NULL) {
        label_nd->goto_label = new_label(LABEL_CODE);
      }
      for (Node *goto_nd = fn->gotos; goto_nd; goto_nd = goto_nd->goto_next) {
        if (!strcmp(goto_nd->label, label_nd->label)) {
          goto_nd->goto_label = label_nd->goto_label;
        }
      }
    }

    // Emit function body.
    gen_stmt(fn->body);

    if (strcmp(fn->name, "main") == 0) {
      // [https://www.sigbus.info/n1570#5.1.2.2.3p1] The C spec defines
      // a special rule for the main function. Reaching the end of the
      // main function is equivalent to returning 0, even though the
      // behavior is undefined for the other functions.
      emit_mov_r0_imm(0);
    }

    label_set_dest(current_return_label);

    // Emit function epilogue.
    //   mov sp, fp
    //   pop fp
    //   ret
    emit_leave();

    current_fn = NULL;
    current_return_label = NULL;
  }
}

static void emit_data(Obj *prog_obj) {
  for (Obj *var = prog_obj; var; var = var->next) {
    if (var->is_function || !var->is_definition || !var->init_data) {
      continue;
    }
    int pos = 0;
    Relocation *rel = var->rel;
    while (pos < var->ty->size) {
      if (rel && rel->offset == pos) {
        // Data-to-data or data-to-code reference.
        // TODO : Needs runtime fixup.
        int64_t rel_var_offset = rel->var->label->vaddr + rel->addend;
        emit_i64(rel_var_offset);
        rel = rel->next;
        pos += 8;
      } else {
        emit_u8(var->init_data[pos] & 0xff);
        pos += 1;
      }
    }
  }
}

#define EHSIZE 64
#define PHENTSIZE 56
#define SHENTSIZE 64

void codegen(Prog *progs, ByteArray *out) {
  current_out = out;

  // Emit ELF file header.
  // e_ident (64-bit, little endian, version 1, ELFOSABI_NONE)
  emit_bytes("\x7f\x45\x4c\x46\x02\x01\x01\x00", 8);
  // e_ident continued (os abi version 0, 7 bytes padding)
  emit_bytes("\x00\x00\x00\x00\x00\x00\x00\x00", 8);
  emit_i16(3); // e_type: ET_DYN
  emit_i16(0x3e); // e_machine: AMD64=0x3e ARM64=0xb7
  emit_i32(1); // e_version: 1
  emit_i64(0); // e_entry: entry point
  emit_i64(EHSIZE); // e_phoff: program header offset
  emit_i64(0); // e_shoff: section header offset
  emit_i32(0); // e_flags: 0
  emit_i16(EHSIZE); // e_ehsize: ELF header size
  emit_i16(PHENTSIZE); // e_phentsize: program header size
  emit_i16(2); // e_phnum: number of program headers
  emit_i16(SHENTSIZE); // e_shentsize: section header size
  emit_i16(0); // e_shnum: number of section headers
  emit_i16(0); // e_shstrndx: string table index

  // Emit text program header.
  emit_i32(1); // p_type: PT_LOAD
  emit_i32(5); // p_flags: R+X
  emit_i64(0); // p_offset: offset in file
  emit_i64(0); // p_vaddr: virtual address
  emit_i64(0); // p_paddr: physical address
  emit_i64(0); // p_filesz: size in file
  emit_i64(0); // p_memsz: size in memory
  emit_i64(SEG_ALIGN); // p_align: alignment

  // Emit data program header.
  emit_i32(1); // p_type: PT_LOAD
  emit_i32(6); // p_flags: R+W
  emit_i64(0); // p_offset: offset in file
  emit_i64(0); // p_vaddr: virtual address
  emit_i64(0); // p_paddr: physical address
  emit_i64(0); // p_filesz: size in file
  emit_i64(0); // p_memsz: size in memory
  emit_i64(SEG_ALIGN); // p_align: alignment

  resolve_names(progs);

  int data_filesz = 0;
  int data_memsz = 0;
  calculate_gvar_offsets(progs, &data_filesz, &data_memsz);

  for (Prog *prog = progs; prog; prog = prog->next) {
    emit_funcs(prog->obj);
  }

  int text_filesz = align_to(current_offset, SEG_ALIGN);
  while (current_offset < text_filesz) {
    emit_u8(0);
  }

  // Set label offsets for global vars now that we know the text segment size.
  for (Prog *prog = progs; prog; prog = prog->next) {
    for (Obj *var = prog->obj; var; var = var->next) {
      if (var->is_function || !var->is_definition) {
        continue;
      }
      var->label->offset = text_filesz + var->offset;
      var->label->vaddr = text_filesz + var->offset;
    }
  }

  // TODO : emit runtime relocation handlers for data-to-data and data-to-code
  //   references.

  for (Prog *prog = progs; prog; prog = prog->next) {
    emit_data(prog->obj);
  }

  // Fix up code-to-code and code-to-data references.
  for (Label *label = labels; label; label = label->next) {
      for (LabelRef *ref = label->refs; ref; ref = ref->next) {
          int32_t disp = label->vaddr - ref->vaddr;
          patch_i32(ref->offset - 4, disp);
      }
  }

  // Find entry point (_start).
  Obj *start_func = find_exported_obj(progs, "_start");
  if (start_func == NULL || !start_func->is_function) {
    error("failed to find _start function");
  }

  // Fix up ELF file and program headers.
  patch_i64(24, start_func->label->vaddr); // e_entry
  patch_i64(EHSIZE + 32, text_filesz); // text p_filesz
  patch_i64(EHSIZE + 40, text_filesz); // text p_memsz
  patch_i64(EHSIZE + PHENTSIZE + 8, text_filesz); // data p_offset
  patch_i64(EHSIZE + PHENTSIZE + 16, text_filesz); // data p_vaddr
  patch_i64(EHSIZE + PHENTSIZE + 24, text_filesz); // data p_paddr
  patch_i64(EHSIZE + PHENTSIZE + 32, data_filesz); // data p_filesz
  patch_i64(EHSIZE + PHENTSIZE + 40, data_memsz); // data p_memsz

  current_out = NULL;
}
