#include "compiler/type_checker.h"

#include <iomanip>
#include <iostream>
#include <numeric>

#include "compiler/logging.h"

namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

enum LiteralType : TypeId { Void = 0, i32, f32, BOOL, STRING, kNextValue };

void print_error(const std::string& file, TextRange range) {
  size_t line_start = file.rfind('\n', range.first);
  if (line_start == std::string_view::npos)
    line_start = 0;
  else
    ++line_start;

  size_t line_end = file.find('\n', range.first);
  if (line_end == std::string_view::npos)
    line_end = file.size();

  std::string line = file.substr(line_start, line_end - line_start);
  size_t relative_offset = range.first - line_start;

  std::string kErrorPrefix = "Parsing: ";
  std::cerr << kErrorPrefix << line << std::endl;
}

}  // namespace

TypeChecker::TypeChecker(const std::string& file)
    : file_(file), next_type_id_(kNextValue) {
  string_to_type_ = {
      {"Void", LiteralType::Void},
      {"i32", LiteralType::i32},
      {"f32", LiteralType::f32},
      {"bool", LiteralType::BOOL},
  };

  scopes_.emplace_back();
}

void TypeChecker::Check(Block& block) {
  // Collect user-defined types to ensure forward-references resolve.
  for (auto& statement : block.statements) {
    if (auto* struct_decl = std::get_if<StructDeclaration>(&statement->as)) {
      TypeId type_id = next_type_id_++;
      string_to_type_[struct_decl->name] = type_id;
      scopes_.back().symbols[struct_decl->name] = Symbol{
          .kind = Symbol::Struct,
          .type_id = type_id,
      };
    }
  }

  // Collect signatures for functions and methods AFTER all types are known.
  for (auto& statement : block.statements) {
    if (auto* struct_decl = std::get_if<StructDeclaration>(&statement->as)) {
      StructType struct_type;
      struct_type.parsed_struct = struct_decl;

      MemberIdx next_member_idx = 0;
      for (const auto& [name, type] : struct_decl->fields) {
        if (auto type_id = GetIdFor({type}, CreateIfMissing::No)) {
          struct_type.member_types[name] = {next_member_idx++, type_id.value()};
          struct_type.field_members.push_back(type_id.value());
        }
      }
      for (auto& [name, fn] : struct_decl->methods) {
        if (auto type_id = DefineFunction(fn, FunctionKind::Method)) {
          // Methods are not stored within the struct memory so no index.
          struct_type.member_types[name] = {std::nullopt, type_id.value()};
        }
      }

      Symbol symbol = scopes_.back().symbols[struct_decl->name];
      type_info_[symbol.type_id] = std::move(struct_type);
    }

    if (auto* fn_decl = std::get_if<FunctionDeclaration>(&statement->as)) {
      if (std::optional<TypeId> type_id =
              DefineFunction(*fn_decl, FunctionKind::Free)) {
        string_to_type_[fn_decl->name] = type_id.value();
        scopes_.back().symbols[fn_decl->name] = Symbol{
            .kind = Symbol::Function,
            .type_id = type_id.value(),
        };
      }
    }
  }

  for (auto& statement : block.statements) {
    CheckStatement(statement);
  }
}

void TypeChecker::CheckStatement(std::unique_ptr<Statement>& statement) {
  std::visit(
      Overloaded{
          [&](std::unique_ptr<Expression>& expr) { CheckExpression(expr); },
          [&](const FunctionDeclaration& fn) { CheckFunctionBody(fn); },
          [&](ReturnStatement& ret) {
            TypeId return_type = CheckExpression(ret.value);

            const auto& expected_return_type = scopes_.back().return_type;
            if (expected_return_type.has_value()) {
              if (*expected_return_type != return_type) {
                LOG(ERROR) << "Function return type is " << return_type
                           << " but expected " << *expected_return_type;
              }
            }
          },
          [&](const ThrowStatement& thr) {}, [&](const IfStatement& if_stmt) {},
          [&](WhileStatement& while_stmt) {
            CheckExpression(while_stmt.condition);
            Check(while_stmt.body);
          },
          [&](const BreakStatement&) {}, [&](const ContinueStatement&) {},
          [&](AssignStatement& assign) {
            // Check this is the first assignment in this scope with that name.
            if (scopes_.back().symbols.find(assign.name) !=
                scopes_.back().symbols.end()) {
              LOG(WARNING) << "Identifier already declared with name: '"
                           << assign.name << "'";
            }

            // Ensure assignment expression's type matches the declared type (if
            // given, otherwise the variable's type is deduced from the value).
            std::optional<TypeId> expected_type;
            if (!assign.type.empty()) {
              expected_type = GetIdFor({assign.type}, CreateIfMissing::No);
            }
            TypeId value_type = CheckExpression(assign.value);

            if (expected_type.has_value() && *expected_type != value_type) {
              LOG(ERROR) << "Assigning " << value_type << " to "
                         << *expected_type;
            }

            // Register the variable's type within the current scope.
            TypeId type_id =
                expected_type.has_value() ? *expected_type : value_type;
            scopes_.back().symbols[assign.name] = {
                .kind = Symbol::Variable,
                .type_id = type_id,
            };
          },
          [&](const StructDeclaration& struct_decl) {
            Symbol symbol = scopes_.back().symbols[struct_decl.name];
            for (const auto& [name, fn] : struct_decl.methods) {
              CheckFunctionBody(fn);
            }
          }},
      statement->as);
}

TypeId TypeChecker::CheckExpression(std::unique_ptr<Expression>& expression) {
  expression->type = std::visit(
      Overloaded{
          [&](const PrimaryExpression& primary) -> TypeId {
            return std::visit(
                Overloaded{
                    [&](const StringLiteral&) -> TypeId {
                      if (auto symbol = GetSymbolFor("String"))
                        return symbol->type_id;
                      return LiteralType::Void;
                    },
                    [&](const Identifier& ident) -> TypeId {
                      if (auto symbol = GetSymbolFor(ident.name))
                        return symbol->type_id;
                      return LiteralType::Void;
                    },
                    [&](int32_t) -> TypeId { return LiteralType::i32; },
                    [&](float) -> TypeId { return LiteralType::f32; },
                    [&](bool) -> TypeId { return LiteralType::BOOL; },
                },
                primary.value);
          },
          [&](BinaryExpression& binary) -> TypeId {
            TypeId lhs = CheckExpression(binary.lhs);
            TypeId rhs = CheckExpression(binary.rhs);
            if (lhs != rhs)
              LOG(ERROR) << "LHS and RHS must be the same type";

            return lhs;
          },
          [&](CallExpression& call) -> TypeId {
            TypeId callee_type = CheckExpression(call.callee);

            auto& fn_type = type_info_[callee_type];
            if (std::holds_alternative<FunctionType>(fn_type)) {
              const FunctionType& f = std::get<FunctionType>(fn_type);

              // Account for the implicit "self" argument for method calls
              size_t extra_argc = 0;
              if (f.kind == FunctionKind::Method) {
                if (f.argument_types.size() == 0) {
                  LOG(ERROR) << "Methods must begin with a `self` argument";
                }
                extra_argc += 1;
              }

              if (call.arguments.size() + extra_argc !=
                  f.argument_types.size()) {
                LOG(ERROR) << "Wrong number of arguments";
              } else {
                for (size_t i = 0; i < call.arguments.size(); ++i) {
                  TypeId arg_type = CheckExpression(call.arguments[i]);
                  if (arg_type != f.argument_types[i + extra_argc])
                    LOG(ERROR) << "Argument type mismatch";
                }
              }

              call.resolved = ResolvedCall{f.call_idx, f.kind};
              return f.return_type;
            } else if (std::holds_alternative<StructType>(fn_type)) {
              const StructType& s = std::get<StructType>(fn_type);
              if (s.parsed_struct->is_extern) {
                LOG(ERROR) << "extern structs can not be instantiated";
                return LiteralType::Void;
              }
              call.resolved = ResolvedCall{0, FunctionKind::Constructor};
              if (s.field_members.size() != call.arguments.size()) {
                LOG(ERROR) << "Incorrect number of arguments to instantiate "
                              "struct: expected("
                           << s.field_members.size() << ") vs. provided("
                           << call.arguments.size() << ")";
              } else {
                for (size_t i = 0; i < call.arguments.size(); ++i) {
                  TypeId type_id = CheckExpression(call.arguments[i]);
                  if (type_id != s.field_members[i]) {
                    LOG(ERROR) << "Argument to new has incorrect type";
                  }
                }
              }
              return callee_type;
            } else {
              LOG(ERROR) << "Calling non-callable type";
              return LiteralType::Void;
            }
          },
          [&](const AssignmentExpression& assign) -> TypeId {
            return LiteralType::Void;
          },
          [&](MemberAccessExpression& member_access) -> TypeId {
            TypeId obj_id = CheckExpression(member_access.object);

            auto& struct_type = type_info_[obj_id];
            if (!std::holds_alternative<StructType>(struct_type)) {
              LOG(ERROR) << "Called object is not a struct";
              return LiteralType::Void;
            }

            const StructType& s = std::get<StructType>(struct_type);
            if (auto it = s.member_types.find(member_access.member_name);
                it != s.member_types.end()) {
              if (auto index = it->second.index_in_array) {
                member_access.resolved = ResolvedAccess{index.value()};
              }
              return it->second.type_id;
            }

            LOG(ERROR) << "No member '" << member_access.member_name << "' on "
                       << s.parsed_struct->name;
            return LiteralType::Void;
          },
          [&](const ArrayAccessExpression& array_access) -> TypeId {
            return LiteralType::Void;
          },
          [&](LogicExpression& logic) -> TypeId {
            TypeId lhs = CheckExpression(logic.lhs);
            TypeId rhs = CheckExpression(logic.rhs);

            if (lhs != rhs)
              LOG(ERROR) << "LHS and RHS must be the same type";

            return LiteralType::BOOL;
          },
          [&](NewExpression& new_expr) -> TypeId {
            TypeId type_id = string_to_type_[new_expr.struct_name];
            auto& struct_type = type_info_[type_id];
            if (!std::holds_alternative<StructType>(struct_type)) {
              LOG(ERROR) << "new only works with structs";
              return LiteralType::Void;
            }

            const StructType& s = std::get<StructType>(struct_type);
            if (s.parsed_struct->is_extern) {
              LOG(ERROR) << "extern structs can not be instantiated";
              return LiteralType::Void;
            }

            if (s.field_members.size() != new_expr.arguments.size()) {
              LOG(ERROR) << "Incorrect number of arguments to instantiate "
                            "struct: expected("
                         << s.field_members.size() << ") vs. provided("
                         << new_expr.arguments.size() << ")";
            } else {
              for (size_t i = 0; i < new_expr.arguments.size(); ++i) {
                TypeId type_id = CheckExpression(new_expr.arguments[i]);
                if (type_id != s.field_members[i]) {
                  LOG(ERROR) << "Argument to new has incorrect type";
                }
              }
            }
            return type_id;
          }},
      expression->as);

  return expression->type;
}

std::optional<TypeId> TypeChecker::GetIdFor(std::set<std::string> sumtype,
                                            CreateIfMissing create) {
  if (sumtype.size() == 1) {
    if (auto it = string_to_type_.find(*sumtype.begin());
        it != string_to_type_.end()) {
      return it->second;
    } else if (create == CreateIfMissing::Yes) {
      TypeId next_id = next_type_id_++;
      string_to_type_[*sumtype.begin()] = next_id;
      return next_id;
    } else {
      LOG(ERROR) << "Type not found: " << *sumtype.begin();
      return std::nullopt;
    }
  }

  std::string joined_sumtype = std::accumulate(
      sumtype.begin(), sumtype.end(), std::string{},
      [](std::string a, std::string b) { return std::move(a) + "|" + b; });

  return GetIdFor({joined_sumtype}, create);
}

std::optional<TypeId> TypeChecker::DefineFunction(FunctionDeclaration& fn,
                                                  FunctionKind kind) {
  TypeId type_id = next_type_id_++;

  std::vector<TypeId> typed_arguments;
  for (const auto& [name, type] : fn.arguments) {
    if (auto id = GetIdFor({type}, CreateIfMissing::No); id.has_value()) {
      typed_arguments.push_back(*id);
    } else {
      return std::nullopt;
    }
  }
  std::optional<TypeId> return_type = GetIdFor(
      std::set<std::string>({fn.return_types.begin(), fn.return_types.end()}),
      CreateIfMissing::No);
  if (!return_type.has_value())
    return std::nullopt;

  CallIdx call_idx = next_call_idx_++;
  fn.call_idx = call_idx;
  type_info_[type_id] =
      FunctionType{typed_arguments, *return_type, kind, call_idx};
  return type_id;
}

void TypeChecker::CheckFunctionBody(const FunctionDeclaration& fn) {
  if (!fn.body)
    return;

  Scope& function_scope = scopes_.emplace_back();
  function_scope.return_type = GetIdFor(
      std::set<std::string>({fn.return_types.begin(), fn.return_types.end()}),
      CreateIfMissing::No);

  for (const auto& [name, type] : fn.arguments) {
    std::optional<TypeId> type_id = GetIdFor({type}, CreateIfMissing::No);
    if (!type_id.has_value())
      return;

    function_scope.symbols[name] = {
        .kind = Symbol::Variable,
        .type_id = *type_id,
    };
  }

  Check(*fn.body);
  scopes_.pop_back();
}

std::optional<TypeChecker::Symbol> TypeChecker::GetSymbolFor(
    const std::string& name) {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    if (auto found = it->symbols.find(name); found != it->symbols.end()) {
      return found->second;
    }
  }
  LOG(ERROR) << "Unknown identifier: " << name;
  return std::nullopt;
}