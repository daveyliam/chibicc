#include "chibicc.h"

#include "llvm-c/Core.h"
#include "llvm-c/Error.h"
#include "llvm-c/LLJIT.h"
#include "llvm-c/Support.h"
#include "llvm-c/Target.h"
#include "llvm-c/Analysis.h"

static LLVMContextRef current_ir_context = NULL;
static LLVMModuleRef current_ir_module = NULL;
static LLVMBuilderRef current_ir_builder = NULL;
static LLVMValueRef current_ir_function = NULL;

static LLVMBasicBlockRef current_break_bb = NULL;
static LLVMBasicBlockRef current_continue_bb = NULL;
static LLVMValueRef current_switch_insn = NULL;
static LLVMBasicBlockRef current_switch_default_bb = NULL;
static LLVMBasicBlockRef current_switch_extra_bb = NULL;
static Type *current_switch_cond_ty = NULL;
static LLVMValueRef current_switch_cond_val = NULL;

static LLVMValueRef builtin_va_start_ir_val = NULL;
static LLVMValueRef builtin_va_copy_ir_val = NULL;
static LLVMValueRef builtin_va_end_ir_val = NULL;
static LLVMTypeRef builtin_va_start_ir_type = NULL;
static LLVMTypeRef builtin_va_copy_ir_type = NULL;
static LLVMTypeRef builtin_va_end_ir_type = NULL;

static LLVMValueRef gen_expr(Node *node);
static LLVMValueRef gen_stmt(Node *node);

#define ADDR_SPACE 0

// Round up `n` to the nearest multiple of `align`. For instance,
// align_to(5, 8) returns 8 and align_to(11, 8) returns 16.
int align_to(int n, int align) {
  return (n + align - 1) / align * align;
}

static bool is_current_bb_terminated(void) {
  LLVMBasicBlockRef bb = LLVMGetInsertBlock(current_ir_builder);
  return LLVMGetBasicBlockTerminator(bb) != NULL;
}

static LLVMTypeRef get_opaque_ptr_type() {
  return LLVMPointerType(LLVMVoidType(), ADDR_SPACE);
}

static LLVMTypeRef get_llvm_type(Type *ty) {
  if (ty->ir_type != NULL) {
    return ty->ir_type;
  }
  if (ty->ir_type_started) {
    error("recursive type");
  }
  ty->ir_type_started = true;
  switch (ty->kind) {
  case TY_VOID:
    ty->ir_type = LLVMVoidType();
    break;
  case TY_BOOL:
    ty->ir_type = LLVMInt1Type();
    break;
  case TY_CHAR:
    ty->ir_type = LLVMInt8Type();
    break;
  case TY_SHORT:
    ty->ir_type = LLVMInt16Type();
    break;
  case TY_INT:
    ty->ir_type = LLVMInt32Type();
    break;
  case TY_LONG:
    ty->ir_type = LLVMInt64Type();
    break;
  case TY_FLOAT:
    ty->ir_type = LLVMFloatType();
    break;
  case TY_DOUBLE:
    ty->ir_type = LLVMDoubleType();
    break;
  case TY_LDOUBLE:
    ty->ir_type = LLVMFP128Type();
    break;
  case TY_ENUM:
    ty->ir_type = LLVMInt32Type();
    break;
  case TY_PTR:
    // All LLVM pointers are opaque, so just set the element type to void.
    // Also sidesteps recursion issue with struct members referring to the struct type in the
    // type declaration.
    ty->ir_type = get_opaque_ptr_type();
    break;
  case TY_FUNC: {
    size_t param_count = 0;
    bool returns_struct = (ty->return_ty->kind == TY_STRUCT || ty->return_ty->kind == TY_UNION);
    bool returns_large_struct = returns_struct && (ty->return_ty->size > 16);
    if (returns_large_struct) {
      param_count += 1;
    }
    for (Type *param_ty = ty->params; param_ty; param_ty = param_ty->next) {
      param_count += 1;
    }
    LLVMTypeRef *param_types = NULL;
    if (param_count > 0) {
      param_types = calloc(param_count, sizeof(LLVMTypeRef));
      param_count = 0;
      if (returns_large_struct) {
        param_types[0] = get_opaque_ptr_type();
        param_count += 1;
      }
      for (Type *param_ty = ty->params; param_ty; param_ty = param_ty->next) {
        param_types[param_count] = get_llvm_type(param_ty);
        param_count += 1;
      }
    }
    LLVMTypeRef ret_type;
    if (returns_large_struct) {
      ret_type = LLVMVoidType();
    } else {
      ret_type = get_llvm_type(ty->return_ty);
    }
    ty->ir_type = LLVMFunctionType(ret_type, param_types, param_count, ty->is_variadic);
    break;
  }
  case TY_ARRAY:
    ty->ir_type = LLVMArrayType(get_llvm_type(ty->base), ty->array_len);
    break;
  case TY_VLA:
    // Don't know the size, so need to treat variable length arrays as opaque pointers.
    ty->ir_type = get_opaque_ptr_type();
    break;
  case TY_STRUCT: {
    size_t element_count = 0;
    for (Member *m = ty->members; m; m = m->next) {
      if (m->is_bitfield) {
        error_tok(m->tok, "struct bitfields not supported");
      }
      element_count += 1;
    }
    LLVMTypeRef *elements = NULL;
    if (element_count > 0) {
      elements = calloc(element_count, sizeof(LLVMTypeRef));
      element_count = 0;
      for (Member *m = ty->members; m; m = m->next) {

        elements[element_count] = get_llvm_type(m->ty);
        element_count += 1;
      }
    }
    ty->ir_type = LLVMStructType(elements, element_count, ty->is_packed);
    break;
  }
  case TY_UNION: {
    // In LLVM IR there is no such thing as a union. Unions are emulated by creating a struct that
    // matches the largest union variant. Then all access to other variants is via bitcasting.
    size_t size = 0;
    Member *largest_member = ty->members;
    for (Member *m = ty->members; m; m = m->next) {
      if (m->ty->size > size) {
        size = m->ty->size;
        largest_member = m;
      }
    }
    LLVMTypeRef mem_ir_type = get_llvm_type(largest_member->ty);
    ty->ir_type = LLVMStructType(&mem_ir_type, 1, ty->is_packed);
    break;
  }
  default:
    error("get_llvm_type: type kind %d unimplemented", ty->kind);
  }
  if (ty->ir_type == NULL) {
    error("get_llvm_type: unreachable");
  }
  return ty->ir_type;
}

static LLVMValueRef get_zero_val(Type *ty) {
  return LLVMConstNull(get_llvm_type(ty));
}

static LLVMValueRef gen_is_non_zero(Type *ty, LLVMValueRef val) {
  switch (ty->kind) {
    case TY_BOOL:
    case TY_CHAR:
    case TY_SHORT:
    case TY_INT:
    case TY_LONG:
    case TY_ENUM:
      return LLVMBuildICmp(
        current_ir_builder, LLVMIntNE, val, get_zero_val(ty), ""
      );
    case TY_FLOAT:
    case TY_DOUBLE:
    case TY_LDOUBLE:
      return LLVMBuildFCmp(
        current_ir_builder, LLVMRealUNE, val, get_zero_val(ty), ""
      );
    case TY_PTR:
    case TY_FUNC:
    case TY_ARRAY:
    case TY_VLA:
    {
      return LLVMBuildICmp(
        current_ir_builder, LLVMIntNE, val, get_zero_val(ty), ""
      );
    }
  }
  error("gen_is_non_zero: bad type");
}

// Compute the absolute address of a given node.
// It's an error if a given node does not reside in memory.
static LLVMValueRef gen_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR:
    // For locals ir_val is an alloca containing the variable's value.
    // For globals ir_val is the value from AddGlobal, which resolves to an address.
    // For functions ir_val is the value from AddFunction, which resolves to an address.
    if (node->var->ir_val == NULL) {
      error_tok(node->tok, "node->var->ir_val is null for var");
    }
    if (node->var->ty->kind == TY_VLA) {
      // For VLA locals the address of the alloca for the array is stored in the local
      // var alloca. So we need to load the local to get the address of the array.
      return LLVMBuildLoad2(
        current_ir_builder, get_llvm_type(node->var->ty), node->var->ir_val, ""
      );
    }
    return node->var->ir_val;
  case ND_DEREF:
    return gen_expr(node->lhs);
  case ND_COMMA:
    gen_expr(node->lhs);
    return gen_addr(node->rhs);
  case ND_MEMBER: {
    LLVMValueRef base = gen_addr(node->lhs);
    if (node->lhs->ty->kind == TY_STRUCT) {
      LLVMValueRef indexes[2] = {
        LLVMConstInt(LLVMInt32Type(), 0, false),
        LLVMConstInt(LLVMInt32Type(), node->member->idx, false)
      };
      return LLVMBuildGEP2(current_ir_builder, get_llvm_type(node->lhs->ty), base, indexes, 2, "");
    }
    else if (node->lhs->ty->kind == TY_UNION) {
      return LLVMBuildBitCast(
        current_ir_builder, base, LLVMPointerType(get_llvm_type(node->member->ty), ADDR_SPACE), ""
      );
    }
    else {
      error_tok(node->tok, "bad type for ND_MEMBER");
    }
  }
  case ND_FUNCALL:
    // If the function returns a struct, then it should be copied to the local var 'ret_buffer'.
    if (node->ret_buffer) {
      return gen_expr(node);
    }
    break;
  case ND_ASSIGN:
  case ND_COND:
    if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION) {
      return gen_expr(node);
    }
    break;
  case ND_VLA_PTR:
    // This node type wraps an ND_VAR node when a VLA is assigned to a local var.
    // Seems to be used to prevent the var from being loaded as would normally be done to
    // get the address of the contained VLA.
    return node->var->ir_val;
  }

  error_tok(node->tok, "not an lvalue");
}

// Load a value from where addr is pointing to.
static LLVMValueRef load(Type *ty, LLVMValueRef addr) {
  switch (ty->kind) {
  case TY_ARRAY:
  // XXX : causes issues passing structs by value in ND_FUNCALL.
  // But without causes issues in ND_ASSIGN (e.g. (struct1=struct2).x).
  // How to resolve? Add loads where necessary around function calls and returns?
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
    return addr;
  }
  return LLVMBuildLoad2(current_ir_builder, get_llvm_type(ty), addr, "");
}

static void store(Type *ty, LLVMValueRef addr, LLVMValueRef val) {
  // TODO : does this work for structs and arrays? Or do we need to memcpy?
  LLVMBuildStore(current_ir_builder, val, addr);
}

static LLVMValueRef cast(Type *from, Type *to, LLVMValueRef val) {
  if (to->kind == from->kind) {
    return val;
  }
  switch (to->kind) {
    case TY_VOID:
      return val;
    case TY_BOOL:
      return gen_is_non_zero(from, val);
    case TY_CHAR:
    case TY_SHORT:
    case TY_INT:
    case TY_LONG:
    case TY_ENUM:
      switch (from->kind) {
        case TY_BOOL:
          // cast bool to integer.
          return LLVMBuildZExt(current_ir_builder, val, get_llvm_type(to), "");
        case TY_CHAR:
        case TY_SHORT:
        case TY_INT:
        case TY_LONG:
        case TY_ENUM:
          // cast integer to integer.
          if (to->size == from->size) {
            return LLVMBuildBitCast(current_ir_builder, val, get_llvm_type(to), "");
          }
          if (to->size < from->size) {
            return LLVMBuildTrunc(current_ir_builder, val, get_llvm_type(to), "");
          }
          if (from->is_unsigned) {
            return LLVMBuildZExt(current_ir_builder, val, get_llvm_type(to), "");
          }
          return LLVMBuildSExt(current_ir_builder, val, get_llvm_type(to), "");
        case TY_FLOAT:
        case TY_DOUBLE:
        case TY_LDOUBLE:
          // cast float to integer.
          if (to->is_unsigned) {
            return LLVMBuildFPToUI(current_ir_builder, val, get_llvm_type(to), "");
          }
          return LLVMBuildFPToSI(current_ir_builder, val, get_llvm_type(to), "");
        case TY_PTR:
        case TY_FUNC:
        case TY_ARRAY:
        case TY_VLA:
          // cast pointer to integer.
          return LLVMBuildPtrToInt(current_ir_builder, val, get_llvm_type(to), "");
        default:
          // cannot cast struct to integer.
          break;
      }
      break;
    case TY_FLOAT:
    case TY_DOUBLE:
    case TY_LDOUBLE:
      switch (from->kind) {
        case TY_BOOL:
          // cast bool to float.
          return LLVMBuildUIToFP(current_ir_builder, val, get_llvm_type(to), "");
        case TY_CHAR:
        case TY_SHORT:
        case TY_INT:
        case TY_LONG:
        case TY_ENUM:
          // cast integer to float.
          if (from->is_unsigned) {
            return LLVMBuildUIToFP(current_ir_builder, val, get_llvm_type(to), "");
          }
          return LLVMBuildSIToFP(current_ir_builder, val, get_llvm_type(to), "");
        case TY_FLOAT:
        case TY_DOUBLE:
        case TY_LDOUBLE:
          // cast float to float.
          if (to->size == from->size) {
            return val;
          }
          if (to->size < from->size) {
            return LLVMBuildFPTrunc(current_ir_builder, val, get_llvm_type(to), "");
          }
          return LLVMBuildFPExt(current_ir_builder, val, get_llvm_type(to), "");
        default:
          // cannot cast pointer or struct to float.
          break;
      }
      break;
    case TY_PTR:
    case TY_FUNC:
    case TY_ARRAY:
    case TY_VLA:
      switch (from->kind) {
        case TY_BOOL:
        case TY_CHAR:
        case TY_SHORT:
        case TY_INT:
        case TY_LONG:
        case TY_ENUM:
          // cast integer to pointer.
          // TODO : need to cast to pointer sized int first?
          return LLVMBuildIntToPtr(current_ir_builder, val, get_llvm_type(to), "");
        case TY_PTR:
        case TY_FUNC:
        case TY_ARRAY:
        case TY_VLA:
          // cast pointer to pointer.
          // all pointers are opaque in recent LLVM, so don't need to bitcast or anything.
          return val;
        default:
          // cannot cast float or struct to pointer.
          break;
      }
      break;
    default:
      // cannot cast struct to integer, float, or pointer.
      break;
  }
  error("unable to cast from type %d to %d\n", from->kind, to->kind);
}

// Generate code for a given node.
static LLVMValueRef gen_expr(Node *node) {
  switch (node->kind) {
  case ND_NULL_EXPR:
    return NULL;
  case ND_NUM: {
    switch (node->ty->kind) {
    case TY_FLOAT:
    case TY_DOUBLE:
    case TY_LDOUBLE:
      return LLVMConstReal(get_llvm_type(node->ty), node->fval);
    case TY_BOOL:
    case TY_CHAR:
    case TY_SHORT:
    case TY_INT:
    case TY_LONG:
    case TY_ENUM:
      return LLVMConstInt(get_llvm_type(node->ty), node->val, false);
    default:
      break;
    }
    error_tok(node->tok, "bad type for number expression");
  }
  case ND_NEG: {
    LLVMValueRef expr = gen_expr(node->lhs);
    switch (node->ty->kind) {
    case TY_FLOAT:
    case TY_DOUBLE:
    case TY_LDOUBLE:
      return LLVMBuildFNeg(current_ir_builder, expr, "");
    }
    return LLVMBuildNeg(current_ir_builder, expr, "");
  }
  case ND_VAR:
    return load(node->ty, gen_addr(node));
  case ND_MEMBER:
    // Member *mem = node->member;
    // if (mem->is_bitfield) {
    //   println("  shl $%d, %%rax", 64 - mem->bit_width - mem->bit_offset);
    //   if (mem->ty->is_unsigned)
    //     println("  shr $%d, %%rax", 64 - mem->bit_width);
    //   else
    //     println("  sar $%d, %%rax", 64 - mem->bit_width);
    // }
    return load(node->ty, gen_addr(node));
  case ND_DEREF:
    return load(node->ty, gen_expr(node->lhs));
  case ND_ADDR:
    return gen_addr(node->lhs);
  case ND_ASSIGN: {
    LLVMValueRef addr = gen_addr(node->lhs);
    LLVMValueRef val = gen_expr(node->rhs);

    if ((node->lhs->ty->kind == TY_STRUCT || node->lhs->ty->kind == TY_UNION)) {
      LLVMBuildMemMove(
        current_ir_builder,
        addr, 1,
        val, 1,
        LLVMConstInt(LLVMInt32Type(), node->lhs->ty->size, false)
      );
      return val;
    }

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

    store(node->ty, addr, val);
    return val;
  }
  case ND_STMT_EXPR: {
    LLVMValueRef ret = NULL;
    for (Node *n = node->body; n; n = n->next) {
      ret = gen_stmt(n);
    }
    return ret;
  }
  case ND_COMMA:
    gen_expr(node->lhs);
    return gen_expr(node->rhs);
  case ND_CAST:
    return cast(node->lhs->ty, node->ty, gen_expr(node->lhs));
  case ND_MEMZERO:
    // Node is always a local var.
    return LLVMBuildMemSet(
      current_ir_builder,
      node->var->ir_val,
      LLVMConstInt(LLVMInt8Type(), 0, false),
      LLVMConstInt(LLVMInt32Type(), node->var->ty->size, false),
      1
    );
  case ND_COND: {
    // Ternary expression.
    // TODO : clang creates an extra alloca for the result rather than using phi.
    LLVMBasicBlockRef then_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMBasicBlockRef else_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMValueRef cond_val = gen_is_non_zero(node->cond->ty, gen_expr(node->cond));
    LLVMBuildCondBr(current_ir_builder, cond_val, then_bb, else_bb);

    LLVMPositionBuilderAtEnd(current_ir_builder, then_bb);
    LLVMValueRef then_val = gen_expr(node->then);
    LLVMBasicBlockRef then_final_bb = LLVMGetInsertBlock(current_ir_builder);
    LLVMBuildBr(current_ir_builder, end_bb);
  
    LLVMPositionBuilderAtEnd(current_ir_builder, else_bb);
    LLVMValueRef else_val = gen_expr(node->els);
    LLVMBasicBlockRef else_final_bb = LLVMGetInsertBlock(current_ir_builder);
    LLVMBuildBr(current_ir_builder, end_bb);

    LLVMPositionBuilderAtEnd(current_ir_builder, end_bb);
    LLVMValueRef phi = NULL;
    if (node->ty->kind != TY_VOID) {
      // if this returns a struct or union, set the result type to pointer.
      // TODO : is this correct?
      LLVMTypeRef result_ty;
      if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION) {
        result_ty = get_opaque_ptr_type();
      } else {
        result_ty = get_llvm_type(node->ty);
      }
      phi = LLVMBuildPhi(current_ir_builder, result_ty, "");
      LLVMAddIncoming(phi, &then_val, &then_final_bb, 1);
      LLVMAddIncoming(phi, &else_val, &else_final_bb, 1);
    }
    return phi;
  }
  case ND_NOT: {
    LLVMValueRef val1 = gen_is_non_zero(node->lhs->ty, gen_expr(node->lhs));
    LLVMValueRef val2 = LLVMBuildNot(current_ir_builder, val1, "");
    return LLVMBuildZExt(current_ir_builder, val2, get_llvm_type(node->ty), "");
  }
  case ND_BITNOT:
    return LLVMBuildNot(current_ir_builder, gen_expr(node->lhs), "");
  case ND_LOGAND: {
    // Logical AND with short-circuit. Skip the RHS expression if the LHS is false.
    LLVMBasicBlockRef rhs_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMValueRef lhs_val = gen_is_non_zero(node->lhs->ty, gen_expr(node->lhs));
    LLVMBasicBlockRef lhs_final_bb = LLVMGetInsertBlock(current_ir_builder);
    LLVMBuildCondBr(current_ir_builder, lhs_val, rhs_bb, end_bb);

    LLVMPositionBuilderAtEnd(current_ir_builder, rhs_bb);
    LLVMValueRef rhs_val = gen_is_non_zero(node->rhs->ty, gen_expr(node->rhs));
    LLVMBasicBlockRef rhs_final_bb = LLVMGetInsertBlock(current_ir_builder);
    LLVMBuildBr(current_ir_builder, end_bb);

    LLVMPositionBuilderAtEnd(current_ir_builder, end_bb);
    LLVMValueRef result = NULL;
    if (node->ty->kind != TY_VOID) {
      LLVMValueRef phi = LLVMBuildPhi(current_ir_builder, LLVMInt1Type(), "");
      LLVMValueRef false_val = LLVMConstInt(LLVMInt1Type(), 0, false);
      LLVMAddIncoming(phi, &false_val, &lhs_final_bb, 1);
      LLVMAddIncoming(phi, &rhs_val, &rhs_final_bb, 1);
      result = LLVMBuildZExt(current_ir_builder, phi, get_llvm_type(node->ty), "");
    }
    return result;
  }
  case ND_LOGOR: {
    // Logical OR with short-circuit. Skip the RHS expression if the LHS is true.
    LLVMBasicBlockRef rhs_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMValueRef lhs_val = gen_is_non_zero(node->lhs->ty, gen_expr(node->lhs));
    LLVMBasicBlockRef lhs_final_bb = LLVMGetInsertBlock(current_ir_builder);
    LLVMBuildCondBr(current_ir_builder, lhs_val, end_bb, rhs_bb);

    LLVMPositionBuilderAtEnd(current_ir_builder, rhs_bb);
    LLVMValueRef rhs_val = gen_is_non_zero(node->rhs->ty, gen_expr(node->rhs));
    LLVMBasicBlockRef rhs_final_bb = LLVMGetInsertBlock(current_ir_builder);
    LLVMBuildBr(current_ir_builder, end_bb);

    LLVMPositionBuilderAtEnd(current_ir_builder, end_bb);
    LLVMValueRef result = NULL;
    if (node->ty->kind != TY_VOID) {
      LLVMValueRef phi = LLVMBuildPhi(current_ir_builder, LLVMInt1Type(), "");
      LLVMValueRef true_val = LLVMConstInt(LLVMInt1Type(), 1, false);
      LLVMAddIncoming(phi, &true_val, &lhs_final_bb, 1);
      LLVMAddIncoming(phi, &rhs_val, &rhs_final_bb, 1);
      result = LLVMBuildZExt(current_ir_builder, phi, get_llvm_type(node->ty), "");
    }
    return result;
  }
  case ND_FUNCALL: {
    // Get the function address.
    LLVMValueRef func_val = gen_expr(node->lhs);

    // Build args.
    // If the return type is a large struct/union, the caller passes
    // a pointer to a buffer as if it were the first argument.
    size_t arg_count = 0;
    bool returns_struct = node->ret_buffer;
    bool returns_large_struct = node->ret_buffer && (node->ty->size > 16);
    for (Node *arg = node->args; arg; arg = arg->next) {
      arg_count += 1;
    }
    if (returns_large_struct) {
      arg_count += 1;
    }
    LLVMValueRef *arg_vals = NULL;
    if (arg_count > 0) {
      arg_vals = calloc(arg_count, sizeof(LLVMValueRef));
      arg_count = 0;
      if (returns_large_struct) {
        arg_vals[0] = node->ret_buffer->ir_val;
        arg_count += 1;
      }
      for (Node *arg = node->args; arg; arg = arg->next) {
        LLVMValueRef arg_val = gen_expr(arg);
        if ((arg->ty->kind == TY_STRUCT) || (arg->ty->kind == TY_UNION)) {
          // TODO : gen_expr always returns pointers, force a load. check correct.
          arg_val = LLVMBuildLoad2(current_ir_builder, get_llvm_type(arg->ty), arg_val, "");
        }
        arg_vals[arg_count] = arg_val;
        arg_count += 1;
      }
    }
  
    // Build call.
    // XXX : what is the type here for? should be type of result? but replacing with node->ty
    // causes failures.
    LLVMValueRef ret = LLVMBuildCall2(
      current_ir_builder, get_llvm_type(node->func_ty), func_val, arg_vals, arg_count, ""
    );
  
    if (returns_large_struct) {
      ret = node->ret_buffer->ir_val;
    }
    else if (returns_struct) {
      LLVMBuildStore(current_ir_builder, ret, node->ret_buffer->ir_val);
      ret = node->ret_buffer->ir_val;
    }

    // If this is a non-returning function, then we should add an unreachable instruction.
    // How do we get the function obj (global) from the function call node?
    // Or can we add is_noreturn to the type at parse time?
    // if (func_var->is_noreturn) {
    //   LLVMBuildUnreachable(current_ir_builder);
    // }

    return ret;
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
    LLVMValueRef alloca_size = gen_expr(node->args);
    // TODO : Do we need to cast to LHS var type?
    return LLVMBuildArrayAlloca(current_ir_builder, LLVMInt8Type(), alloca_size, "");
  }
  case ND_VA_START: {
    LLVMValueRef arg = gen_expr(node->args);
    LLVMValueRef ret = LLVMBuildCall2(
      current_ir_builder, builtin_va_start_ir_type, builtin_va_start_ir_val, &arg, 1, ""
    );
    return ret;
  }
  case ND_VA_COPY: {
    LLVMValueRef arg1 = gen_expr(node->args);
    LLVMValueRef arg2 = gen_expr(node->args->next);
    LLVMValueRef args[2] = {arg1, arg2};
    LLVMValueRef ret = LLVMBuildCall2(
      current_ir_builder, builtin_va_copy_ir_type, builtin_va_copy_ir_val, args, 2, ""
    );
    return ret;
  }
  case ND_VA_END: {
    LLVMValueRef arg = gen_expr(node->args);
    LLVMValueRef ret = LLVMBuildCall2(
      current_ir_builder, builtin_va_end_ir_type, builtin_va_end_ir_val, &arg, 1, ""
    );
    return ret;
  }
  case ND_VA_ARG: {
    LLVMValueRef arg = gen_expr(node->args);
    LLVMValueRef ret = LLVMBuildVAArg(current_ir_builder, arg, get_llvm_type(node->arg_ty), "");
    return ret;
  }
  }

  // All remaining node types should be simple binary expressions.
  if (node->lhs == NULL) {
    error_tok(node->tok, "unhandled expression: kind=%d", node->kind);
  }

  switch (node->lhs->ty->kind) {
  case TY_FLOAT:
  case TY_DOUBLE:
  case TY_LDOUBLE: {
    LLVMValueRef lhs_val = gen_expr(node->lhs);
    LLVMValueRef rhs_val = gen_expr(node->rhs);

    switch (node->kind) {
    case ND_ADD:
      return LLVMBuildFAdd(current_ir_builder, lhs_val, rhs_val, "");
    case ND_SUB:
      return LLVMBuildFSub(current_ir_builder, lhs_val, rhs_val, "");
    case ND_MUL:
      return LLVMBuildFMul(current_ir_builder, lhs_val, rhs_val, "");
    case ND_DIV:
      return LLVMBuildFDiv(current_ir_builder, lhs_val, rhs_val, "");
    case ND_EQ: {
      LLVMValueRef val = LLVMBuildFCmp(current_ir_builder, LLVMRealOEQ, lhs_val, rhs_val, "");
      return LLVMBuildZExt(current_ir_builder, val, get_llvm_type(node->ty), "");
    }
    case ND_NE: {
      LLVMValueRef val = LLVMBuildFCmp(current_ir_builder, LLVMRealUNE, lhs_val, rhs_val, "");
      return LLVMBuildZExt(current_ir_builder, val, get_llvm_type(node->ty), "");
    }
    case ND_LT: {
      LLVMValueRef val = LLVMBuildFCmp(current_ir_builder, LLVMRealOLT, lhs_val, rhs_val, "");
      return LLVMBuildZExt(current_ir_builder, val, get_llvm_type(node->ty), "");
    }
    case ND_LE: {
      LLVMValueRef val = LLVMBuildFCmp(current_ir_builder, LLVMRealOLE, lhs_val, rhs_val, "");
      return LLVMBuildZExt(current_ir_builder, val, get_llvm_type(node->ty), "");
    }
    }
    error_tok(node->tok, "invalid floating point expression");
  }
  case TY_PTR:
  case TY_FUNC:
  case TY_ARRAY:
  case TY_VLA:
  {
    LLVMValueRef lhs_val = gen_expr(node->lhs);
    LLVMValueRef rhs_val = gen_expr(node->rhs);

    switch (node->kind) {
    case ND_ADD:
      // Can only add pointers and integers.
      // Parser always makes the pointer the LHS.
      // TODO : for functions and void pointers make the base type i8?
      switch (node->rhs->ty->kind) {
      case TY_BOOL:
      case TY_CHAR:
      case TY_SHORT:
      case TY_INT:
      case TY_LONG:
      case TY_ENUM: {
        LLVMTypeRef base_ir_type;
        if (node->lhs->ty->base->kind == TY_VOID) {
          base_ir_type = LLVMInt8Type();
        } else if (node->lhs->ty->base->kind == TY_VLA) {
          // If the LHS is a pointer to a VLA the parser multiplies the RHS
          // integer by the VLA size in bytes.
          base_ir_type = LLVMInt8Type();
        } else {
          base_ir_type = get_llvm_type(node->lhs->ty->base);
        }
        return LLVMBuildGEP2(
          current_ir_builder, base_ir_type, lhs_val, &rhs_val, 1, ""
        );
      }
      default:
        error_tok(node->tok, "invalid pointer addition");
      }
      break;
    case ND_SUB: {
      switch (node->rhs->ty->kind) {
      case TY_BOOL:
      case TY_CHAR:
      case TY_SHORT:
      case TY_INT:
      case TY_LONG:
      case TY_ENUM: {
        // Subtract pointer and integer.
        LLVMValueRef rhs_val_neg = LLVMBuildNeg(current_ir_builder, rhs_val, "");
        LLVMTypeRef base_ir_type;
        if (node->lhs->ty->base->kind == TY_VOID) {
          base_ir_type = LLVMInt8Type();
        } else {
          base_ir_type = get_llvm_type(node->lhs->ty->base);
        }
        return LLVMBuildGEP2(
          current_ir_builder, base_ir_type, lhs_val, &rhs_val_neg, 1, ""
        );
      }
      case TY_PTR:
      case TY_FUNC:
      case TY_ARRAY:
      case TY_VLA: {
        // Subtract two pointers.
        // Returns the difference in number of elements (not bytes).
        // TODO : what should happen if LHS and RHS point to different sized base types?
        LLVMValueRef lhs_val_int = LLVMBuildPtrToInt(
          current_ir_builder, lhs_val, LLVMInt64Type(), ""
        );
        LLVMValueRef rhs_val_int = LLVMBuildPtrToInt(
          current_ir_builder, rhs_val, LLVMInt64Type(), ""
        );
        LLVMValueRef diff_in_bytes_val = LLVMBuildSub(
          current_ir_builder, lhs_val_int, rhs_val_int, ""
        );
        size_t elem_size = node->lhs->ty->base->size;
        if (elem_size == 1) {
          return diff_in_bytes_val;
        }
        return LLVMBuildSDiv(
          current_ir_builder,
          diff_in_bytes_val,
          LLVMConstInt(LLVMInt64Type(), elem_size, false),
          ""
        );
      }
      default:
        error_tok(node->tok, "invalid pointer subtraction");
      }
      break;
    }
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE:
    {
      LLVMValueRef lhs_val_int = LLVMBuildPtrToInt(
        current_ir_builder, lhs_val, LLVMInt64Type(), ""
      );
      LLVMValueRef rhs_val_int = LLVMBuildPtrToInt(
        current_ir_builder, rhs_val, LLVMInt64Type(), ""
      );
      LLVMIntPredicate pred;
      switch (node->kind) {
        case ND_EQ:
          pred = LLVMIntEQ;
          break;
        case ND_NE:
          pred = LLVMIntNE;
          break;
        case ND_LT:
          pred = LLVMIntULT;
          break;
        case ND_LE:
          pred = LLVMIntULE;
          break;
        default:
          error("unreachable pointer comparison");
          break;
      }
      LLVMValueRef val = LLVMBuildICmp(current_ir_builder, pred, lhs_val_int, rhs_val_int, "");
      return LLVMBuildZExt(current_ir_builder, val, get_llvm_type(node->ty), "");
    }
    }
    error_tok(node->tok, "invalid pointer expression");
  }
  }

  LLVMValueRef lhs_val = gen_expr(node->lhs);
  LLVMValueRef rhs_val = gen_expr(node->rhs);

  LLVMValueRef result = NULL;

  switch (node->kind) {
  case ND_ADD:
    result = LLVMBuildAdd(current_ir_builder, lhs_val, rhs_val, "");
    break;
  case ND_SUB:
    result = LLVMBuildSub(current_ir_builder, lhs_val, rhs_val, "");
    break;
  case ND_MUL:
    result = LLVMBuildMul(current_ir_builder, lhs_val, rhs_val, "");
    break;
  case ND_DIV:
    if (node->ty->is_unsigned) {
      result = LLVMBuildUDiv(current_ir_builder, lhs_val, rhs_val, "");
    } else {
      result = LLVMBuildSDiv(current_ir_builder, lhs_val, rhs_val, "");
    }
    break;
  case ND_MOD:
    if (node->ty->is_unsigned) {
      result = LLVMBuildURem(current_ir_builder, lhs_val, rhs_val, "");
    } else {
      result = LLVMBuildSRem(current_ir_builder, lhs_val, rhs_val, "");
    }
    break;
  case ND_BITAND:
    result = LLVMBuildAnd(current_ir_builder, lhs_val, rhs_val, "");
    break;
  case ND_BITOR:
    result = LLVMBuildOr(current_ir_builder, lhs_val, rhs_val, "");
    break;
  case ND_BITXOR:
    result = LLVMBuildXor(current_ir_builder, lhs_val, rhs_val, "");
    break;
  case ND_EQ:
    result = LLVMBuildICmp(current_ir_builder, LLVMIntEQ, lhs_val, rhs_val, "");
    result = LLVMBuildZExt(current_ir_builder, result, get_llvm_type(node->ty), "");
    break;
  case ND_NE:
    result = LLVMBuildICmp(current_ir_builder, LLVMIntNE, lhs_val, rhs_val, "");
    result = LLVMBuildZExt(current_ir_builder, result, get_llvm_type(node->ty), "");
    break;
  case ND_LT:
    if (node->lhs->ty->is_unsigned) {
      result = LLVMBuildICmp(current_ir_builder, LLVMIntULT, lhs_val, rhs_val, "");
    } else {
      result = LLVMBuildICmp(current_ir_builder, LLVMIntSLT, lhs_val, rhs_val, "");
    }
    result = LLVMBuildZExt(current_ir_builder, result, get_llvm_type(node->ty), "");
    break;
  case ND_LE:
    if (node->lhs->ty->is_unsigned) {
      result = LLVMBuildICmp(current_ir_builder, LLVMIntULE, lhs_val, rhs_val, "");
    } else {
      result = LLVMBuildICmp(current_ir_builder, LLVMIntSLE, lhs_val, rhs_val, "");
    }
    result = LLVMBuildZExt(current_ir_builder, result, get_llvm_type(node->ty), "");
    break;
  case ND_SHL:
    result = LLVMBuildShl(current_ir_builder, lhs_val, rhs_val, "");
    break;
  case ND_SHR:
    if (node->lhs->ty->is_unsigned) {
      result = LLVMBuildLShr(current_ir_builder, lhs_val, rhs_val, "");
    } else {
      result = LLVMBuildAShr(current_ir_builder, lhs_val, rhs_val, "");
    }
    break;
  default:
    error_tok(node->tok, "invalid expression");
  }

  return result;  
}

static LLVMValueRef gen_stmt(Node *node) {
  switch (node->kind) {
  case ND_IF: {
    LLVMBasicBlockRef then_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMBasicBlockRef else_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMValueRef cond_val = gen_is_non_zero(node->cond->ty, gen_expr(node->cond));
    LLVMBuildCondBr(current_ir_builder, cond_val, then_bb, else_bb);

    LLVMPositionBuilderAtEnd(current_ir_builder, then_bb);
    if (node->then) {
      gen_stmt(node->then);
    }

    LLVMBuildBr(current_ir_builder, end_bb);
  
    LLVMPositionBuilderAtEnd(current_ir_builder, else_bb);
    if (node->els) {
      gen_stmt(node->els);
    }
    LLVMBuildBr(current_ir_builder, end_bb);

    LLVMPositionBuilderAtEnd(current_ir_builder, end_bb);
    return NULL;
  }
  case ND_FOR: {
    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMBasicBlockRef inc_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMBasicBlockRef loop_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlock(current_ir_function, "");

    if (node->init) {
      gen_stmt(node->init);
    }
    LLVMBuildBr(current_ir_builder, cond_bb);

    LLVMPositionBuilderAtEnd(current_ir_builder, cond_bb);
    if (node->cond) {
      LLVMValueRef cond_val = gen_is_non_zero(node->cond->ty, gen_expr(node->cond));
      LLVMBuildCondBr(current_ir_builder, cond_val, loop_bb, end_bb);
    } else {
      LLVMBuildBr(current_ir_builder, loop_bb);
    }

    LLVMPositionBuilderAtEnd(current_ir_builder, loop_bb);
    if (node->then) {
      LLVMBasicBlockRef prev_break_bb = current_break_bb;
      LLVMBasicBlockRef prev_continue_bb = current_continue_bb;
      current_break_bb = end_bb;
      current_continue_bb = inc_bb;
      gen_stmt(node->then);
      current_break_bb = prev_break_bb;
      current_continue_bb = prev_continue_bb;
    }
    LLVMBuildBr(current_ir_builder, inc_bb);

    LLVMPositionBuilderAtEnd(current_ir_builder, inc_bb);
    if (node->inc) {
      gen_expr(node->inc);
    }
    LLVMBuildBr(current_ir_builder, cond_bb);

    LLVMPositionBuilderAtEnd(current_ir_builder, end_bb);
    return NULL;
  }
  case ND_DO: {
    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMBasicBlockRef loop_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlock(current_ir_function, "");

    LLVMBuildBr(current_ir_builder, loop_bb);

    LLVMPositionBuilderAtEnd(current_ir_builder, loop_bb);
    if (node->then) {
      LLVMBasicBlockRef prev_break_bb = current_break_bb;
      LLVMBasicBlockRef prev_continue_bb = current_continue_bb;
      current_break_bb = end_bb;
      current_continue_bb = cond_bb;
      gen_stmt(node->then);
      current_break_bb = prev_break_bb;
      current_continue_bb = prev_continue_bb;
    }
    LLVMBuildBr(current_ir_builder, cond_bb);

    LLVMPositionBuilderAtEnd(current_ir_builder, cond_bb);
    LLVMValueRef cond_val = gen_is_non_zero(node->cond->ty, gen_expr(node->cond));
    LLVMBuildCondBr(current_ir_builder, cond_val, loop_bb, end_bb);

    LLVMPositionBuilderAtEnd(current_ir_builder, end_bb);
    return NULL;
  }
  case ND_SWITCH: {
    // First count the number of cases we need to add to the switch instruction.
    // Don't count cases with large ranges. These will be checked separately.
    size_t num_cases = 0;
    for (Node *nd = node->case_next; nd; nd = nd->case_next) {
      if (nd->is_default) {
        continue;
      }
      long dist = nd->end + 1 - nd->begin;
      if (dist > 32) {
        num_cases += dist;
      }
    }

    // Add basic blocks that we will need to refer to.
    // Usually the 'else' block of the switch is the default case block.
    // However if there are GNU case ranges then we need to check those, and if they don't match
    // go to the default block.
    LLVMBasicBlockRef default_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMBasicBlockRef extra_cases_bb = LLVMAppendBasicBlock(current_ir_function, "");

    // Add switch instruction. Cases will be added later.
    LLVMValueRef cond_val = gen_expr(node->cond);
    LLVMValueRef switch_insn = LLVMBuildSwitch(
      current_ir_builder, cond_val, extra_cases_bb, num_cases
    );

    // Add a probably unreachable BB.
    LLVMBasicBlockRef unreachable_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMPositionBuilderAtEnd(current_ir_builder, unreachable_bb);

    LLVMBasicBlockRef prev_break_bb = current_break_bb;
    current_break_bb = end_bb;
    LLVMValueRef prev_switch_insn = current_switch_insn;
    current_switch_insn = switch_insn;
    LLVMBasicBlockRef prev_switch_default_bb = current_switch_default_bb;
    current_switch_default_bb = default_bb;
    LLVMBasicBlockRef prev_switch_extra_bb = current_switch_extra_bb;
    current_switch_extra_bb = extra_cases_bb;
    Type *prev_switch_cond_ty = current_switch_cond_ty;
    current_switch_cond_ty = node->cond->ty;
    LLVMValueRef prev_switch_cond_val = current_switch_cond_val;
    current_switch_cond_val = cond_val;

    // Generate code for switch body.
    // Includes case labels and all case bodies.
    gen_stmt(node->then);

    // If the last case does not end with a break, then it will not be terminated.
    if (!is_current_bb_terminated()) {
      LLVMBuildBr(current_ir_builder, end_bb);
    }

    // The switch body updates current_switch_extra_bb when it adds comparisons
    // for large case ranges. Need to add a branch to the default block if none of
    // the extra cases match.
    LLVMPositionBuilderAtEnd(current_ir_builder, current_switch_extra_bb);
    if (!is_current_bb_terminated()) {
      LLVMBuildBr(current_ir_builder, default_bb);
    }

    // If no explicit 'default:' case defined then the default case bb won't be terminated.
    LLVMPositionBuilderAtEnd(current_ir_builder, default_bb);
    if (!is_current_bb_terminated()) {
        LLVMBuildBr(current_ir_builder, end_bb);
    }

    current_break_bb = prev_break_bb;
    current_switch_insn = prev_switch_insn;
    current_switch_default_bb = prev_switch_default_bb;
    current_switch_extra_bb = prev_switch_extra_bb;
    current_switch_cond_ty = prev_switch_cond_ty;
    current_switch_cond_val = prev_switch_cond_val;
    LLVMPositionBuilderAtEnd(current_ir_builder, end_bb);

    return NULL;
  }
  case ND_CASE: {
    // TODO : Can we move the case BBs closer to the switch
    LLVMBasicBlockRef case_bb;
    if (node->is_default) {
      case_bb = current_switch_default_bb;
    } else {
      case_bb = LLVMAppendBasicBlock(current_ir_function, "");
    }
    LLVMBuildBr(current_ir_builder, case_bb);

    long dist = node->end + 1 - node->begin;
    LLVMTypeRef cond_ir_type = get_llvm_type(current_switch_cond_ty);
    if (!node->is_default) {
      if (dist <= 32) {
        for (long x = node->begin; x <= node->end; x++) {
          LLVMValueRef case_val = LLVMConstInt(cond_ir_type, x, false);
          LLVMAddCase(current_switch_insn, case_val, case_bb);
        }
      }
      else {
        // Large case range. Need to add check and branch to the 'extra cases' basic block.
        LLVMPositionBuilderAtEnd(current_ir_builder, current_switch_extra_bb);
        LLVMBasicBlockRef next_range_bb = LLVMAppendBasicBlock(current_ir_function, "");
        LLVMValueRef begin_val = LLVMConstInt(cond_ir_type, node->begin, false);
        LLVMValueRef end_val = LLVMConstInt(cond_ir_type, node->end, false);
        LLVMValueRef is_in_range_val;
        if (current_switch_cond_ty->is_unsigned) {
          is_in_range_val = LLVMBuildAnd(
            current_ir_builder,
            LLVMBuildICmp(current_ir_builder, LLVMIntUGE, current_switch_cond_val, begin_val, ""),
            LLVMBuildICmp(current_ir_builder, LLVMIntULE, current_switch_cond_val, end_val, ""),
            ""
          );
        } else {
          is_in_range_val = LLVMBuildAnd(
            current_ir_builder,
            LLVMBuildICmp(current_ir_builder, LLVMIntSGE, current_switch_cond_val, begin_val, ""),
            LLVMBuildICmp(current_ir_builder, LLVMIntSLE, current_switch_cond_val, end_val, ""),
            ""
          );
        }
        LLVMBuildCondBr(current_ir_builder, is_in_range_val, case_bb, next_range_bb);
        current_switch_extra_bb = next_range_bb;
      }
    }

    LLVMPositionBuilderAtEnd(current_ir_builder, case_bb);
    gen_stmt(node->lhs);
    return NULL;
  }
  case ND_BLOCK: {
    LLVMValueRef ret = NULL;
    for (Node *nd = node->body; nd; nd = nd->next) {
      ret = gen_stmt(nd);
    }
    return ret;
  }
  case ND_BREAK: {
    // Need to add a new BB just in case we write (unreachable) instructions after the
    // goto. LLVM blows up if there is anything after a terminating branch.
    LLVMBuildBr(current_ir_builder, current_break_bb);
    LLVMBasicBlockRef next_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMPositionBuilderAtEnd(current_ir_builder, next_bb);
    return NULL;
  }
  case ND_CONTINUE: {
    // Same as above, add a new BB after the branch.
    LLVMBuildBr(current_ir_builder, current_continue_bb);
    LLVMBasicBlockRef next_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMPositionBuilderAtEnd(current_ir_builder, next_bb);
    return NULL;
  }
  case ND_GOTO: {
    // The terminating branch instruction will be added to this basic block
    // after all labels have been processed.
    // Add a new basic block (will be unreachable unless it begins with a label).
    node->goto_bb = LLVMGetInsertBlock(current_ir_builder);
    LLVMBasicBlockRef next_bb = LLVMAppendBasicBlock(current_ir_function, "");
    LLVMPositionBuilderAtEnd(current_ir_builder, next_bb);
    return NULL;
  }
  case ND_GOTO_EXPR:
    // gen_expr(node->lhs);
    // println("  jmp *%%rax");
    error_tok(node->tok, "goto expr not supported");
    return NULL;
  case ND_LABEL: {
    // TODO : only append BB if current BB is not empty?
    LLVMBasicBlockRef label_bb = LLVMAppendBasicBlock(current_ir_function, "");
    node->goto_bb = label_bb;
    LLVMBuildBr(current_ir_builder, label_bb);
    LLVMPositionBuilderAtEnd(current_ir_builder, label_bb);
    gen_stmt(node->lhs);
    return NULL;
  }
  case ND_RETURN: {
    // Need to add a basic block in case something tries to generate code after the return.
    // It will most likely be unreachable.
    // XXX: This is a bad solution :(
    LLVMBasicBlockRef next_bb = LLVMAppendBasicBlock(current_ir_function, "");
    if (!node->lhs) {
      LLVMBuildRetVoid(current_ir_builder);
      LLVMPositionBuilderAtEnd(current_ir_builder, next_bb);
      return NULL;
    }
    Type *ty = node->lhs->ty;
    bool returns_struct = (ty->kind == TY_STRUCT) || (ty->kind == TY_UNION);
    bool returns_large_struct = returns_struct && (ty->size > 16);
    if (returns_large_struct) {
      // Large structure returns are written to the buffer pointed to by the first arg.
      // TODO : what should alignment be? Probably not 1.
      LLVMValueRef large_struct_ptr = gen_addr(node->lhs);
      LLVMBuildMemMove(
        current_ir_builder,
        LLVMGetFirstParam(current_ir_function), 1,
        large_struct_ptr, 1,
        LLVMConstInt(LLVMInt32Type(), ty->size, false)
      );
      LLVMBuildRetVoid(current_ir_builder);
      LLVMPositionBuilderAtEnd(current_ir_builder, next_bb);
      return NULL;
    }
    LLVMValueRef ret_val = gen_expr(node->lhs);
    if (returns_struct) {
      // Need an additional load for structs or unions, as gen_expr always returns a pointer.
      ret_val = LLVMBuildLoad2(current_ir_builder, get_llvm_type(node->lhs->ty), ret_val, "");
    }
    LLVMBuildRet(current_ir_builder, ret_val);
    LLVMPositionBuilderAtEnd(current_ir_builder, next_bb);
    return NULL;
  }
  case ND_EXPR_STMT:
    return gen_expr(node->lhs);
  case ND_ASM:
    error_tok(node->tok, "inline asm not supported");
    return NULL;
  }

  error_tok(node->tok, "invalid statement");
}

static LLVMTypeRef get_gvar_init_ir_type(GVarInitializer *ginit) {
  if (ginit->ir_type != NULL) {
    return ginit->ir_type;
  }
  if (ginit->ir_type_started) {
    error("recursive type");
  }
  ginit->ir_type_started = true;
  Type *ty = ginit->ty;
  switch (ty->kind) {
  case TY_VOID:
  case TY_BOOL:
  case TY_CHAR:
  case TY_SHORT:
  case TY_INT:
  case TY_LONG:
  case TY_FLOAT:
  case TY_DOUBLE:
  case TY_LDOUBLE:
  case TY_ENUM:
  case TY_PTR:
    ginit->ir_type = get_llvm_type(ty);
    break;
  case TY_ARRAY:
    // TODO : Check if type of all members is the same. If not then use vector.
    if (ginit->n_children > 0) {
      ginit->ir_type = LLVMArrayType(get_gvar_init_ir_type(&ginit->children[0]), ty->array_len);
    } else {
      ginit->ir_type = LLVMArrayType(get_llvm_type(ty->base), ty->array_len);
    }
    break;
  case TY_STRUCT: {
    LLVMTypeRef *elements = NULL;
    if (ginit->n_children > 0) {
      elements = calloc(ginit->n_children, sizeof(LLVMTypeRef));
      for (int i = 0; i < ginit->n_children; i++) {
        elements[i] = get_gvar_init_ir_type(&ginit->children[i]);
      }
    }
    ginit->ir_type = LLVMStructType(elements, ginit->n_children, ty->is_packed);
    break;
  }
  case TY_UNION: {
    int union_size = ty->size;
    int init_member_size = ginit->children[0].ty->size;
    if (init_member_size < union_size) {
      // Need to add padding if the init member is smaller than the largest
      // member of the union.
      LLVMTypeRef element = get_gvar_init_ir_type(&ginit->children[0]);
      LLVMTypeRef padding = LLVMArrayType(LLVMInt8Type(), union_size - init_member_size);
      LLVMTypeRef fields[2] = {element, padding};
      ginit->ir_type = LLVMStructType(fields, 2, ty->is_packed);
    }
    else if (init_member_size == union_size) {
      LLVMTypeRef element = get_gvar_init_ir_type(&ginit->children[0]);
      ginit->ir_type = LLVMStructType(&element, 1, ty->is_packed);
    }
    else {
      error("get_gvar_init_ir_type: union member size larger than union type size");
    }
    break;
  }
  default:
    error("get_gvar_init_ir_type: type kind %d unimplemented", ty->kind);
  }
  if (ginit->ir_type == NULL) {
    error("get_gvar_init_ir_type: unreachable");
  }
  return ginit->ir_type;
}

static LLVMValueRef get_gvar_init_val(GVarInitializer *ginit) {
  switch (ginit->ty->kind) {
  case TY_BOOL:
  case TY_CHAR:
  case TY_SHORT:
  case TY_INT:
  case TY_LONG:
  case TY_ENUM: {
    if (ginit->var) {
      if (ginit->ty->kind != TY_LONG) {
        // TODO : clang reports 'not a compile-time constant' if the type is not
        // large enough to hold a pointer. Check type sizes properly.
        error_tok(ginit->tok, "initializer element is not a compile-time constant");
      }
      if (!ginit->var->ir_val) {
        error_tok(ginit->tok, "bad ginit->var->ir_val for integer initializer");
      }
      LLVMValueRef ptr;
      if (ginit->val != 0) {
        LLVMValueRef index = LLVMConstInt(LLVMInt64Type(), ginit->val, true);
        // Offsets for constant expressions are always calculated in terms of bytes rather
        // than elements. Pretend the pointer is to i8.
        ptr = LLVMConstGEP2(LLVMInt8Type(), ginit->var->ir_val, &index, 1);
      } else {
        ptr = ginit->var->ir_val;
      }
      return LLVMConstPtrToInt(ptr, get_llvm_type(ginit->ty));
    }
    return LLVMConstInt(get_llvm_type(ginit->ty), ginit->val, false);
  }
  case TY_FLOAT:
  case TY_DOUBLE:
  case TY_LDOUBLE:
    return LLVMConstReal(get_llvm_type(ginit->ty), ginit->fval);
  case TY_PTR:
    if (ginit->var) {
      if (!ginit->var->ir_val) {
        error_tok(ginit->tok, "bad ginit->var->ir_val for pointer initializer");
      }
      if (ginit->val != 0) {
        LLVMValueRef index = LLVMConstInt(LLVMInt64Type(), ginit->val, true);
        // Offsets for constant expressions are always calculated in terms of bytes rather
        // than elements. Pretend the pointer is to i8.
        return LLVMConstGEP2(LLVMInt8Type(), ginit->var->ir_val, &index, 1);
      }
      return ginit->var->ir_val;
    }
    return LLVMConstIntToPtr(
      LLVMConstInt(LLVMInt64Type(), ginit->val, false),
      get_opaque_ptr_type()
    );
  case TY_ARRAY: {
    LLVMValueRef *vals = calloc(ginit->n_children, sizeof(LLVMValueRef));
    for (size_t i = 0; i < ginit->n_children; i++) {
      vals[i] = get_gvar_init_val(&ginit->children[i]);
    }

    // TODO : Will need to change to a vector if types of elements are different
    //   e.g. for array of unions with different variants for some elements.
    LLVMTypeRef base_ir_type;
    if (ginit->n_children > 0) {
      base_ir_type = get_gvar_init_ir_type(&ginit->children[0]);
    } else {
      base_ir_type = get_llvm_type(ginit->ty->base);
    }
    return LLVMConstArray(base_ir_type, vals, ginit->n_children);
  }
  case TY_UNION: {
    // Initializing unions is ugly. Bitcast doesn't work on aggregate types so can't just
    // bitcast the initialization val to the variable type.
    // Can initialize using any variant, and missing values need to be zero initialized.
    // Clang creates a global var with the type of the union variant being used to
    // initialize it, padded with i8 to the size of the largest variant.
    // If an array of unions is initialized we need to use the LLVM vector type so that
    // each element can have a different type, and be initialized with a different variant.
    //
    // E.g.
    //   union U0 { uint64_t a[2]; char b[4]; };
    //   union U0 g50[2] = {{.b[2]=0x12}, {.a=0x23}};
    //
    // Produces:
    //   %union.U0 = type { [2 x i64] }
    //   @g50 = dso_local global <{{ [4 x i8], [12 x i8] }, %union.U0 }>
    //     <{
    //       { [4 x i8], [12 x i8] } { [4 x i8] c"\00\00\12\00", [12 x i8] zeroinitializer },
    //       %union.U0 { [2 x i64] [i64 35, i64 0] }
    //     }>, align 16
    //
    int union_size = ginit->ty->size;
    int init_member_size = ginit->children[0].ty->size;
    LLVMValueRef val = get_gvar_init_val(&ginit->children[0]);
    if (init_member_size < union_size) {
      LLVMValueRef padding_val = LLVMConstNull(LLVMArrayType(
        LLVMInt8Type(), union_size - init_member_size
      ));
      LLVMValueRef vals[2] = {val, padding_val};
      return LLVMConstStruct(vals, 2, ginit->ty->is_packed);
    } else {
      return LLVMConstStruct(&val, 1, ginit->ty->is_packed);
    }
  }
  case TY_STRUCT: {
    LLVMValueRef *vals = calloc(ginit->n_children, sizeof(LLVMValueRef));
    for (size_t i = 0; i < ginit->n_children; i++) {
      vals[i] = get_gvar_init_val(&ginit->children[i]);
    }
    return LLVMConstNamedStruct(get_gvar_init_ir_type(ginit), vals, ginit->n_children);
  }
  default:
    error_tok(ginit->tok, "bad init data for type %d", ginit->ty->kind);
  }
}

static void emit_data(Obj *prog) {
  for (Obj *var = prog; var; var = var->next) {
    if (var->is_function) {
      if (!var->is_live) {
        continue;
      }
      var->ir_val = LLVMAddFunction(current_ir_module, var->name, get_llvm_type(var->ty));
      if (var->is_definition) {
        if (var->is_static) {
          // LLVMSetVisibility(ir_val, LLVMHiddenVisibility);
          LLVMSetLinkage(var->ir_val, LLVMPrivateLinkage);
        }
      }
      continue;
    }

    // If not a function, must be a global variable.
    LLVMTypeRef ir_type;
    if (var->init) {
      ir_type = get_gvar_init_ir_type(var->init);
    } else {
      ir_type = get_llvm_type(var->ty);
    }
    LLVMValueRef ir_val = LLVMAddGlobal(current_ir_module, ir_type, var->name);
    var->ir_val = ir_val;

    if (var->is_definition) {
      if (var->is_static) {
        // LLVMSetVisibility(ir_val, LLVMHiddenVisibility);
        LLVMSetLinkage(var->ir_val, LLVMPrivateLinkage);
      }
    }

    // TODO : is_const should be part of the type?
    if (var->is_const) {
      LLVMSetGlobalConstant(var->ir_val, true);
    }

    if (var->is_tls) {
      LLVMSetThreadLocal(var->ir_val, true);
    }
  }

  // Second pass, initialization data. Needs to be a separate pass as initialization data
  // can reference other global vars.
  for (Obj *var = prog; var; var = var->next) {
    if (var->is_function) {
      continue;
    }
  
    // For global vars is_definition is false if the var has the 'extern' attribute.
    if (!var->is_definition) {
      LLVMSetExternallyInitialized(var->ir_val, true);
      continue;
    }

    if (var->init) {
      // All other values.
      LLVMValueRef init_val = get_gvar_init_val(var->init);
      LLVMSetInitializer(var->ir_val, init_val);
    }
    else {
      // If no initializer specified then initialize with zero.
      LLVMSetInitializer(var->ir_val, get_zero_val(var->ty));
    }
  }

  // Declare LLVM builtins.
  LLVMTypeRef void_ptr_ty = get_opaque_ptr_type();
  LLVMTypeRef va_copy_arg_types[2] = {void_ptr_ty, void_ptr_ty}; 
  builtin_va_start_ir_type = LLVMFunctionType(LLVMVoidType(), &void_ptr_ty, 1, false);
  builtin_va_copy_ir_type = LLVMFunctionType(LLVMVoidType(), va_copy_arg_types, 2, false);
  builtin_va_end_ir_type = LLVMFunctionType(LLVMVoidType(), &void_ptr_ty, 1, false);

  builtin_va_start_ir_val = LLVMAddFunction(
    current_ir_module, "llvm.va_start.p0", builtin_va_start_ir_type
  );
  builtin_va_copy_ir_val = LLVMAddFunction(
    current_ir_module, "llvm.va_copy.p0", builtin_va_copy_ir_type
  );
  builtin_va_end_ir_val = LLVMAddFunction(
    current_ir_module, "llvm.va_end.p0", builtin_va_end_ir_type
  );
}

static void emit_text(Obj *prog) {
  for (Obj *fn = prog; fn; fn = fn->next) {
    if (!fn->is_function) {
      continue;
    }
  
    // No code is emitted for "static inline" functions
    // if no one is referencing them.
    if (!fn->is_live) {
      continue;
    }

    if (!fn->is_definition) {
      continue;
    }

    current_ir_function = fn->ir_val;

    // if (fn->ty->is_variadic) {
    //   error_tok(fn->tok, "code gen for variadic function not supported");
    // }

    LLVMBasicBlockRef entry_bb = LLVMAppendBasicBlock(current_ir_function, "");

    LLVMPositionBuilderAtEnd(current_ir_builder, entry_bb);

    // Add alloca instructions for all local variables (including params).
    for (Obj *var = fn->locals; var; var = var->next) {
      var->ir_val = LLVMBuildAlloca(current_ir_builder, get_llvm_type(var->ty), "");
    }

    // Copy all params to alloca buffers.
    // Some params and locals share the same node?
    // Hopefully will be optimised out?
    size_t param_count = 0;
    for (Obj *var = fn->params; var; var = var->next) {
      if (var->ir_val == NULL) {
        var->ir_val = LLVMBuildAlloca(current_ir_builder, get_llvm_type(var->ty), "");
      }
      store(var->ty, var->ir_val, LLVMGetParam(current_ir_function, param_count));
      param_count++;
    }

    // Emit function body.
    gen_stmt(fn->body);

    LLVMBasicBlockRef last_bb = LLVMGetInsertBlock(current_ir_builder);

    // Add branches to the end of goto basic blocks now that the basic blocks for
    // all labels have been created.
    for (Node *goto_nd = fn->gotos; goto_nd; goto_nd = goto_nd->goto_next) {
      for (Node *label_nd = fn->labels; label_nd; label_nd = label_nd->goto_next) {
        if (!strcmp(goto_nd->label, label_nd->label)) {
          LLVMPositionBuilderAtEnd(current_ir_builder, goto_nd->goto_bb);
          if (!is_current_bb_terminated()) {
            LLVMBuildBr(current_ir_builder, label_nd->goto_bb);
          }
        }
      }
    }

    LLVMPositionBuilderAtEnd(current_ir_builder, last_bb);

    // Need to add a return if there wasn't one.
    // TODO : remove the current BB rather than adding a return if it is unreachable.
    if (!is_current_bb_terminated()) {
      Type *return_ty = fn->ty->return_ty;
      bool returns_struct = (return_ty->kind == TY_STRUCT || return_ty->kind == TY_UNION);
      bool returns_large_struct = returns_struct && (return_ty->size > 16);
      if (strcmp(fn->name, "main") == 0) {
        // [https://www.sigbus.info/n1570#5.1.2.2.3p1] The C spec defines
        // a special rule for the main function. Reaching the end of the
        // main function is equivalent to returning 0, even though the
        // behavior is undefined for the other functions.
        LLVMBuildRet(current_ir_builder, get_zero_val(return_ty));
      }
      else if (fn->is_noreturn) {
        LLVMBuildUnreachable(current_ir_builder);
      }
      else if (fn->ty->return_ty->kind == TY_VOID) {
        LLVMBuildRetVoid(current_ir_builder);
      }
      else if (returns_large_struct) {
        LLVMBuildRetVoid(current_ir_builder);
      }
      else {
        // Add an alloca, then load and return the uninitialized value.
        // Mirrors clang behaviour.
        LLVMBasicBlockRef current_bb = LLVMGetInsertBlock(current_ir_builder);
        LLVMPositionBuilder(current_ir_builder, entry_bb, LLVMGetFirstInstruction(entry_bb));
        LLVMValueRef ret_alloca = LLVMBuildAlloca(
          current_ir_builder, get_llvm_type(fn->ty->return_ty), ""
        );
        LLVMPositionBuilderAtEnd(current_ir_builder, current_bb);
        LLVMValueRef ret_val = LLVMBuildLoad2(
          current_ir_builder, get_llvm_type(fn->ty->return_ty), ret_alloca, ""
        );
        LLVMBuildRet(current_ir_builder, ret_val);
      }
    }

    current_ir_function = NULL;
  }
}

void handle_llvm_fatal_error(const char *reason) {
  fprintf(stderr, "codegen: LLVM Fatal Error: %s\n", reason);
}

void codegen(Obj *prog, CodeGenOutputType out_type, FILE *out) {
  LLVMInstallFatalErrorHandler(handle_llvm_fatal_error);
  LLVMEnablePrettyStackTrace();

  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();

  // Get default target triple for the host.
  const char *target_triple = LLVMGetDefaultTargetTriple();
  
  // Get target from the host triple.
  LLVMTargetRef target = NULL;
  char *err_msg = NULL;
  if (LLVMGetTargetFromTriple(target_triple, &target, &err_msg)) {
    // LLVMDisposeMessage(err_msg);
    error("codegen: failed to get target from triple %s: %s", target_triple, err_msg);
  }

  LLVMTargetMachineOptionsRef opts = LLVMCreateTargetMachineOptions();
  LLVMTargetMachineOptionsSetCPU(opts, LLVMGetHostCPUName());
  LLVMTargetMachineOptionsSetFeatures(opts, LLVMGetHostCPUFeatures());
  LLVMTargetMachineOptionsSetCodeGenOptLevel(opts, LLVMCodeGenLevelNone);
  // LLVMTargetMachineOptionsSetCodeGenOptLevel(opts, LLVMCodeGenLevelDefault);
  LLVMTargetMachineOptionsSetCodeModel(opts, LLVMCodeModelDefault);
  // LLVMTargetMachineOptionsSetCodeModel(opts, LLVMCodeModelJITDefault);
  LLVMTargetMachineOptionsSetRelocMode(opts, LLVMRelocPIC);

  // Create a target machine from the target and triple.
  LLVMTargetMachineRef machine = LLVMCreateTargetMachineWithOptions(target, target_triple, opts);

  // fprintf(stderr, "target triple: %s\n", target_triple);
  // fprintf(stderr, "target cpu: %s\n", LLVMGetTargetMachineCPU(machine));
  // fprintf(stderr, "target features: %s\n", LLVMGetTargetMachineFeatureString(machine));

  // Get data layout based on the target machine.
  LLVMTargetDataRef layout = LLVMCreateTargetDataLayout(machine);

  // char *layout_string = LLVMCopyStringRepOfTargetData(layout);

  // fprintf(stderr, "target layout: %s\n", layout_string);

  // int pointer_size = LLVMPointerSize(layout);

  // fprintf(stderr, "target pointer size: %d\n", pointer_size);

  // TODO : use pointer size for pointer conversions!

  // TODO : don't use global context, would need to replace a lot of LLVM calls with the
  //     *InContext variants though.
  // current_ir_context = LLVMContextCreate();
  // current_ir_module = LLVMModuleCreateWithNameInContext("", current_ir_context);

  current_ir_context = LLVMGetGlobalContext();
  current_ir_module = LLVMModuleCreateWithNameInContext("", current_ir_context);
  current_ir_builder = LLVMCreateBuilder();

  LLVMSetTarget(current_ir_module, target_triple);
  LLVMSetModuleDataLayout(current_ir_module, layout);

  emit_data(prog);
  emit_text(prog);

  err_msg = NULL;
  if (LLVMVerifyModule(current_ir_module, LLVMPrintMessageAction, &err_msg)) {
    // LLVMDisposeMessage(err_msg);
    error("codegen: verify module error:\n%s", err_msg);
  }

  if (out_type == CODEGEN_OUTPUT_LLVM) {
    char *ir_text = LLVMPrintModuleToString(current_ir_module);
    fputs(ir_text, out);
    LLVMDisposeMessage(ir_text);
  }
  else if (out_type == CODEGEN_OUTPUT_OBJECT || out_type == CODEGEN_OUTPUT_ASSEMBLY) {
    LLVMCodeGenFileType llvm_out_type;
    if (out_type == CODEGEN_OUTPUT_OBJECT) {
      llvm_out_type = LLVMObjectFile;
    } else {
      llvm_out_type = LLVMAssemblyFile;
    }

    LLVMMemoryBufferRef out_buf = NULL;
    err_msg = NULL;
    if (LLVMTargetMachineEmitToMemoryBuffer(
      machine, current_ir_module, llvm_out_type, &err_msg, &out_buf
    )) {
      // LLVMDisposeMessage(err_msg);
      error("codegen: failed to compile to memory buffer: %s", err_msg);
    }

    fwrite(LLVMGetBufferStart(out_buf), 1, LLVMGetBufferSize(out_buf), out);
    LLVMDisposeMemoryBuffer(out_buf);
  }
  else {
    error("codegen: bad output type");
  }

  LLVMDisposeBuilder(current_ir_builder);
  LLVMDisposeModule(current_ir_module);
  LLVMDisposeTargetData(layout);
  LLVMDisposeTargetMachine(machine);
  LLVMDisposeTargetMachineOptions(opts);
  current_ir_builder = NULL;
  current_ir_module = NULL;
  current_ir_context = NULL;
}
