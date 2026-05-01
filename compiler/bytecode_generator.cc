// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/bytecode_generator.h"

#include <cstdint>
#include <ostream>
#include <utility>
#include <variant>
#include <vector>

#include "compiler/assembler.h"
#include "compiler/logging.h"
#include "compiler/program_builder.h"
#include "compiler/tokenizer.h"
#include "compiler/type_context.h"
#include "compiler/types.h"
#include "src/types.h"
#include "src/vm.h"

namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

}  // namespace

ByteCodeGenerator::ByteCodeGenerator(ProgramBuilder& program_builder,
                                     const TypeContext& type_context)
    : builder_(program_builder), type_context_(type_context) {}

void ByteCodeGenerator::EmitBlock(const Block& root) {
  EmitBlock(root, std::nullopt);
}

void ByteCodeGenerator::EmitBlock(const Block& block,
                                  std::optional<LoopContext> loop_ctx) {
  for (const auto& stmt : block.statements) {
    std::visit(
        Overloaded{
            [&](const std::unique_ptr<Expression>& expr) {
              EmitExpression(expr);

              // Expressions always leave a value on the stack (if not Void) so
              // when executed as a Statement the unused value must be consumed.
              if (expr->type != TypeContext::LiteralType::Void)
                builder_.GetCurrentCode().StackDel();
            },
            [&](const FunctionDeclaration& fn) {
              if (fn.body) {
                CHECK(fn.resolved->variables_to_capture.empty())
                    << "Captures found for function.";

                EmitFunctionBlock(fn);
              }
            },
            [&](const ReturnStatement& ret) {
              EmitExpression(ret.value);
              builder_.GetCurrentCode().Return();
            },
            [&](const ThrowStatement& thr) {
              EmitExpression(thr.value);
              builder_.GetCurrentCode().Throw();
            },
            [&](const IfStatement& if_stmt) {
              EmitExpression(if_stmt.condition);
              std::string id = std::to_string(next_unique_id_++);
              builder_.GetCurrentCode().JumpIfFalse("else" + id);
              EmitBlock(if_stmt.then_body, loop_ctx);
              builder_.GetCurrentCode().Jump("end_if" + id);
              builder_.GetCurrentCode().Label("else" + id);
              EmitBlock(if_stmt.else_body, loop_ctx);
              builder_.GetCurrentCode().Label("end_if" + id);
            },
            [&](const WhileStatement& while_stmt) {
              std::string id = std::to_string(next_unique_id_++);
              std::string condition_label = "while_cond" + id;
              std::string end_label = "while_end" + id;

              builder_.GetCurrentCode().Label(condition_label);
              EmitExpression(while_stmt.condition);
              builder_.GetCurrentCode().JumpIfFalse(end_label);
              EmitBlock(while_stmt.body,
                        LoopContext{end_label, condition_label});
              builder_.GetCurrentCode().Jump(condition_label);
              builder_.GetCurrentCode().Label(end_label);
            },
            [&](const BreakStatement&) {
              CHECK(loop_ctx) << "break statement not within a loop";
              builder_.GetCurrentCode().Jump(loop_ctx->break_label);
            },
            [&](const ContinueStatement&) {
              CHECK(loop_ctx) << "continue statement not within a loop";
              builder_.GetCurrentCode().Jump(loop_ctx->continue_label);
            },
            [&](const AssignStatement& assign) {
              EmitExpression(assign.value);
              CHECK(assign.resolved)
                  << "Unresolved variable in assignment: " << assign.name;

              builder_.StoreSymbol(assign.resolved->symbol);
            },
            [&](const StructDeclaration& struct_decl) {
              for (const auto& method : struct_decl.methods) {
                // Assume that a function missing a body is valid by the time
                // we generate ByteCode (i.e. struct is extern, etc) and skip.
                if (!method.second.body)
                  continue;

                CHECK(method.second.resolved)
                    << "Unresolved method: " << method.first;
                CHECK(method.second.resolved->variables_to_capture.empty())
                    << "Captures found for method: " << method.first;

                EmitFunctionBlock(method.second,
                                  struct_decl.name + "_" + method.first);
              }
            },
            [&](ImportStatement& import) { /*nothing to compile*/ },
        },
        stmt->as);
  }
}

void ByteCodeGenerator::EmitFunctionBlock(
    const FunctionDeclaration& fn,
    std::optional<std::string> override_name) {
  builder_.EnterFunctionScope(
      override_name.value_or(fn.name), fn.resolved->function_symbol.idx.value(),
      fn.resolved->arguments, fn.resolved->capture_arguments);
  EmitBlock(*fn.body);

  const auto& type = type_context_.GetTypeInfo<FunctionType>(
      fn.resolved->function_symbol.type_id);

  // Insert an return; to unwind the stack
  if (type->return_type == TypeContext::LiteralType::Void)
    builder_.GetCurrentCode().Return();
  builder_.ExitFunctionScope();
}

void ByteCodeGenerator::EmitExpression(
    const std::unique_ptr<Expression>& expr,
    std::optional<OptionalChainContext> optional_chain_ctx,
    AccessMode access_mode) {
  if (expr == nullptr) {
    LOG(ERROR) << "Failed to compile null expression";
    return;
  }

  // builder.GetCurrentCode().DebugString(
  //     std::to_string(expr->meta.line_range.start) + " -> " +
  //     std::to_string(expr->meta.line_range.end));

  std::visit(
      Overloaded{
          [&](const PrimaryExpression& primary) {
            std::visit(
                Overloaded{
                    [&](const StringLiteral& str) {
                      builder_.GetCurrentCode().PushConstRef(
                          builder_.GetIdForConstant(str.value));
                    },
                    [&](const Identifier& ident) {
                      CHECK(ident.resolved)
                          << "Unresolved identifier: " << ident.name;

                      switch (ident.resolved->symbol.kind) {
                        case Symbol::Function:
                          CHECK(ident.resolved->symbol.idx.has_value())
                              << "Function symbol missing index: "
                              << ident.name;
                          builder_.GetCurrentCode().Bind(
                              ident.resolved->symbol.idx.value(),
                              /*argc=*/0);
                          break;
                        case Symbol::Variable:
                        case Symbol::Capture:
                        case Symbol::Narrowed: {
                          builder_.PushSymbol(ident.resolved->symbol);
                          break;
                        }
                        default:
                          LOG(ERROR) << "Unsupported identifier kind: "
                                     << ident.resolved->symbol.kind;
                          break;
                      }
                    },
                    [&](int32_t i32) {
                      builder_.GetCurrentCode().PushInt32(i32);
                    },
                    [&](float f32) {
                      builder_.GetCurrentCode().PushFloat(f32);
                    },
                    [&](const CodepointLiteral& codepoint) {
                      builder_.GetCurrentCode().PushInt32(codepoint.value);
                    },
                    [&](bool b) { builder_.GetCurrentCode().PushBool(b); },
                    [&](Nil) { builder_.GetCurrentCode().PushNil(); }},
                primary.value);
          },
          [&](const BinaryExpression& binary) {
            CHECK(binary.resolved) << "Unresolved binary expression";

            if (binary.resolved->specialization ==
                ResolvedBinary::Specialization::Nil) {
              const std::unique_ptr<Expression>& target =
                  binary.lhs->type == TypeContext::LiteralType::Nil
                      ? binary.rhs
                      : binary.lhs;
              EmitExpression(target);
              switch (binary.op) {
                case TokenKind::kCompareEq:
                  builder_.GetCurrentCode().Is(value_type_t::VALUE_TYPE_NULL);
                  break;
                case TokenKind::kCompareNe:
                  builder_.GetCurrentCode().Is(value_type_t::VALUE_TYPE_NULL);
                  builder_.GetCurrentCode().Not();
                  break;
                default:
                  NOTREACHED()
                      << "only equality comparisons allowed against Nil";
                  break;
              }
            } else {
              EmitExpression(binary.lhs);
              EmitExpression(binary.rhs);
              EmitOp(Token{.kind = binary.op},
                     binary.resolved->specialization ==
                         ResolvedBinary::Specialization::String);
            }
          },
          [&](const CallExpression& call) {
            EmitCall(call, optional_chain_ctx);
          },
          [&](const AssignmentExpression& assign) {
            if (std::holds_alternative<PrimaryExpression>(assign.lhs->as)) {
              if (const auto* identifier = std::get_if<Identifier>(
                      &std::get<PrimaryExpression>(assign.lhs->as).value)) {
                EmitExpression(assign.rhs);
                builder_.GetCurrentCode().StackDup();

                CHECK(identifier->resolved)
                    << "Unresolved identifier on LHS of assignment: "
                    << identifier->name;

                builder_.StoreSymbol(identifier->resolved->symbol);
              } else {
                LOG(FATAL) << "Assigning to literal type";
              }
            } else if (std::holds_alternative<ArrayAccessExpression>(
                           assign.lhs->as) ||
                       std::holds_alternative<MemberAccessExpression>(
                           assign.lhs->as) ||
                       std::holds_alternative<OptionalChainExpression>(
                           assign.lhs->as)) {
              // AssignmentExpression should not occur within an optional chain.
              EmitExpression(assign.lhs, /*optional_chain_ctx=*/std::nullopt,
                             AccessMode::STORE);
              EmitExpression(assign.rhs);

              builder_.GetCurrentCode().Call(VM_BUILTIN_ARRAY_SET, 3);
            } else {
              LOG(FATAL) << "Unsupported LHS for AssignmentExpression: "
                         << assign.lhs->as.index();
            }
          },
          [&](const MemberAccessExpression& member_access) {
            CHECK(member_access.resolved)
                << "Unresolved member access: " << member_access.member_name
                << " at line: " << expr->meta.line_range.start;

            EmitExpression(member_access.object, optional_chain_ctx);

            if (access_mode == AccessMode::LOAD ||
                access_mode == AccessMode::STORE) {
              MemberIdx idx = member_access.resolved->index;
              builder_.GetCurrentCode().PushInt32(idx);
            }
            if (access_mode == AccessMode::LOAD)
              builder_.GetCurrentCode().Call(VM_BUILTIN_ARRAY_GET, 2);
          },
          [&](const ArrayAccessExpression& array_access) {
            EmitExpression(array_access.array, optional_chain_ctx);
            EmitExpression(array_access.index);

            if (access_mode == AccessMode::LOAD)
              builder_.GetCurrentCode().Call(VM_BUILTIN_ARRAY_GET, 2);
          },
          [&](const LogicExpression& logic) {
            EmitExpression(logic.lhs);

            std::string id = std::to_string(next_unique_id_++);
            std::string lhs_label = "logic_lhs_" + id;
            std::string end_label = "logic_end_" + id;

            // Handle short-circuiting to skip evaluating the RHS if possible.
            if (logic.kind == LogicExpression::AND) {
              builder_.GetCurrentCode().JumpIfFalse(lhs_label);
            } else {
              builder_.GetCurrentCode().JumpIfTrue(lhs_label);
            }

            EmitExpression(logic.rhs);
            builder_.GetCurrentCode().Jump(end_label);

            // Short-circuiting consumes the LHS value on the stack, so we need
            // to push the correct value for the result of the logic expression.
            builder_.GetCurrentCode().Label(lhs_label);
            if (logic.kind == LogicExpression::AND) {
              builder_.GetCurrentCode().PushBool(false);
            } else {
              builder_.GetCurrentCode().PushBool(true);
            }

            builder_.GetCurrentCode().Label(end_label);
          },
          [&](const ClosureExpression& closure) {
            CHECK(closure.fn.resolved) << "Unresolved function!";
            EmitFunctionBlock(closure.fn);

            for (const auto& capture :
                 closure.fn.resolved->variables_to_capture) {
              builder_.PushSymbol(capture);
            }
            builder_.GetCurrentCode().Bind(
                closure.fn.resolved->function_symbol.idx.value(),
                /*argc=*/closure.fn.resolved->variables_to_capture.size());
          },
          [&](PrefixUnaryExpression& prefix) {
            EmitExpression(prefix.operand);
            switch (prefix.op) {
              case TokenKind::kPlus:  // no-op
                break;
              case TokenKind::kMinus:
                // TODO: Add OP_NEGATE?
                builder_.GetCurrentCode().PushInt32(-1);
                builder_.GetCurrentCode().Multiply();
                break;
              case TokenKind::kPlusPlus:
              case TokenKind::kMinusMinus:
                LOG(FATAL) << "Prefix ++/-- are not yet supported!";
                break;
              case TokenKind::kNot:
                builder_.GetCurrentCode().Not();
                break;
              default:
                LOG(FATAL) << "Unsupported TokenKind op: " << prefix.op;
                break;
            }
          },
          [&](PostfixUnaryExpression& postfix) {
            LOG(FATAL) << "PostFix are not yet supported!";
          },
          [&](TypeCastExpression& cast) { EmitExpression(cast.expr); },
          [&](OptionalChainExpression& optional_chain) {
            std::string null_label =
                "null_case_" + std::to_string(next_unique_id_++);
            EmitExpression(optional_chain.root,
                           OptionalChainContext{null_label}, access_mode);
            builder_.GetCurrentCode().Label(null_label);
          },
          [&](NilCoalescingExpression& coalescing) {
            std::string not_null_label =
                "coalesce_" + std::to_string(next_unique_id_++);

            EmitExpression(coalescing.lhs);
            builder_.GetCurrentCode().StackDup();
            builder_.GetCurrentCode().Is(value_type_t::VALUE_TYPE_NULL);
            builder_.GetCurrentCode().JumpIfFalse(not_null_label);

            EmitExpression(coalescing.rhs);

            builder_.GetCurrentCode().Label(not_null_label);
          },
          [&](OptionalAccessExpression& optional_access) {
            EmitExpression(optional_access.target, optional_chain_ctx,
                           access_mode);
            CHECK(optional_chain_ctx.has_value())
                << "Missing OptionalChainContext for OptionalAccessExpression";
            builder_.GetCurrentCode().StackDup();
            builder_.GetCurrentCode().Is(value_type_t::VALUE_TYPE_NULL);
            builder_.GetCurrentCode().JumpIfTrue(
                optional_chain_ctx->null_label);
          }},
      expr->as);
}

void ByteCodeGenerator::EmitCall(
    const CallExpression& call,
    std::optional<OptionalChainContext> optional_chain_ctx) {
  if (call.resolved) {
    switch (call.resolved->kind) {
      case Free:
      case Extern:
      case StaticMethod: {
        for (const auto& argument : call.arguments)
          EmitExpression(argument);
        builder_.GetCurrentCode().Call(call.resolved->function_idx,
                                       call.arguments.size());
        break;
      }
      case Anonymous: {
        for (const auto& argument : call.arguments)
          EmitExpression(argument);
        EmitExpression(call.callee);
        builder_.GetCurrentCode().CallDynamic(call.arguments.size());
        break;
      }
      case Method: {
        EmitExpression(call.callee, optional_chain_ctx,
                       AccessMode::OBJECT_ONLY);
        for (const auto& argument : call.arguments)
          EmitExpression(argument);

        builder_.GetCurrentCode().Call(call.resolved->function_idx,
                                       call.arguments.size() + 1);
        break;
      }
      case Constructor: {
        for (const auto& argument : call.arguments)
          EmitExpression(argument);
        builder_.GetCurrentCode().Call(VM_BUILTIN_ARRAY_INIT,
                                       call.arguments.size());
        break;
      }
    }
  }
}

void ByteCodeGenerator::EmitOp(const Token& op, bool is_string) {
  switch (op.kind) {
    case TokenKind::kPlus:
      if (is_string)
        builder_.GetCurrentCode().Concat();
      else
        builder_.GetCurrentCode().Add();
      break;
    case TokenKind::kMinus:
      builder_.GetCurrentCode().Subtract();
      break;
    case TokenKind::kMultiply:
      builder_.GetCurrentCode().Multiply();
      break;
    case TokenKind::kDivide:
      builder_.GetCurrentCode().Divide();
      break;
    case TokenKind::kCompareEq:
      builder_.GetCurrentCode().Compare(OP_EQUAL);
      break;
    case TokenKind::kCompareNe:
      builder_.GetCurrentCode().Compare(OP_EQUAL);
      builder_.GetCurrentCode().Not();
      break;
    case TokenKind::kCompareGt:
      builder_.GetCurrentCode().Compare(OP_GREATER_THAN);
      break;
    case TokenKind::kCompareGe:
      builder_.GetCurrentCode().Compare(OP_GREAT_OR_EQ);
      break;
    case TokenKind::kCompareLt:
      builder_.GetCurrentCode().Compare(OP_LESS_THAN);
      break;
    case TokenKind::kCompareLe:
      builder_.GetCurrentCode().Compare(OP_LESS_OR_EQ);
      break;
    default:
      LOG(ERROR) << "unsupported binary operator: " << op.kind;
      return;
  }
}