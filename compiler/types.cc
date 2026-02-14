#include "compiler/types.h"

#include <iomanip>
#include <iostream>

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

static void print_primary(const PrimaryExpression& primary, size_t indent) {
  std::visit(
      Overloaded{
          [&](const StringLiteral& str) {
            std::cout << std::setw(indent * 2) << " " << "StringLiteral: \""
                      << str.value << "\"" << std::endl;
          },
          [&](const Identifier& ident) {
            std::cout << std::setw(indent * 2) << " "
                      << "Identifier: " << ident.name << std::endl;
          },
          [&](int32_t i32) {
            std::cout << std::setw(indent * 2) << " " << "Int32: " << i32
                      << std::endl;
          },
          [&](float f32) {
            std::cout << std::setw(indent * 2) << "Float: " << f32 << std::endl;
          },
          [&](bool b) {
            std::cout << std::setw(indent * 2) << " "
                      << "Bool: " << (b ? "true" : "false") << std::endl;
          },
      },
      primary);
}

static void print_expression(const Expression& expr, size_t indent) {
  std::visit(
      Overloaded{[&](const PrimaryExpression& primary) {
                   print_primary(primary, indent);
                 },
                 [&](const BinaryExpression& binary) {
                   std::cout
                       << std::setw(indent * 2) << " "
                       << "BinaryExpression (op=" << static_cast<int>(binary.op)
                       << ")" << std::endl;

                   std::cout << std::setw(indent * 2) << " " << "LHS:\n";
                   print_expression(*binary.lhs, indent + 1);

                   std::cout << std::setw(indent * 2) << " " << "RHS:\n";
                   print_expression(*binary.rhs, indent + 1);
                 },
                 [&](const AssignmentExpression& assign) {
                   std::cout << std::setw(indent * 2) << " "
                             << "AssignmentExpression: " << assign.variable_name
                             << std::endl;

                   std::cout << std::setw(indent * 2) << " " << "RHS:\n";
                   print_expression(*assign.rhs, indent + 1);
                 },
                 [&](const CallExpression& call) {
                   std::cout << std::setw(indent * 2) << " "
                             << "CallExpression: " << call.fn_name << std::endl;

                   std::cout << std::setw(indent * 2) << " " << "Arguments:\n";
                   for (const auto& arg : call.arguments)
                     print_expression(*arg, indent + 1);
                 }},
      expr.as);
}

void print_statement(const Statement& stmt, size_t indent) {
  std::visit(Overloaded{[&](const std::unique_ptr<Expression>& expr) {
                          std::cout << std::setw(indent * 2) << " "
                                    << "ExpressionStatement:" << std::endl;
                          print_expression(*expr, indent + 1);
                        },
                        [&](const FunctionDeclaration& fn) {
                          std::cout << std::setw(indent * 2) << " "
                                    << "FunctionDeclaration: " << fn.name
                                    << std::endl;

                          std::cout << std::setw(indent * 2) << " "
                                    << "Arguments:" << std::endl;
                          for (const auto& arg : fn.arguments) {
                            std::cout << std::setw(indent * 2) << " "
                                      << arg.first << " : " << arg.second
                                      << std::endl;
                          }

                          std::cout << std::setw(indent * 2) << " "
                                    << "Body:" << std::endl;
                          for (const auto& stmt : fn.body.statements)
                            print_statement(*stmt, indent + 1);
                        },
                        [&](const ReturnStatement& ret) {
                          std::cout << std::setw(indent * 2) << " "
                                    << "ReturnStatement:" << std::endl;
                          print_expression(*ret.value, indent + 1);
                        },
                        [&](const ThrowStatement& thr) {
                          std::cout << std::setw(indent * 2) << " "
                                    << "ThrowStatement:" << std::endl;
                          print_expression(*thr.value, indent + 1);
                        },
                        [&](const IfStatement& if_stmt) {
                          std::cout << std::setw(indent * 2) << " "
                                    << "IfStatement:" << std::endl;

                          std::cout << std::setw(indent * 2) << " "
                                    << "Condition:" << std::endl;
                          print_expression(*if_stmt.condition, indent + 1);

                          std::cout << std::setw(indent * 2) << " "
                                    << "Then Body:" << std::endl;
                          for (const auto& stmt : if_stmt.then_body.statements)
                            print_statement(*stmt, indent + 1);

                          std::cout << std::setw(indent * 2) << " "
                                    << "Else Body:" << std::endl;
                          for (const auto& stmt : if_stmt.else_body.statements)
                            print_statement(*stmt, indent + 1);
                        },
                        [&](const WhileStatement& while_stmt) {
                          std::cout << std::setw(indent * 2) << " "
                                    << "WhileStatement:" << std::endl;

                          std::cout << std::setw(indent * 2) << " "
                                    << "Condition:" << std::endl;
                          print_expression(*while_stmt.condition, indent + 1);

                          std::cout << std::setw(indent * 2) << " "
                                    << "Body:" << std::endl;
                          for (const auto& stmt : while_stmt.body.statements)
                            print_statement(*stmt, indent + 1);
                        }},
             stmt.as);
}