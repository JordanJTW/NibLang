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
#include "compiler/printer.h"
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

ByteCodeGenerator::ByteCodeGenerator(const TypeContext& type_context,
                                     const ScopeManager& scope_manager,
                                     ConstantPool& constant_pool)
    : type_context_(type_context),
      scope_manager_(scope_manager),
      constant_pool_(constant_pool) {}

ByteCodeGenerator::FunctionObject ByteCodeGenerator::Build(
    const FunctionSymbol& symbol,
    std::vector<SymbolId>& called_symbols) && {
  size_t argument_count = 0;

  const auto& [key, instance] = *symbol.instances.begin();

  // Process captures first to reflect their position in an OP_BIND.
  for (const auto& binding :
       scope_manager_.GetBindingsForScope(instance.scope_id)) {
    if (binding.kind == NamedBinding::Kind::Capture) {
      symbol_to_local_idx_[binding.idx.value()] = next_local_idx_++;
      argument_count++;
    }
  }

  for (const auto& binding :
       scope_manager_.GetBindingsForScope(instance.scope_id)) {
    if (binding.kind == NamedBinding::Kind::Argument) {
      symbol_to_local_idx_[binding.idx.value()] = next_local_idx_++;
      argument_count++;
    }
  }

  if (symbol.declaration.body)
    EmitBlock(*symbol.declaration.body);

  // Insert an return; to unwind the stack
  if (auto* string =
          std::get_if<std::string>(&symbol.declaration.return_type.type);
      string && (*string == "Void"))
    bytecode_.Return();

  called_symbols = std::move(called_symbols_);
  return FunctionObject{&symbol, std::move(bytecode_), argument_count,
                        symbol_to_local_idx_.size()};
}

void ByteCodeGenerator::EmitBlock(const Block& block,
                                  std::optional<LoopContext> loop_ctx) {
  for (const auto& stmt : block.statements) {
    std::visit(
        Overloaded{
            [&](const std::unique_ptr<Expression>& expr) {
              EmitExpression(expr);

              // Expressions always leave a value on the stack (if not
              // Void) so when executed as a Statement the unused value
              // must be consumed.
              if (expr->type != TypeContext::LiteralType::Void)
                bytecode_.StackDel();
            },
            [&](const FunctionDeclaration& fn) {
              // Will be handled as separate FunctionSymbol codegen.
            },
            [&](const ReturnStatement& ret) {
              EmitExpression(ret.value);
              bytecode_.Return();
            },
            [&](const ThrowStatement& thr) {
              EmitExpression(thr.value);
              bytecode_.Throw();
            },
            [&](const IfStatement& if_stmt) {
              EmitExpression(if_stmt.condition);
              std::string id = std::to_string(next_unique_id_++);
              bytecode_.JumpIfFalse("else" + id);
              EmitBlock(if_stmt.then_body, loop_ctx);
              bytecode_.Jump("end_if" + id);
              bytecode_.Label("else" + id);
              EmitBlock(if_stmt.else_body, loop_ctx);
              bytecode_.Label("end_if" + id);
            },
            [&](const WhileStatement& while_stmt) {
              std::string id = std::to_string(next_unique_id_++);
              std::string condition_label = "while_cond" + id;
              std::string end_label = "while_end" + id;

              bytecode_.Label(condition_label);
              EmitExpression(while_stmt.condition);
              bytecode_.JumpIfFalse(end_label);
              EmitBlock(while_stmt.body,
                        LoopContext{end_label, condition_label});
              bytecode_.Jump(condition_label);
              bytecode_.Label(end_label);
            },
            [&](const BreakStatement&) {
              CHECK(loop_ctx) << "break statement not within a loop";
              bytecode_.Jump(loop_ctx->break_label);
            },
            [&](const ContinueStatement&) {
              CHECK(loop_ctx) << "continue statement not within a loop";
              bytecode_.Jump(loop_ctx->continue_label);
            },
            [&](const AssignStatement& assign) {
              EmitExpression(assign.value);
              CHECK(assign.resolved)
                  << "Unresolved variable in assignment: " << assign.name;

              StoreSymbol(assign.resolved->symbol);
            },
            [&](const StructDeclaration& struct_decl) {
              // SemanticAnalyzer produces FunctionSymbols for methods
              // and they will be handled as separate functions.
            },
            [&](const ImportStatement& import) { /*nothing to compile*/ },
            [&](const TypeAliasStatement& alias) { /*nothing to compile*/ },
        },
        stmt->as);
  }
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
                Overloaded{[&](const StringLiteral& str) {
                             bytecode_.PushConstRef(
                                 constant_pool_.GetIdFor(str.value));
                           },
                           [&](const Identifier& ident) {
                             CHECK(ident.resolved)
                                 << "Unresolved identifier: " << ident.name;

                             switch (ident.resolved->symbol.kind) {
                               case NamedBinding::Function:
                                 CHECK(ident.resolved)
                                     << "Unresolved function:" << ident.name;
                                 bytecode_.PatchBind(
                                     *ident.resolved->symbol.symbol_id,
                                     /*argc=*/0);
                                 called_symbols_.push_back(
                                     *ident.resolved->symbol.symbol_id);
                                 break;
                               case NamedBinding::Argument:
                               case NamedBinding::Variable:
                               case NamedBinding::Capture:
                               case NamedBinding::Narrowed: {
                                 PushSymbol(ident.resolved->symbol);
                                 break;
                               }
                               default:
                                 NOTREACHED() << "Unsupported identifier kind: "
                                              << ident.resolved->symbol.kind;
                                 break;
                             }
                           },
                           [&](int32_t i32) { bytecode_.PushInt32(i32); },
                           [&](float f32) { bytecode_.PushFloat(f32); },
                           [&](const CodepointLiteral& codepoint) {
                             bytecode_.PushInt32(codepoint.value);
                           },
                           [&](bool b) { bytecode_.PushBool(b); },
                           [&](Nil) { bytecode_.PushNil(); }},
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
                  bytecode_.Is(value_type_t::VALUE_TYPE_NULL);
                  break;
                case TokenKind::kCompareNe:
                  bytecode_.Is(value_type_t::VALUE_TYPE_NULL);
                  bytecode_.Not();
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
                bytecode_.StackDup();

                CHECK(identifier->resolved)
                    << "Unresolved identifier on LHS of assignment: "
                    << identifier->name;

                StoreSymbol(identifier->resolved->symbol);
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

              bytecode_.Call(VM_BUILTIN_ARRAY_SET, 3);
            } else {
              LOG(FATAL) << "Unsupported LHS for AssignmentExpression: "
                         << assign.lhs->as.index();
            }
          },
          [&](const MemberAccessExpression& member_access) {
            EmitExpression(member_access.object, optional_chain_ctx);

            if (access_mode == AccessMode::LOAD ||
                access_mode == AccessMode::STORE) {
              CHECK(member_access.resolved)
                  << "Unresolved field access: " << member_access.member_name
                  << " at line: " << expr->meta.line_range.start;

              MemberIdx idx = member_access.resolved->index;
              bytecode_.PushInt32(idx);
            }
            if (access_mode == AccessMode::LOAD)
              bytecode_.Call(VM_BUILTIN_ARRAY_GET, 2);
          },
          [&](const ArrayAccessExpression& array_access) {
            EmitExpression(array_access.array, optional_chain_ctx);
            EmitExpression(array_access.index);

            if (access_mode == AccessMode::LOAD)
              bytecode_.Call(VM_BUILTIN_ARRAY_GET, 2);
          },
          [&](const LogicExpression& logic) {
            EmitExpression(logic.lhs);

            std::string id = std::to_string(next_unique_id_++);
            std::string lhs_label = "logic_lhs_" + id;
            std::string end_label = "logic_end_" + id;

            // Handle short-circuiting to skip evaluating the RHS if possible.
            if (logic.kind == LogicExpression::AND) {
              bytecode_.JumpIfFalse(lhs_label);
            } else {
              bytecode_.JumpIfTrue(lhs_label);
            }

            EmitExpression(logic.rhs);
            bytecode_.Jump(end_label);

            // Short-circuiting consumes the LHS value on the stack, so we need
            // to push the correct value for the result of the logic expression.
            bytecode_.Label(lhs_label);
            if (logic.kind == LogicExpression::AND) {
              bytecode_.PushBool(false);
            } else {
              bytecode_.PushBool(true);
            }

            bytecode_.Label(end_label);
          },
          [&](const ClosureExpression& closure) {
            CHECK(closure.fn.resolved) << "Unresolved function!";

            for (const auto& binding : closure.fn.resolved->required_captures)
              PushSymbol(binding);

            bytecode_.PatchBind(
                *closure.fn.resolved->function_symbol.symbol_id,
                /*argc=*/closure.fn.resolved->required_captures.size());
            called_symbols_.push_back(
                *closure.fn.resolved->function_symbol.symbol_id);
          },
          [&](PrefixUnaryExpression& prefix) {
            EmitExpression(prefix.operand);
            switch (prefix.op) {
              case TokenKind::kPlus:  // no-op
                break;
              case TokenKind::kMinus:
                // TODO: Add OP_NEGATE?
                bytecode_.PushInt32(-1);
                bytecode_.Multiply();
                break;
              case TokenKind::kPlusPlus:
              case TokenKind::kMinusMinus:
                LOG(FATAL) << "Prefix ++/-- are not yet supported!";
                break;
              case TokenKind::kNot:
                bytecode_.Not();
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
            bytecode_.Label(null_label);
          },
          [&](NilCoalescingExpression& coalescing) {
            std::string not_null_label =
                "coalesce_" + std::to_string(next_unique_id_++);

            EmitExpression(coalescing.lhs);
            bytecode_.StackDup();
            bytecode_.Is(value_type_t::VALUE_TYPE_NULL);
            bytecode_.JumpIfFalse(not_null_label);

            EmitExpression(coalescing.rhs);

            bytecode_.Label(not_null_label);
          },
          [&](OptionalAccessExpression& optional_access) {
            EmitExpression(optional_access.target, optional_chain_ctx,
                           access_mode);
            CHECK(optional_chain_ctx.has_value())
                << "Missing OptionalChainContext for OptionalAccessExpression";
            bytecode_.StackDup();
            bytecode_.Is(value_type_t::VALUE_TYPE_NULL);
            bytecode_.JumpIfTrue(optional_chain_ctx->null_label);
          },
          [&](TemplateInstantiationExpression& template_expr) {
            EmitExpression(template_expr.generic_target);
          },
      },
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
        bytecode_.PatchCall(call.resolved->target_symbol_id,
                            call.arguments.size());
        called_symbols_.push_back(call.resolved->target_symbol_id);
        break;
      }
      case Anonymous: {
        for (const auto& argument : call.arguments)
          EmitExpression(argument);
        EmitExpression(call.callee);
        bytecode_.CallDynamic(call.arguments.size());
        break;
      }
      case Method: {
        EmitExpression(call.callee, optional_chain_ctx,
                       AccessMode::OBJECT_ONLY);
        for (const auto& argument : call.arguments)
          EmitExpression(argument);

        bytecode_.PatchCall(call.resolved->target_symbol_id,
                            call.arguments.size() + 1);
        called_symbols_.push_back(call.resolved->target_symbol_id);
        break;
      }
      case Constructor: {
        for (const auto& argument : call.arguments)
          EmitExpression(argument);
        bytecode_.Call(VM_BUILTIN_ARRAY_INIT, call.arguments.size());
        break;
      }
    }
  }
}

void ByteCodeGenerator::EmitOp(const Token& op, bool is_string) {
  switch (op.kind) {
    case TokenKind::kPlus:
      if (is_string)
        bytecode_.Concat();
      else
        bytecode_.Add();
      break;
    case TokenKind::kMinus:
      bytecode_.Subtract();
      break;
    case TokenKind::kMultiply:
      bytecode_.Multiply();
      break;
    case TokenKind::kDivide:
      bytecode_.Divide();
      break;
    case TokenKind::kCompareEq:
      bytecode_.Compare(OP_EQUAL);
      break;
    case TokenKind::kCompareNe:
      bytecode_.Compare(OP_EQUAL);
      bytecode_.Not();
      break;
    case TokenKind::kCompareGt:
      bytecode_.Compare(OP_GREATER_THAN);
      break;
    case TokenKind::kCompareGe:
      bytecode_.Compare(OP_GREAT_OR_EQ);
      break;
    case TokenKind::kCompareLt:
      bytecode_.Compare(OP_LESS_THAN);
      break;
    case TokenKind::kCompareLe:
      bytecode_.Compare(OP_LESS_OR_EQ);
      break;
    default:
      LOG(ERROR) << "unsupported binary operator: " << op.kind;
      return;
  }
}

void ByteCodeGenerator::PushSymbol(NamedBinding symbol) {
  uint32_t local_idx = GetIdFor(symbol);
  bytecode_.PushLocal(local_idx);
}

void ByteCodeGenerator::StoreSymbol(NamedBinding symbol) {
  uint32_t local_idx = GetIdFor(symbol);
  bytecode_.StoreLocal(local_idx);
}

uint32_t ByteCodeGenerator::GetIdFor(NamedBinding lookup) {
  if (auto it = symbol_to_local_idx_.find(lookup.idx.value());
      it != symbol_to_local_idx_.end())
    return it->second;

  uint32_t id = next_local_idx_++;
  symbol_to_local_idx_[lookup.idx.value()] = id;
  return lookup.idx.value();
}

uint32_t ConstantPool::GetIdFor(const std::string& value) {
  auto it = string_constant_to_id_.find(value);
  if (it != string_constant_to_id_.end())
    return it->second;

  uint32_t id = next_string_constant_id_++;
  string_constant_to_id_[value] = id;
  return id;
}

std::vector<std::string> ConstantPool::GetStrings() const {
  std::vector<std::string> strings;
  strings.resize(string_constant_to_id_.size());
  for (const auto& [str, id] : string_constant_to_id_)
    strings[id] = str;
  return strings;
}