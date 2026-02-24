#include "compiler/types.h"

#include <iomanip>
#include <iostream>

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

std::ostream& operator<<(std::ostream& os, const ParsedType& type) {
  std::visit(Overloaded{[&](const ParsedTypeName& type) { os << type.name; },
                        [&](const ParsedUnionType& type) {
                          for (const auto& name : type.names) {
                            for (size_t i = 0; i < type.names.size(); ++i) {
                              if (i > 0) {
                                os << "|";
                              }
                              os << type.names[i];
                            }
                          }
                        }},
             type);

  return os;
}

static void print_primary(const PrimaryExpression& primary, size_t indent) {
  std::visit(Overloaded{
                 [&](const StringLiteral& str) {
                   std::cout << std::string(indent, ' ') << "StringLiteral: \""
                             << str.value << "\"" << std::endl;
                 },
                 [&](const Identifier& ident) {
                   std::cout << std::string(indent, ' ') << " "
                             << "Identifier: " << ident.name << std::endl;
                 },
                 [&](int32_t i32) {
                   std::cout << std::string(indent, ' ') << "Int32: " << i32
                             << std::endl;
                 },
                 [&](float f32) {
                   std::cout << std::string(indent, ' ') << "Float: " << f32
                             << std::endl;
                 },
                 [&](bool b) {
                   std::cout << std::string(indent, ' ')
                             << "Bool: " << (b ? "true" : "false") << std::endl;
                 },
             },
             primary.value);
}

void print_expression(const std::unique_ptr<Expression>& expr, size_t indent) {
  if (expr == nullptr) {
    std::cerr << std::string(indent, ' ') << "Parse Failure" << std::endl;
    return;
  }

  std::visit(
      Overloaded{
          [&](const PrimaryExpression& primary) {
            print_primary(primary, indent);
          },
          [&](const BinaryExpression& binary) {
            std::cout << std::string(indent, ' ')
                      << "BinaryExpression (op=" << static_cast<int>(binary.op)
                      << ") type: " << expr->type << std::endl;

            std::cout << std::string(indent + 2, ' ') << "LHS:" << std::endl;
            print_expression(binary.lhs, indent + 4);

            std::cout << std::string(indent + 2, ' ') << "RHS:" << std::endl;
            print_expression(binary.rhs, indent + 4);
          },
          [&](const AssignmentExpression& assign) {
            std::cout << std::string(indent, ' ')
                      << "AssignmentExpression type: " << expr->type
                      << std::endl;

            std::cout << std::string(indent + 2, ' ') << "LHS:" << std::endl;
            print_expression(assign.lhs, indent + 4);
            std::cout << std::string(indent + 2, ' ') << "RHS:" << std::endl;
            print_expression(assign.rhs, indent + 4);
          },
          [&](const CallExpression& call) {
            std::cout << std::string(indent, ' ')
                      << "CallExpression type: " << expr->type << std::endl;
            std::cout << std::string(indent + 2, ' ') << "Callee:" << std::endl;
            print_expression(call.callee, indent + 4);

            std::cout << std::string(indent + 2, ' ')
                      << "Arguments:" << std::endl;
            for (const auto& arg : call.arguments)
              print_expression(arg, indent + 4);

            std::cout << std::string(indent + 2, ' ') << "Call Idx: "
                      << (call.resolved.has_value()
                              ? std::to_string(call.resolved->function_idx)
                              : "Unresolved")
                      << std::endl;
          },
          [&](const MemberAccessExpression& member_access) {
            std::cout << std::string(indent, ' ')
                      << "MemberAccessExpression: " << member_access.member_name
                      << " type: " << expr->type << std::endl;

            std::cout << std::string(indent + 2, ' ') << "Object:" << std::endl;
            print_expression(member_access.object, indent + 4);

            std::cout << std::string(indent + 2, ' ') << "Resolved: "
                      << (member_access.resolved.has_value() ? "YES" : "NO")
                      << std::endl;
          },
          [&](const ArrayAccessExpression& array_access) {
            std::cout << std::string(indent, ' ')
                      << "ArrayAccessExpression type: " << expr->type
                      << std::endl;

            std::cout << std::string(indent + 2, ' ') << "Array:" << std::endl;
            print_expression(array_access.array, indent + 4);

            std::cout << std::string(indent + 2, ' ') << "Index:" << std::endl;
            print_expression(array_access.index, indent + 4);
          },
          [&](const LogicExpression& logic) {
            std::cout << std::string(indent, ' ')
                      << "LogicExpression (op=" << static_cast<int>(logic.kind)
                      << ") type: " << expr->type << std::endl;

            std::cout << std::string(indent + 2, ' ') << "LHS:" << std::endl;
            print_expression(logic.lhs, indent + 4);

            std::cout << std::string(indent + 2, ' ') << "RHS:" << std::endl;
            print_expression(logic.rhs, indent + 4);
          },
          [&](const NewExpression& new_expr) {
            std::cout << std::string(indent, ' ')
                      << "NewExpression: " << new_expr.struct_name
                      << " type: " << expr->type << std::endl;

            for (const auto& expr : new_expr.arguments) {
              print_expression(expr, indent + 2);
            }
          }},
      expr->as);
}

void print_function(const FunctionDeclaration& fn, size_t indent) {
  std::cout << std::string(indent, ' ') << "FunctionDeclaration: " << fn.name
            << " ["
            << (fn.call_idx.has_value() ? std::to_string(fn.call_idx.value())
                                        : "Unresolved")
            << "]" << std::endl;

  std::cout << std::string(indent + 2, ' ') << "Arguments:" << std::endl;
  for (const auto& arg : fn.arguments) {
    std::cout << std::string(indent + 4, ' ') << arg.first << ": " << arg.second
              << std::endl;
  }

  if (fn.body) {
    std::cout << std::string(indent + 2, ' ') << "Body:" << std::endl;
    for (const auto& stmt : fn.body->statements)
      print_statement(*stmt, indent + 4);

  } else {
    std::cout << std::string(indent + 2, ' ') << " "
              << "Extern function, no body" << std::endl;
  }
}

void print_statement(const Statement& stmt, size_t indent) {
  std::visit(
      Overloaded{
          [&](const std::unique_ptr<Expression>& expr) {
            std::cout << std::string(indent, ' ')
                      << "ExpressionStatement:" << std::endl;
            print_expression(expr, indent + 2);
          },
          [&](const FunctionDeclaration& fn) { print_function(fn, indent); },
          [&](const ReturnStatement& ret) {
            std::cout << std::string(indent, ' ')
                      << "ReturnStatement:" << std::endl;
            print_expression(ret.value, indent + 2);
          },
          [&](const ThrowStatement& thr) {
            std::cout << std::string(indent, ' ')
                      << "ThrowStatement:" << std::endl;
            print_expression(thr.value, indent + 2);
          },
          [&](const IfStatement& if_stmt) {
            std::cout << std::string(indent, ' ')
                      << "IfStatement:" << std::endl;

            std::cout << std::string(indent + 2, ' ')
                      << "Condition:" << std::endl;
            print_expression(if_stmt.condition, indent + 4);

            std::cout << std::string(indent + 2, ' ')
                      << "Then Body:" << std::endl;
            for (const auto& stmt : if_stmt.then_body.statements)
              print_statement(*stmt, indent + 4);

            std::cout << std::string(indent + 2, ' ')
                      << "Else Body:" << std::endl;
            for (const auto& stmt : if_stmt.else_body.statements)
              print_statement(*stmt, indent + 4);
          },
          [&](const WhileStatement& while_stmt) {
            std::cout << std::string(indent, ' ')
                      << "WhileStatement:" << std::endl;

            std::cout << std::string(indent + 2, ' ')
                      << "Condition:" << std::endl;
            print_expression(while_stmt.condition, indent + 4);

            std::cout << std::string(indent + 2, ' ') << "Body:" << std::endl;
            for (const auto& stmt : while_stmt.body.statements)
              print_statement(*stmt, indent + 4);
          },
          [&](const BreakStatement&) {
            std::cout << std::string(indent, ' ') << "BreakStatement"
                      << std::endl;
          },
          [&](const ContinueStatement&) {
            std::cout << std::string(indent, ' ') << "ContinueStatement"
                      << std::endl;
          },
          [&](const AssignStatement& assign) {
            std::cout << std::string(indent, ' ')
                      << "AssignStatement: " << assign.name;
            if (assign.type.has_value())
              std::cout << " type: " << assign.type.value() << std::endl;
            else
              std::cout << " type: ?" << std::endl;

            std::cout << std::string(indent + 2, ' ') << "Value:" << std::endl;
            print_expression(assign.value, indent + 4);
          },
          [&](const StructDeclaration& struct_decl) {
            std::cout << std::string(indent, ' ') << "StructDeclaration: "
                      << (struct_decl.is_extern ? "extern " : "")
                      << struct_decl.name << std::endl;

            std::cout << std::string(indent + 2, ' ') << "Fields:" << std::endl;
            for (const auto& field : struct_decl.fields) {
              std::cout << std::string(indent + 4, ' ') << field.first << ": "
                        << field.second << std::endl;
            }

            std::cout << std::string(indent + 2, ' ')
                      << "Methods:" << std::endl;
            for (auto& method : struct_decl.methods) {
              std::cout << std::string(indent + 4, ' ') << method.first << ":"
                        << std::endl;
              print_function(method.second, indent + 6);
            }
          }},
      stmt.as);
}