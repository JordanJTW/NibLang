/*
 * Copyright (C) 2026, Jordan Werthman <jordanwerthman@gmail.com>.
 *
 * This software is dual-licensed. You may use this file under the terms of
 * the GNU General Public License v3.0 (GPL-3.0) OR under a commercial
 * license obtained directly from the copyright holder.
 *
 * See the LICENSE.md file in the project root for full licensing details.
 *
 * SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-Commercial
 */

#include "compiler/codegen/bytecode_generator.h"

#include <ranges>
#include <utility>
#include <variant>
#include <vector>

#include "compiler/codegen/assembler.h"
#include "compiler/logging.h"
#include "compiler/parser/printer.h"
#include "compiler/parser/tokenizer.h"
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

ByteCodeGenerator::ByteCodeGenerator(const ScopeManager& scope_manager,
                                     const TypeRegistry& type_registry,
                                     ConstantPool& constant_pool)
    : scope_manager_(scope_manager),
      type_registry_(type_registry),
      constant_pool_(constant_pool) {}

ByteCodeGenerator::FunctionObject ByteCodeGenerator::Build(
    const FunctionSymbol& symbol,
    std::vector<SymbolId>& called_symbols) && {
  size_t argument_count = 0;
  // Process captures first to reflect their position in an OP_BIND.
  for (const auto& binding :
       scope_manager_.GetBindingsForScope(symbol.instance_scope_id)) {
    if (binding.kind == NamedBinding::Kind::Capture) {
      Slot slot = GetSlotFor(binding);
      argument_count += slot.width;
    }
  }

  for (const auto& binding :
       scope_manager_.GetBindingsForScope(symbol.instance_scope_id)) {
    if (binding.kind == NamedBinding::Kind::Argument) {
      Slot slot = GetSlotFor(binding);
      argument_count += slot.width;
    }
  }

  for (const auto& type_id :
       symbol.declaration.resolved->required_template_variables) {
    if (type_registry_.GetTypeChecked<TemplateVariableType>(type_id)
            .constraint_type_id.has_value()) {
      template_variable_to_local_slot_[type_id] = next_local_idx_++;
      ++argument_count;
    }
  }

  if (symbol.declaration.body)
    EmitBlock(*symbol.declaration.body);

  if (symbol.declaration.resolved->should_insert_unit_return) {
    bytecode_.PushUnit();
    bytecode_.Return();
  }

  called_symbols = std::move(called_symbols_);

  uint32_t local_count =
      symbol.declaration.resolved->required_template_variables.size();
  for (const auto& [idx, width] : symbol_to_local_slot_ | std::views::values) {
    local_count += width;
  }

  return FunctionObject{&symbol, bytecode_, argument_count, local_count};
}

void ByteCodeGenerator::EmitBlock(const Block& block,
                                  std::optional<LoopContext> loop_ctx) {
  for (const auto& stmt : block.statements) {
    std::visit(
        Overloaded{
            [&](const std::unique_ptr<Expression>& expr) {
              EmitExpression(expr);

              // Expressions always leave a value on the stack so when executed
              // as a Statement the unused value must be consumed.
              bytecode_.StackDel();
            },
            [&](const FunctionDeclaration& fn) {
              // Will be handled as separate FunctionSymbol codegen.
            },
            [&](const ReturnStatement& ret) {
              if (ret.value) {
                EmitExpression(ret.value);
              } else {
                bytecode_.PushUnit();
              }
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
                  << "Unresolved variable in assignment: " << assign.name.text;

              StoreSymbol(assign.resolved->symbol);
            },
            [&](const StructDeclaration& struct_decl) {
              // SemanticAnalyzer produces FunctionSymbols for methods
              // and they will be handled as separate functions.
            },
            [&](const ImportStatement& import) { /*nothing to compile*/ },
            [&](const TypeAliasStatement& alias) { /*nothing to compile*/ },
            [&](const InterfaceDeclaration& decl) { /*nothing to compile*/ },
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
                Overloaded{
                    [&](const StringLiteral& str) {
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
                          bytecode_.PatchBind(*ident.resolved->symbol.symbol_id,
                                              /*argc=*/0);
                          called_symbols_.push_back(
                              *ident.resolved->symbol.symbol_id);
                          break;
                        case NamedBinding::Argument:
                        case NamedBinding::Variable:
                        case NamedBinding::Capture: {
                          PushSymbol(ident.resolved->symbol,
                                     expr->implicit_cast &&
                                             expr->implicit_cast->kind ==
                                                 ImplicitCast::Kind::kSelf
                                         ? SuppressFatPointer::YES
                                         : SuppressFatPointer::NO);
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
            auto emit_comparison_operator = [this](TokenKind kind) {
              switch (kind) {
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
                  LOG(FATAL) << "Invalid comparison op: " << kind;
                  break;
              }
            };

            if (binary.lhs->type_id == TypeRegistry::Nil ||
                binary.rhs->type_id == TypeRegistry::Nil) {
              const std::unique_ptr<Expression>& target =
                  binary.lhs->type_id != TypeRegistry::Nil ? binary.lhs
                                                           : binary.rhs;
              EmitExpression(target);
              bytecode_.Is(VALUE_TYPE_NULL);
            } else {
              CHECK(binary.resolved) << "Unresolved binary expression";

              const auto& symbol =
                  type_registry_.GetSymbolChecked<FunctionSymbol>(
                      binary.resolved->symbol_id);

              if (symbol.intrinsic_id.has_value()) {
                EmitExpression(binary.lhs);
                EmitExpression(binary.rhs);

                switch (symbol.intrinsic_id.value()) {
                  case IntrinsicId::Int32_EqualTo:
                  case IntrinsicId::Float32_EqualTo:
                    bytecode_.Compare(OP_EQUAL);
                    break;
                  case IntrinsicId::Int32_Add:
                  case IntrinsicId::Float32_Add:
                    bytecode_.Add();
                    break;
                  case IntrinsicId::Int32_Subtract:
                  case IntrinsicId::Float32_Subtract:
                    bytecode_.Subtract();
                    break;
                  case IntrinsicId::Int32_Multiply:
                  case IntrinsicId::Float32_Multiply:
                    bytecode_.Multiply();
                    break;
                  case IntrinsicId::Int32_Divide:
                  case IntrinsicId::Float32_Divide:
                    bytecode_.Divide();
                    break;
                  case IntrinsicId::Int32_Modulus:
                    bytecode_.Modulo();
                    break;
                  case IntrinsicId::Int32_BitwiseAnd:
                    bytecode_.BitwiseAnd();
                    break;
                  case IntrinsicId::Int32_CompareTo:
                  case IntrinsicId::Float32_CompareTo:
                    emit_comparison_operator(binary.op);
                    break;
                  case IntrinsicId::String_Concat:
                    bytecode_.Concat();
                    break;
                }
              } else {
                if (binary.IsComparison())
                  bytecode_.PushInt32(0);

                EmitExpression(binary.lhs);
                EmitExpression(binary.rhs);
                if (symbol.RequiresVirtualDispatch()) {
                  PushDispatchIdForTypeId(binary.lhs->type_id);
                  bytecode_.PatchCallVirtual(binary.resolved->symbol_id, 2);
                } else
                  bytecode_.PatchCall(binary.resolved->symbol_id, 2);
                called_symbols_.push_back(binary.resolved->symbol_id);

                if (binary.IsComparison())
                  emit_comparison_operator(binary.op);
              }
            }

            if (binary.op == TokenKind::kCompareNe)
              bytecode_.Not();
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

              size_t slot_count = GetWidthOf(assign.lhs->type_id);

              bytecode_.Call(VM_BUILTIN_ARRAY_SET, 2 + slot_count);
            } else {
              LOG(FATAL) << "Unsupported LHS for AssignmentExpression: "
                         << assign.lhs->as.index();
            }
          },
          [&](const MemberAccessExpression& member_access) {
            CHECK(member_access.resolved)
                << "Unresolved MemberAccessExpression";

            if (const auto* fn = std::get_if<ResolvedAccess::Function>(
                    &member_access.resolved->type)) {
              bytecode_.PatchBind(fn->symbol_id, /*argc=*/0);
              called_symbols_.push_back(fn->symbol_id);
            } else if (const auto* method = std::get_if<ResolvedAccess::Method>(
                           &member_access.resolved->type)) {
              EmitExpression(member_access.object, optional_chain_ctx);

              if (access_mode == AccessMode::OBJECT_ONLY)
                return;

              // Bind to `self` when referencing a method
              bytecode_.PatchBind(method->symbol_id, /*argc=*/1);
              called_symbols_.push_back(method->symbol_id);
            } else if (const auto* field = std::get_if<ResolvedAccess::Field>(
                           &member_access.resolved->type)) {
              if (access_mode == AccessMode::LOAD) {
                for (size_t idx = 0; idx < GetWidthOf(expr->type_id); ++idx) {
                  EmitExpression(member_access.object, optional_chain_ctx);
                  bytecode_.PushInt32(field->index + idx);
                  bytecode_.Call(VM_BUILTIN_ARRAY_GET, 2);
                }
              } else if (access_mode == AccessMode::STORE) {
                EmitExpression(member_access.object, optional_chain_ctx);
                bytecode_.PushInt32(field->index);
              }
            } else {
              NOTREACHED() << "MemberAccessExpression resolved to unknown type";
            }
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
              PushSymbol(binding, SuppressFatPointer::NO);

            bytecode_.PatchBind(
                closure.fn.resolved->function_symbol_id,
                /*argc=*/closure.fn.resolved->required_captures.size());
            called_symbols_.push_back(closure.fn.resolved->function_symbol_id);
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
          [&](SizeOfExpression& size_expr) {
            CHECK(size_expr.resolved) << "unresolved sizeof";
            bytecode_.PushInt32(*size_expr.resolved);
          }},
      expr->as);

  if (expr->implicit_cast &&
      expr->implicit_cast->kind == ImplicitCast::Kind::kInterface) {
    PushDispatchIdForTypeId(expr->implicit_cast->type_id);
  }
}

void ByteCodeGenerator::EmitCall(
    const CallExpression& call,
    const std::optional<OptionalChainContext>& optional_chain_ctx) {
  size_t argc = 0;
  if (call.resolved) {
    switch (call.resolved->kind) {
      case Method:
        EmitExpression(call.callee, optional_chain_ctx,
                       AccessMode::OBJECT_ONLY);
        argc += 1;  // Account for `self`
        // Intentional fall-through to function call logic
      case Free:
      case Extern:
      case StaticMethod: {
        const auto& symbol = type_registry_.GetSymbolChecked<FunctionSymbol>(
            call.resolved->target.symbol_id);

        for (const auto& argument : call.arguments) {
          EmitExpression(argument);
          if (argument->implicit_cast &&
              argument->implicit_cast->kind == ImplicitCast::Kind::kInterface) {
            argc += 2;
          } else {
            argc += GetWidthOf(argument->type_id);
          }
        }

        for (const auto& variable :
             symbol.declaration.resolved->required_template_variables) {
          if (type_registry_.GetTypeChecked<TemplateVariableType>(variable)
                  .constraint_type_id.has_value()) {
            auto it = call.resolved->template_arguments.find(variable);
            CHECK_NE(it, call.resolved->template_arguments.end());
            PushDispatchIdForTypeId(it->second);
            ++argc;
          }
        }

        bytecode_.PatchCall(call.resolved->target.symbol_id, argc);
        called_symbols_.push_back(call.resolved->target.symbol_id);
        break;
      }
      case Anonymous: {
        for (const auto& argument : call.arguments)
          EmitExpression(argument);
        EmitExpression(call.callee);
        bytecode_.CallDynamic(argc);
        break;
      }
      case Virtual: {
        const auto* member_access =
            std::get_if<MemberAccessExpression>(&call.callee->as);
        CHECK(member_access != nullptr) << "Virtual dispatch on non-method?";

        EmitExpression(member_access->object);

        for (const auto& argument : call.arguments) {
          EmitExpression(argument);
          argc += GetWidthOf(argument->type_id);
        }

        PushDispatchIdForTypeId(member_access->object->type_id);
        bytecode_.PatchCallVirtual(call.resolved->target.symbol_id,
                                   argc + 1 /*self*/);
        called_symbols_.push_back(call.resolved->target.symbol_id);
        break;
      }
      case Constructor: {
        for (const auto& argument : call.arguments) {
          EmitExpression(argument);
          if (argument->implicit_cast &&
              argument->implicit_cast->kind == ImplicitCast::Kind::kInterface) {
            argc += 2;
          } else {
            argc += GetWidthOf(argument->type_id);
          }
        }
        bytecode_.Call(VM_BUILTIN_ARRAY_INIT, argc);
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
    case TokenKind::kPercent:
      bytecode_.Modulo();
      break;
    case TokenKind::kAnd:
      bytecode_.BitwiseAnd();
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

uint32_t ByteCodeGenerator::PushSymbol(
    NamedBinding symbol,
    ByteCodeGenerator::SuppressFatPointer suppress_fat_pointer) {
  Slot slot = GetSlotFor(symbol);
  if (suppress_fat_pointer == SuppressFatPointer::YES) {
    bytecode_.PushLocal(slot.idx + 1);
    return 1;
  }

  for (uint32_t i = 0; i < slot.width; ++i)
    bytecode_.PushLocal(slot.idx + i);
  return slot.width;
}

void ByteCodeGenerator::StoreSymbol(NamedBinding symbol) {
  Slot slot = GetSlotFor(symbol);
  for (uint32_t i = 0; i < slot.width; ++i)
    bytecode_.StoreLocal(slot.idx + slot.width - i - 1);
}

ByteCodeGenerator::Slot ByteCodeGenerator::GetSlotFor(NamedBinding lookup) {
  if (auto it = symbol_to_local_slot_.find(lookup.idx.value());
      it != symbol_to_local_slot_.end()) {
    return it->second;
  }

  uint32_t idx = next_local_idx_;
  uint32_t width = GetWidthOf(lookup.type_id);
  next_local_idx_ += width;
  symbol_to_local_slot_[lookup.idx.value()] = Slot{idx, width};
  return Slot{idx, width};
}

uint32_t ByteCodeGenerator::GetWidthOf(TypeId type_id) {
  if (auto* struct_type = type_registry_.GetType<StructType>(type_id)) {
    if (struct_type->symbol->declaration.IsInterface())
      return 2;
  }

  return 1;
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

void ByteCodeGenerator::PushDispatchIdForTypeId(TypeId type_id) {
  if (type_registry_.GetType<TemplateVariableType>(type_id)) {
    bytecode_.PushLocal(template_variable_to_local_slot_.at(type_id));
    return;
  }

  if (const auto* struct_type = type_registry_.GetType<StructType>(type_id)) {
    if (!struct_type->symbol->declaration.IsInterface())
      bytecode_.PushDispatchIdToPatch(struct_type->symbol->symbol_id);
    return;
  }

  NOTREACHED() << "Invalid type for dynamic dispatch: "
               << type_registry_.GetNameFromTypeId(type_id);
}