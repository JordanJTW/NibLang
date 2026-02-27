#include <cassert>
#include <cstdio>
#include <fstream>

#include "compiler/assembler.h"
#include "compiler/logging.h"
#include "compiler/parser.h"
#include "compiler/program_builder.h"
#include "compiler/tokenizer.h"
#include "compiler/type_checker.h"
#include "src/vm.h"

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

void emit_op(const Token& op, ProgramBuilder& builder) {
  switch (op.kind) {
    case TokenKind::kAdd:
      builder.GetCurrentCode().Add();
      break;
    case TokenKind::kSubtract:
      builder.GetCurrentCode().Subtract();
      break;
    case TokenKind::kMultiply:
      builder.GetCurrentCode().Multiply();
      break;
    case TokenKind::kDivide:
      builder.GetCurrentCode().Divide();
      break;
    case TokenKind::kCompareEq:
      builder.GetCurrentCode().Compare(OP_EQUAL);
      break;
    case TokenKind::kCompareNe:
      builder.GetCurrentCode().Compare(OP_EQUAL);
      builder.GetCurrentCode().Not();
      break;
    case TokenKind::kCompareGt:
      builder.GetCurrentCode().Compare(OP_GREATER_THAN);
      break;
    case TokenKind::kCompareGe:
      builder.GetCurrentCode().Compare(OP_GREAT_OR_EQ);
      break;
    case TokenKind::kCompareLt:
      builder.GetCurrentCode().Compare(OP_LESS_THAN);
      break;
    case TokenKind::kCompareLe:
      builder.GetCurrentCode().Compare(OP_LESS_OR_EQ);
      break;
    default:
      std::cerr << "unsupported binary operator: " << op.kind << std::endl;
      return;
  }
}

void compile_expr(const std::unique_ptr<Expression>& expr,
                  ProgramBuilder& builder);

void compile_call(const CallExpression& call, ProgramBuilder& builder) {
  if (call.resolved) {
    switch (call.resolved->kind) {
      case Free:
        for (const auto& arg : call.arguments)
          compile_expr(arg, builder);
        builder.GetCurrentCode().Call(call.resolved->function_idx,
                                      call.arguments.size());
        break;
      case Method:
        if (const auto* const member_access =
                std::get_if<MemberAccessExpression>(&call.callee->as)) {
          compile_expr(member_access->object, builder);
          for (const auto& arg : call.arguments)
            compile_expr(arg, builder);

          builder.GetCurrentCode().Call(call.resolved->function_idx,
                                        call.arguments.size() + 1);
        }
        break;
      case Constructor:
        builder.GetCurrentCode().PushInt32(call.arguments.size());
        for (const auto& arg : call.arguments)
          compile_expr(arg, builder);
        builder.CallFunction("Array_init", call.arguments.size() + 1);
        break;
    }
  }
}

void compile_expr(const std::unique_ptr<Expression>& expr,
                  ProgramBuilder& builder) {
  if (expr == nullptr) {
    std::cerr << "Failed to compile null expression" << std::endl;
    return;
  }

  std::visit(
      Overloaded{
          [&](const PrimaryExpression& primary) {
            std::visit(
                Overloaded{
                    [&](const StringLiteral& str) {
                      builder.GetCurrentCode().PushConstRef(
                          builder.GetIdForConstant(str.value));
                    },
                    [&](const Identifier& ident) {
                      std::optional<uint32_t> id = builder.GetIdFor(
                          ident.name, ProgramBuilder::CreateIfMissing::No);
                      if (id.has_value()) {
                        builder.GetCurrentCode().PushLocal(*id);
                      } else {
                        std::cerr << "undefined identifier: " << ident.name
                                  << std::endl;
                      }
                    },
                    [&](int32_t i32) {
                      builder.GetCurrentCode().PushInt32(i32);
                    },
                    [&](float f32) { builder.GetCurrentCode().PushFloat(f32); },
                    [&](bool b) { builder.GetCurrentCode().PushBool(b); },
                },
                primary.value);
          },
          [&](const BinaryExpression& binary) {
            compile_expr(binary.lhs, builder);
            compile_expr(binary.rhs, builder);
            emit_op(Token{.kind = binary.op}, builder);
          },
          [&](const CallExpression& call) { compile_call(call, builder); },
          [&](const AssignmentExpression& assign) {
            if (std::holds_alternative<PrimaryExpression>(assign.lhs->as) &&
                std::holds_alternative<Identifier>(
                    std::get<PrimaryExpression>(assign.lhs->as).value)) {
              compile_expr(assign.rhs, builder);

              std::string var_name =
                  std::get<Identifier>(
                      std::get<PrimaryExpression>(assign.lhs->as).value)
                      .name;
              std::optional<uint32_t> id = builder.GetIdFor(
                  var_name, ProgramBuilder::CreateIfMissing::No);
              if (!id.has_value()) {
                std::cerr << "undefined identifier: " << var_name << std::endl;
              }
              builder.GetCurrentCode().StoreLocal(*id);
            } else if (std::holds_alternative<ArrayAccessExpression>(
                           assign.lhs->as)) {
              const auto& array_access =
                  std::get<ArrayAccessExpression>(assign.lhs->as);
              compile_expr(array_access.array, builder);
              compile_expr(array_access.index, builder);
              compile_expr(assign.rhs, builder);

              builder.CallFunction("Array_set", 3);
            } else if (auto* const member_access =
                           std::get_if<MemberAccessExpression>(
                               &assign.lhs->as)) {
              if (!member_access->resolved) {
                std::cerr << "Member was never resolved "
                             "during type checking!"
                          << std::endl;
                return;
              }

              compile_expr(member_access->object, builder);
              builder.GetCurrentCode().PushInt32(
                  member_access->resolved->index);
              compile_expr(assign.rhs, builder);
              builder.CallFunction("Array_set", 3);
            } else {
              std::cerr << "unsupported LHS in assignment expression"
                        << std::endl;
            }
          },
          [&](const MemberAccessExpression& member_access) {
            if (const auto& resolved = member_access.resolved) {
              compile_expr(member_access.object, builder);
              builder.GetCurrentCode().PushInt32(resolved->index);
              builder.CallFunction("Array_get", 2);
            } else {
              LOG(FATAL) << "Unresolved member access: "
                         << member_access.member_name
                         << " at line: " << expr->meta.line_range.start;
            }
          },
          [&](const ArrayAccessExpression& array_access) {
            compile_expr(array_access.array, builder);
            compile_expr(array_access.index, builder);
            builder.CallFunction("Array_get", 2);
          },
          [&](const LogicExpression& logic) {
            compile_expr(logic.lhs, builder);

            static size_t id = 0;
            std::string lhs_label = "logic_lhs_" + std::to_string(id++);
            std::string end_label = "logic_end_" + std::to_string(id);

            // Handle short-circuiting to skip evaluating the RHS if possible.
            if (logic.kind == LogicExpression::AND) {
              builder.GetCurrentCode().JumpIfFalse(lhs_label);
            } else {
              builder.GetCurrentCode().JumpIfTrue(lhs_label);
            }

            compile_expr(logic.rhs, builder);
            builder.GetCurrentCode().Jump(end_label);

            // Short-circuiting consumes the LHS value on the stack, so we need
            // to push the correct value for the result of the logic expression.
            builder.GetCurrentCode().Label(lhs_label);
            if (logic.kind == LogicExpression::AND) {
              builder.GetCurrentCode().PushBool(false);
            } else {
              builder.GetCurrentCode().PushBool(true);
            }

            builder.GetCurrentCode().Label(end_label);
          },
          [&](const NewExpression& new_expr) {
            builder.GetCurrentCode().PushInt32(new_expr.arguments.size());
            for (const auto& expr : new_expr.arguments) {
              compile_expr(expr, builder);
            }
            builder.CallFunction("Array_init", new_expr.arguments.size() + 1);
          }},
      expr->as);
}

struct LoopContext {
  std::string break_label;
  std::string continue_label;
};

void compile(const Block& root,
             ProgramBuilder& builder,
             std::optional<LoopContext> loop_ctx = std::nullopt) {
  for (const auto& stmt : root.statements) {
    std::visit(
        Overloaded{[&](const std::unique_ptr<Expression>& expr) {
                     compile_expr(expr, builder);
                   },
                   [&](const FunctionDeclaration& fn) {
                     if (fn.body) {
                       builder.EnterFunctionScope(fn.name, fn.arguments);
                       compile(*fn.body, builder);
                       // Insert an return; to unwind the stack
                       if (fn.resolved->is_void_return)
                         builder.GetCurrentCode().Return();
                       builder.ExitFunctionScope();
                     }
                   },
                   [&](const ReturnStatement& ret) {
                     compile_expr(ret.value, builder);
                     builder.GetCurrentCode().Return();
                   },
                   [&](const ThrowStatement& thr) {
                     compile_expr(thr.value, builder);
                     builder.GetCurrentCode().Throw();
                   },
                   [&](const IfStatement& if_stmt) {
                     compile_expr(if_stmt.condition, builder);
                     builder.GetCurrentCode().JumpIfFalse(
                         "else" + std::to_string(if_stmt.id));
                     compile(if_stmt.then_body, builder, loop_ctx);
                     builder.GetCurrentCode().Jump("end_if" +
                                                   std::to_string(if_stmt.id));
                     builder.GetCurrentCode().Label("else" +
                                                    std::to_string(if_stmt.id));
                     compile(if_stmt.else_body, builder, loop_ctx);
                     builder.GetCurrentCode().Label("end_if" +
                                                    std::to_string(if_stmt.id));
                   },
                   [&](const WhileStatement& while_stmt) {
                     std::string condition_label =
                         "while_cond" + std::to_string(while_stmt.id);
                     std::string end_label =
                         "while_end" + std::to_string(while_stmt.id);

                     builder.GetCurrentCode().Label(condition_label);
                     compile_expr(while_stmt.condition, builder);
                     builder.GetCurrentCode().JumpIfFalse(end_label);
                     compile(while_stmt.body, builder,
                             LoopContext{end_label, condition_label});
                     builder.GetCurrentCode().Jump(condition_label);
                     builder.GetCurrentCode().Label(end_label);
                   },
                   [&](const BreakStatement&) {
                     if (!loop_ctx) {
                       std::cerr << "break statement not within a loop"
                                 << std::endl;
                       return;
                     }
                     builder.GetCurrentCode().Jump(loop_ctx->break_label);
                   },
                   [&](const ContinueStatement&) {
                     if (!loop_ctx) {
                       std::cerr << "continue statement not within a loop"
                                 << std::endl;
                       return;
                     }
                     builder.GetCurrentCode().Jump(loop_ctx->continue_label);
                   },
                   [&](const AssignStatement& assign) {
                     compile_expr(assign.value, builder);

                     std::optional<uint32_t> id = builder.GetIdFor(
                         assign.name, ProgramBuilder::CreateIfMissing::Yes);
                     builder.GetCurrentCode().StoreLocal(*id);
                   },
                   [&](const StructDeclaration& struct_decl) {
                     for (const auto& method : struct_decl.methods) {
                       if (!method.second.body)
                         continue;

                       builder.EnterFunctionScope(
                           struct_decl.name + "_" + method.first,
                           method.second.arguments);
                       compile(*method.second.body, builder);
                       builder.ExitFunctionScope();
                     }
                   }},
        stmt->as);
  }
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    fprintf(stderr, "%s <path>\n", argv[0]);
    return 1;
  }

  std::ifstream file(argv[1]);
  if (!file) {
    fprintf(stderr, "Error opening: %s\n", argv[1]);
    return 1;
  }

  std::string contents{std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>()};

  ProgramBuilder builder;
  Parser parser(contents, builder);

  Block root = parser.Parse();
  TypeChecker type_checker(contents);
  type_checker.Check(root);

  std::cout << "Finished parsing. Printing AST:\n";
  for (const auto& stmt : root.statements) {
    print_statement(*stmt);
  }

  compile(root, builder);

  std::vector<uint8_t> program_image = builder.GenerateImage();
  std::ofstream out("/tmp/prog.ink", std::ios::binary);

  if (out.is_open()) {
    out.write(reinterpret_cast<const char*>(program_image.data()),
              program_image.size());
    out.close();
  }
  return 0;
}
