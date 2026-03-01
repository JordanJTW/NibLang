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
  std::visit(Overloaded{[&](const std::string& type_name) { os << type_name; },
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
             type.type);

  return os;
}

static const char* ToString(FunctionKind kind) {
  switch (kind) {
    case Free:
      return "Free";
    case Extern:
      return "Extern";
    case Anonymous:
      return "Anonymous";
    case Method:
      return "Method";
    case StaticMethod:
      return "StaticMethod";
    case Constructor:
      return "Constructor";
  }
}

void print_resolved_call(const std::optional<ResolvedCall>& r, size_t indent) {
  if (!r.has_value()) {
    std::cout << std::string(indent, ' ') << "ResolvedCall: UNRESOLVED"
              << std::endl;
  } else {
    std::cout << std::string(indent, ' ') << "ResolvedCall:" << std::endl;
    std::cout << std::string(indent + 2, ' ')
              << "function_idx: " << r->function_idx << std::endl;
    std::cout << std::string(indent + 2, ' ')
              << "function_kind: " << ToString(r->kind) << std::endl;
  }
}

void print_resolved_access(const std::optional<ResolvedAccess>& r,
                           size_t indent) {
  if (!r.has_value()) {
    std::cout << std::string(indent, ' ') << "ResolvedAccess: UNRESOLVED"
              << std::endl;
  } else {
    std::cout << std::string(indent, ' ') << "ResolvedAccess:" << std::endl;
    std::cout << std::string(indent + 2, ' ') << "member_index: " << r->index
              << std::endl;
  }
}

void print_resolved_function(const std::optional<ResolvedFunction>& r,
                             size_t indent) {
  if (!r.has_value()) {
    std::cout << std::string(indent, ' ') << "ResolvedFunction: UNRESOLVED"
              << std::endl;
  } else {
    std::cout << std::string(indent, ' ') << "ResolvedFunction:" << std::endl;
    std::cout << std::string(indent + 2, ' ') << "call_idx: " << r->call_idx
              << std::endl;
    std::cout << std::string(indent + 2, ' ')
              << "is_void_return: " << (r->is_void_return ? "YES" : "NO")
              << std::endl;
  }
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

void print_function(const FunctionDeclaration& fn, size_t indent) {
  std::cout << std::string(indent, ' ') << "FunctionDeclaration: " << fn.name
            << std::endl;
  std::cout << std::string(indent + 2, ' ') << "Arguments:" << std::endl;
  for (const auto& arg : fn.arguments) {
    std::cout << std::string(indent + 4, ' ') << arg.first << ": " << arg.second
              << std::endl;
  }

  print_resolved_function(fn.resolved, indent + 2);

  if (fn.body) {
    std::cout << std::string(indent + 2, ' ') << "Body:" << std::endl;
    for (const auto& stmt : fn.body->statements)
      print_statement(*stmt, indent + 4);

  } else {
    std::cout << std::string(indent + 2, ' ') << " "
              << "Extern function, no body" << std::endl;
  }
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

            print_resolved_call(call.resolved, indent + 2);
          },
          [&](const MemberAccessExpression& member_access) {
            std::cout << std::string(indent, ' ')
                      << "MemberAccessExpression: " << member_access.member_name
                      << " type: " << expr->type << std::endl;
            std::cout << std::string(indent + 2, ' ') << "Object:" << std::endl;
            print_expression(member_access.object, indent + 4);
            print_resolved_access(member_access.resolved, indent + 2);
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
          [&](const ClosureExpression& closure) {
            std::cout << std::string(indent, ' ')
                      << "ClosureExpression: " << std::endl;
            print_function(closure.fn, indent + 2);
          },
          [&](const PrefixUnaryExpression& prefix) {
            std::cout << std::string(indent, ' ')
                      << "PrefixUnaryExpression (op=" << prefix.op
                      << ") type: " << expr->type << std::endl;
            print_expression(prefix.operand, indent + 2);
          },
          [&](const PostfixUnaryExpression& postfix) {
            std::cout << std::string(indent, ' ')
                      << "PostfixUnaryExpression (op=" << postfix.op
                      << ") type: " << expr->type << std::endl;
            print_expression(postfix.operand, indent + 2);
          },
          [&](const TypeCastExpression& cast) {
            std::cout << std::string(indent, ' ')
                      << "TypeCastExpression(type: " << expr->type
                      << "): " << std::endl;
            print_expression(cast.expr, indent + 2);
          }},
      expr->as);
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