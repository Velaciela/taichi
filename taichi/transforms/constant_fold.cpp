#include <cmath>
#include <deque>
#include <set>
#include <thread>

#include "taichi/ir/ir.h"
#include "taichi/ir/snode.h"
#include "taichi/ir/statements.h"
#include "taichi/ir/transforms.h"
#include "taichi/ir/visitors.h"
#include "taichi/transforms/constant_fold.h"
#include "taichi/program/program.h"

TLANG_NAMESPACE_BEGIN

class ConstantFold : public BasicStmtVisitor {
 public:
  using BasicStmtVisitor::visit;
  DelayedIRModifier modifier;
  Program *program;

  explicit ConstantFold(Program *program)
      : BasicStmtVisitor(), program(program) {
  }

  Kernel *get_jit_evaluator_kernel(JITEvaluatorId const &id) {
    auto &cache = program->jit_evaluator_cache;
    // Discussion:
    // https://github.com/taichi-dev/taichi/pull/954#discussion_r423442606
    std::lock_guard<std::mutex> _(program->jit_evaluator_cache_mut);
    auto it = cache.find(id);
    if (it != cache.end())  // cached?
      return it->second.get();

    auto kernel_name = fmt::format("jit_evaluator_{}", cache.size());
    auto func = [&id]() {
      auto lhstmt =
          Stmt::make<ArgLoadStmt>(/*arg_id=*/0, id.lhs, /*is_ptr=*/false);
      auto rhstmt =
          Stmt::make<ArgLoadStmt>(/*arg_id=*/1, id.rhs, /*is_ptr=*/false);
      pStmt oper;
      if (id.is_binary) {
        oper = Stmt::make<BinaryOpStmt>(id.binary_op(), lhstmt.get(),
                                        rhstmt.get());
      } else {
        oper = Stmt::make<UnaryOpStmt>(id.unary_op(), lhstmt.get());
        if (unary_op_is_cast(id.unary_op())) {
          oper->cast<UnaryOpStmt>()->cast_type = id.rhs;
        }
      }
      auto ret = Stmt::make<ReturnStmt>(oper.get());
      current_ast_builder().insert(std::move(lhstmt));
      if (id.is_binary)
        current_ast_builder().insert(std::move(rhstmt));
      current_ast_builder().insert(std::move(oper));
      current_ast_builder().insert(std::move(ret));
    };

    auto ker = std::make_unique<Kernel>(*program, func, kernel_name);
    ker->insert_ret(id.ret);
    ker->insert_arg(id.lhs, false);
    if (id.is_binary)
      ker->insert_arg(id.rhs, false);
    ker->is_evaluator = true;

    auto *ker_ptr = ker.get();
    TI_TRACE("Saving JIT evaluator cache entry id={}",
             std::hash<JITEvaluatorId>{}(id));
    cache[id] = std::move(ker);

    return ker_ptr;
  }

  static bool is_good_type(DataType dt) {
    // ConstStmt of `bad` types like `i8` is not supported by LLVM.
    // Discussion:
    // https://github.com/taichi-dev/taichi/pull/839#issuecomment-625902727
    if (dt->is_primitive(PrimitiveTypeID::i32) ||
        dt->is_primitive(PrimitiveTypeID::i64) ||
        dt->is_primitive(PrimitiveTypeID::u32) ||
        dt->is_primitive(PrimitiveTypeID::u64) ||
        dt->is_primitive(PrimitiveTypeID::f32) ||
        dt->is_primitive(PrimitiveTypeID::f64))
      return true;
    else
      return false;
  }

  bool jit_evaluate_binary_op(TypedConstant &ret,
                              BinaryOpStmt *stmt,
                              const TypedConstant &lhs,
                              const TypedConstant &rhs) {
    if (!is_good_type(ret.dt))
      return false;
    JITEvaluatorId id{std::this_thread::get_id(),
                      (int)stmt->op_type,
                      ret.dt,
                      lhs.dt,
                      rhs.dt,
                      true};
    auto *ker = get_jit_evaluator_kernel(id);
    auto launch_ctx = ker->make_launch_context();
    launch_ctx.set_arg_raw(0, lhs.val_u64);
    launch_ctx.set_arg_raw(1, rhs.val_u64);
    {
      std::lock_guard<std::mutex> _(program->jit_evaluator_cache_mut);
      (*ker)(launch_ctx);
      ret.val_i64 = program->fetch_result<int64_t>(0);
    }
    return true;
  }

  bool jit_evaluate_unary_op(TypedConstant &ret,
                             UnaryOpStmt *stmt,
                             const TypedConstant &operand) {
    if (!is_good_type(ret.dt))
      return false;
    JITEvaluatorId id{std::this_thread::get_id(),
                      (int)stmt->op_type,
                      ret.dt,
                      operand.dt,
                      stmt->cast_type,
                      false};
    auto *ker = get_jit_evaluator_kernel(id);
    auto launch_ctx = ker->make_launch_context();
    launch_ctx.set_arg_raw(0, operand.val_u64);
    {
      std::lock_guard<std::mutex> _(program->jit_evaluator_cache_mut);
      (*ker)(launch_ctx);
      ret.val_i64 = program->fetch_result<int64_t>(0);
    }
    return true;
  }

  void visit(BinaryOpStmt *stmt) override {
    auto lhs = stmt->lhs->cast<ConstStmt>();
    auto rhs = stmt->rhs->cast<ConstStmt>();
    if (!lhs || !rhs)
      return;
    if (stmt->width() != 1)
      return;
    auto dst_type = stmt->ret_type;
    TypedConstant new_constant(dst_type);
    if (jit_evaluate_binary_op(new_constant, stmt, lhs->val[0], rhs->val[0])) {
      auto evaluated =
          Stmt::make<ConstStmt>(LaneAttribute<TypedConstant>(new_constant));
      stmt->replace_usages_with(evaluated.get());
      modifier.insert_before(stmt, std::move(evaluated));
      modifier.erase(stmt);
    }
  }

  void visit(UnaryOpStmt *stmt) override {
    if (stmt->is_cast() && stmt->cast_type == stmt->operand->ret_type) {
      stmt->replace_usages_with(stmt->operand);
      modifier.erase(stmt);
      return;
    }
    auto operand = stmt->operand->cast<ConstStmt>();
    if (!operand)
      return;
    if (stmt->width() != 1) {
      return;
    }
    if (stmt->is_cast()) {
      bool cast_available = true;
      TypedConstant new_constant(stmt->ret_type);
      auto operand = stmt->operand->cast<ConstStmt>();
      if (stmt->op_type == UnaryOpType::cast_bits) {
        new_constant.value_bits = operand->val[0].value_bits;
      } else {
        if (stmt->cast_type == PrimitiveType::f32) {
          new_constant.val_f32 = float32(operand->val[0].val_cast_to_float64());
        } else if (stmt->cast_type == PrimitiveType::f64) {
          new_constant.val_f64 = operand->val[0].val_cast_to_float64();
        } else {
          cast_available = false;
        }
      }
      if (cast_available) {
        auto evaluated =
            Stmt::make<ConstStmt>(LaneAttribute<TypedConstant>(new_constant));
        stmt->replace_usages_with(evaluated.get());
        modifier.insert_before(stmt, std::move(evaluated));
        modifier.erase(stmt);
        return;
      }
    }
    auto dst_type = stmt->ret_type;
    TypedConstant new_constant(dst_type);
    if (jit_evaluate_unary_op(new_constant, stmt, operand->val[0])) {
      auto evaluated =
          Stmt::make<ConstStmt>(LaneAttribute<TypedConstant>(new_constant));
      stmt->replace_usages_with(evaluated.get());
      modifier.insert_before(stmt, std::move(evaluated));
      modifier.erase(stmt);
    }
  }

  void visit(BitExtractStmt *stmt) override {
    auto input = stmt->input->cast<ConstStmt>();
    if (!input)
      return;
    if (stmt->width() != 1)
      return;
    std::unique_ptr<Stmt> result_stmt;
    if (is_signed(input->val[0].dt)) {
      auto result = (input->val[0].val_int() >> stmt->bit_begin) &
                    ((1LL << (stmt->bit_end - stmt->bit_begin)) - 1);
      result_stmt = Stmt::make<ConstStmt>(LaneAttribute<TypedConstant>(
          TypedConstant(input->val[0].dt, result)));
    } else {
      auto result = (input->val[0].val_uint() >> stmt->bit_begin) &
                    ((1LL << (stmt->bit_end - stmt->bit_begin)) - 1);
      result_stmt = Stmt::make<ConstStmt>(LaneAttribute<TypedConstant>(
          TypedConstant(input->val[0].dt, result)));
    }
    stmt->replace_usages_with(result_stmt.get());
    modifier.insert_before(stmt, std::move(result_stmt));
    modifier.erase(stmt);
  }

  static bool run(IRNode *node, Program *program) {
    ConstantFold folder(program);
    bool modified = false;

    auto program_compile_config_org = program->config;
    program->config.advanced_optimization = false;
    program->config.constant_folding = false;
    program->config.external_optimization_level = 0;

    while (true) {
      node->accept(&folder);
      if (folder.modifier.modify_ir()) {
        modified = true;
      } else {
        break;
      }
    }

    program->config = program_compile_config_org;

    return modified;
  }
};

const PassID ConstantFoldPass::id = "ConstantFoldPass";

namespace irpass {

bool constant_fold(IRNode *root,
                   const CompileConfig &config,
                   const ConstantFoldPass::Args &args) {
  TI_AUTO_PROF;
  // @archibate found that `debug=True` will cause JIT kernels
  // to evaluate incorrectly (always return 0), so we simply
  // disable constant_fold when config.debug is turned on.
  // Discussion:
  // https://github.com/taichi-dev/taichi/pull/839#issuecomment-626107010
  if (config.debug) {
    TI_TRACE("config.debug enabled, ignoring constant fold");
    return false;
  }
  if (!config.advanced_optimization)
    return false;
  return ConstantFold::run(root, args.program);
}

}  // namespace irpass

TLANG_NAMESPACE_END
