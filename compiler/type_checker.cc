#include "compiler/type_checker.h"

#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>

#include "compiler/logging.h"
#include "src/vm.h"

namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

enum LiteralType : TypeId { Void = 0, i32, f32, Bool, Any, kCount };

void print_error(const std::string& file,
                 Metadata metadata,
                 std::string_view message) {
  size_t line_start = file.rfind('\n', metadata.text_range.start);
  if (line_start == std::string_view::npos)
    line_start = 0;
  else
    ++line_start;

  size_t line_end = file.find('\n', metadata.text_range.start);
  if (line_end == std::string_view::npos)
    line_end = file.size();

  std::string line = file.substr(line_start, line_end - line_start);
  size_t relative_offset = metadata.text_range.start - line_start;

  std::string kErrorPrefix =
      "Error: " + std::to_string(metadata.line_range.start) + ": ";
  std::cerr << kErrorPrefix << line << std::endl
            << std::setw(kErrorPrefix.length() + relative_offset) << " "
            << "^ " << message << std::endl;
}

}  // namespace

TypeChecker::TypeChecker(const std::string& file)
    : file_(file), next_type_id_(LiteralType::kCount) {
  scopes_.emplace_back();

  // Add dummy entries for the built-in types to simplify logic.
  for (size_t i = 0; i < LiteralType::kCount; ++i)
    type_info_[i] = BuiltInType{};
}

void TypeChecker::Check(Block& block) {
  // Collect user-defined types to ensure forward-references resolve.
  for (auto& statement : block.statements) {
    if (auto* struct_decl = std::get_if<StructDeclaration>(&statement->as)) {
      TypeId type_id = next_type_id_++;
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
        if (auto type_id = GetTypeIdFor(type)) {
          struct_type.member_types[name] = {next_member_idx++, type_id.value()};
          struct_type.field_members.push_back(type_id.value());
        }
      }
      for (auto& [name, fn] : struct_decl->methods) {
        if (auto type_id = DefineFunction(fn, struct_decl)) {
          // Methods are not stored within the struct memory so no index.
          struct_type.member_types[name] = {std::nullopt, type_id.value()};
        }
      }

      // External structs can be constructed with a provided native function.
      if (struct_decl->is_extern) {
        struct_type.constructor_call_idx =
            GetCallIdxFor(struct_decl->name + "_new", CreateIfMissing::NO);
      }

      Symbol symbol = scopes_.back().symbols[struct_decl->name];
      type_info_[symbol.type_id] = std::move(struct_type);
    }

    if (auto* fn_decl = std::get_if<FunctionDeclaration>(&statement->as)) {
      if (std::optional<TypeId> type_id = DefineFunction(*fn_decl)) {
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
            if (expected_return_type.has_value() &&
                !IsTypeSubsetOf(return_type, expected_return_type.value())) {
              LOG(ERROR) << "Function return type is " << return_type
                         << " but expected " << *expected_return_type;
            }
          },
          [&](ThrowStatement& thr) { CheckExpression(thr.value); },
          [&](IfStatement& if_stmt) {
            CheckExpression(if_stmt.condition);
            Check(if_stmt.then_body);
            Check(if_stmt.else_body);
          },
          [&](WhileStatement& while_stmt) {
            CheckExpression(while_stmt.condition);
            Check(while_stmt.body);
          },
          // `break` and `continue` are single word statements.
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
            if (assign.type.has_value())
              expected_type = GetTypeIdFor(*assign.type);

            TypeId value_type = CheckExpression(assign.value);

            if (expected_type.has_value() &&
                !IsTypeSubsetOf(value_type, *expected_type)) {
              print_error(file_, statement->meta,
                          "Assigning " + std::to_string(value_type) + " to " +
                              std::to_string(*expected_type));
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
          [&](PrimaryExpression& primary) -> TypeId {
            return std::visit(
                Overloaded{
                    [&](const StringLiteral&) -> TypeId {
                      if (auto symbol =
                              GetSymbolFor("String", expression->meta))
                        return symbol->type_id;
                      return LiteralType::Void;
                    },
                    [&](Identifier& ident) -> TypeId {
                      if (auto symbol =
                              GetSymbolFor(ident.name, expression->meta)) {
                        switch (symbol->kind) {
                          case Symbol::Kind::Function: {
                            Type type = type_info_[symbol->type_id];
                            auto& fn_type = std::get<FunctionType>(type);

                            ident.resolved = ResolvedIdentifier{
                                ResolvedIdentifier::Function, fn_type.call_idx};
                            break;
                          }
                          case Symbol::Kind::Variable:
                            ident.resolved =
                                ResolvedIdentifier{ResolvedIdentifier::Value};
                            break;
                          case Symbol::Kind::Struct:
                            ident.resolved = ResolvedIdentifier{
                                ResolvedIdentifier::TypeName};
                            break;
                        }
                        return symbol->type_id;
                      }
                      return LiteralType::Void;
                    },
                    [&](int32_t) -> TypeId { return LiteralType::i32; },
                    [&](float) -> TypeId { return LiteralType::f32; },
                    [&](bool) -> TypeId { return LiteralType::Bool; },
                },
                primary.value);
          },
          [&](BinaryExpression& binary) -> TypeId {
            TypeId lhs = CheckExpression(binary.lhs);
            TypeId rhs = CheckExpression(binary.rhs);
            if (lhs != rhs)
              LOG(ERROR) << "LHS and RHS must be the same type";

            // Comparison operators will always generate a boolean
            if (binary.op == TokenKind::kCompareGt ||
                binary.op == TokenKind::kCompareLt ||
                binary.op == TokenKind::kCompareGe ||
                binary.op == TokenKind::kCompareLe ||
                binary.op == TokenKind::kCompareEq ||
                binary.op == TokenKind::kCompareNe)
              return LiteralType::Bool;

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

              size_t expected_argc = f.argument_types.size();
              size_t actual_argc = call.arguments.size() + extra_argc;

              if ((f.is_variadic && actual_argc < expected_argc) ||
                  (!f.is_variadic && actual_argc != expected_argc)) {
                print_error(file_, expression->meta,
                            "Wrong number of arguments");
              } else {
                for (size_t i = 0; i < call.arguments.size(); ++i) {
                  TypeId arg_type = CheckExpression(call.arguments[i]);
                  TypeId expected_type = LiteralType::Any;
                  if (i + extra_argc < f.argument_types.size())
                    expected_type = f.argument_types[i + extra_argc];
                  if (!IsTypeSubsetOf(arg_type, expected_type)) {
                    print_error(file_, expression->meta,
                                "Argument type mismatch");
                  }
                }
              }
              call.resolved = ResolvedCall{f.call_idx, f.kind};
              return f.return_type;
            } else if (std::holds_alternative<StructType>(fn_type)) {
              const StructType& s = std::get<StructType>(fn_type);
              if (s.parsed_struct->is_extern &&
                  s.constructor_call_idx.has_value()) {
                call.resolved =
                    ResolvedCall{*s.constructor_call_idx, FunctionKind::Free};
                // Constructor function signature should be stored and type
                // checked but this requires refactoring out the above logic.
                return callee_type;
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
                  if (!IsTypeSubsetOf(type_id, s.field_members[i])) {
                    LOG(ERROR) << "Argument to new has incorrect type";
                  }
                }
              }
              return callee_type;
            } else {
              LOG(ERROR) << "Calling non-callable type: " << callee_type;
              return LiteralType::Void;
            }
          },
          [&](AssignmentExpression& assign) -> TypeId {
            TypeId lhs = CheckExpression(assign.lhs);
            TypeId rhs = CheckExpression(assign.rhs);

            if (lhs != rhs)
              print_error(file_, expression->meta, "Mismatched assignment");

            return lhs;
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

            return LiteralType::Bool;
          },
          [&](NewExpression& new_expr) -> TypeId {
            std::optional<Symbol> symbol =
                GetSymbolFor(new_expr.struct_name, expression->meta);

            // Error is logged within GetSymbolFor
            if (!symbol.has_value())
              return LiteralType::Void;

            if (symbol->kind != Symbol::Struct) {
              LOG(ERROR) << "new only works with structs";
              return LiteralType::Void;
            }

            auto& struct_type = type_info_[symbol->type_id];
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
                if (!IsTypeSubsetOf(type_id, s.field_members[i])) {
                  LOG(ERROR) << "Argument to new has incorrect type";
                }
              }
            }
            return symbol->type_id;
          },
          [&](ClosureExpression& closure) -> TypeId {
            if (std::optional<TypeId> type_id = DefineFunction(closure.fn)) {
              CheckFunctionBody(closure.fn);
              return *type_id;
            }
            return LiteralType::Void;
          }},
      expression->as);

  return expression->type;
}

std::optional<TypeId> TypeChecker::GetTypeIdFor(ParsedType type) {
  return std::visit(
      Overloaded{
          [&](const ParsedTypeName& type) -> std::optional<TypeId> {
            static const std::unordered_map<std::string, TypeId> kBuiltInTypes =
                {
                    {"Void", LiteralType::Void}, {"i32", LiteralType::i32},
                    {"f32", LiteralType::f32},   {"bool", LiteralType::Bool},
                    {"any", LiteralType::Any},
                };

            // Fast path for built-in type names which are used frequently.
            if (const auto& it = kBuiltInTypes.find(type.name);
                it != kBuiltInTypes.end()) {
              return it->second;
            }

            // Structure types are resolved nominally, while Functions would be
            // resolved structurally to allow for flexible callbacks, etc.
            if (auto symbol = GetSymbolFor(type.name, type.metadata);
                symbol.has_value() && symbol->kind == Symbol::Struct) {
              return symbol->type_id;
            }
            return std::nullopt;
          },
          [&](const ParsedUnionType& type) -> std::optional<TypeId> {
            // Look-up member types and normalize (sorted/dedupe) using set.
            std::set<TypeId> normalized_types;
            for (const auto& name : type.names) {
              std::optional<TypeId> member_type = GetTypeIdFor(name);
              if (!member_type.has_value())
                return std::nullopt;
              normalized_types.insert(member_type.value());
            }
            CHECK_GT(normalized_types.size(), 1)
                << "Parser returned a weird ParsedUnionType";

            // Intern unions structurally to a TypeId for faster comparisons.
            // UnionType stores the types as a std::vector instead of a std::set
            // to take advatange of better cache-locality and moves from the
            // contiguous memory (as opposed to the tree used by std::set).
            auto key =
                UnionType{{normalized_types.begin(), normalized_types.end()}};
            if (const auto& it = interned_union_type_.find(key);
                it != interned_union_type_.end()) {
              return it->second;
            }

            TypeId type_id = next_type_id_++;
            // Ensure UnionType can be looked-up later for comparisons.
            type_info_[type_id] = key;
            interned_union_type_[key] = type_id;
            return type_id;
          },
      },
      type);
}

std::optional<TypeId> TypeChecker::DefineFunction(
    FunctionDeclaration& fn,
    std::optional<StructDeclaration*> object) {
  TypeId type_id = next_type_id_++;

  std::vector<TypeId> typed_arguments;
  for (const auto& [name, type] : fn.arguments) {
    if (auto id = GetTypeIdFor(type); id.has_value()) {
      typed_arguments.push_back(*id);
    } else {
      return std::nullopt;
    }
  }
  std::optional<TypeId> return_type = GetTypeIdFor(fn.return_type);
  if (!return_type.has_value())
    return std::nullopt;

  std::string qualified_name = fn.name;
  bool is_function_extern = false;
  if (object) {
    qualified_name = object.value()->name + "_" + fn.name;
    is_function_extern = object.value()->is_extern;
  }

  // Ensure that extern (built-in) functions that do not exist print an error
  // instead of blindly creating a new CallIdx for them :^)
  std::optional<CallIdx> call_idx =
      GetCallIdxFor(qualified_name, is_function_extern ? CreateIfMissing::NO
                                                       : CreateIfMissing::YES);
  if (!call_idx.has_value()) {
    LOG(ERROR) << "Function " << qualified_name << " is not found";
    return std::nullopt;
  }

  fn.resolved = ResolvedFunction{*call_idx, return_type == LiteralType::Void};
  type_info_[type_id] = FunctionType{
      typed_arguments, *return_type,
      object.has_value() ? FunctionKind::Method : FunctionKind::Free,
      call_idx.value(), fn.is_variadic};
  return type_id;
}

void TypeChecker::CheckFunctionBody(const FunctionDeclaration& fn) {
  if (!fn.body)
    return;

  Scope& function_scope = scopes_.emplace_back();
  function_scope.return_type = GetTypeIdFor(fn.return_type);

  for (const auto& [name, type] : fn.arguments) {
    std::optional<TypeId> type_id = GetTypeIdFor(type);
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

std::optional<CallIdx> TypeChecker::GetCallIdxFor(const std::string& name,
                                                  CreateIfMissing create) {
  static const std::map<std::string, CallIdx> kBuiltInCallIdx = {
      {"Array_get", VM_BUILTIN_ARRAY_GET},
      {"Array_init", VM_BUILTIN_ARRAY_INIT},
      {"Array_new", VM_BUILTIN_ARRAY_NEW},
      {"Array_set", VM_BUILTIN_ARRAY_SET},
      {"Log", VM_BUILTIN_LOG},
      {"Map_new", VM_BUILTIN_MAP_NEW},
      {"Map_set", VM_BUILTIN_MAP_SET},
      {"Math_pow", VM_BUILTIN_MATH_POW},
      {"Promise_fulfill", VM_BUILTIN_PROMISE_FULFILL},
      {"Promise_new", VM_BUILTIN_PROMISE_NEW},
      {"Promise_reject", VM_BUILTIN_PROMISE_REJECT},
      {"Promise_then", VM_BUILTIN_PROMISE_THEN},
      {"String_charAt", VM_BUILTIN_STRINGS_GET},
      {"String_length", VM_BUILTIN_STRING_LENGTH},
      {"String_startsWith", VM_BUILTIN_STRINGS_STARTWITH},
      {"String_substr", VM_BUILTIN_STRINGS_SUBSTRING},
  };

  if (auto it = kBuiltInCallIdx.find(name); it != kBuiltInCallIdx.end())
    return it->second;

  if (create == CreateIfMissing::YES)
    return next_call_idx_++;

  return std::nullopt;
}

std::optional<TypeChecker::Symbol> TypeChecker::GetSymbolFor(
    const std::string& name,
    Metadata metadata) {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    if (auto found = it->symbols.find(name); found != it->symbols.end()) {
      return found->second;
    }
  }

  print_error(file_, metadata, "Unknown identifier: " + name);
  return std::nullopt;
}

bool TypeChecker::IsTypeSubsetOf(TypeId sub_type_id, TypeId super_type_id) {
  if (sub_type_id == super_type_id || super_type_id == LiteralType::Any)
    return true;

  const Type& sub_type = type_info_.at(sub_type_id);
  const Type& super_type = type_info_.at(super_type_id);

  if (!std::holds_alternative<UnionType>(sub_type) &&
      std::holds_alternative<UnionType>(super_type)) {
    const auto& super_types = std::get<UnionType>(super_type).types;
    return std::binary_search(super_types.begin(), super_types.end(),
                              sub_type_id);
  }

  if (std::holds_alternative<UnionType>(sub_type) &&
      !std::holds_alternative<UnionType>(super_type)) {
    return false;  // a union cannot be subset of a single concrete type
  }

  if (std::holds_alternative<UnionType>(sub_type) &&
      std::holds_alternative<UnionType>(super_type)) {
    const auto& sub_types = std::get<UnionType>(sub_type).types;
    const auto& super_types = std::get<UnionType>(super_type).types;

    return std::all_of(sub_types.begin(), sub_types.end(), [&](TypeId id) {
      return std::binary_search(super_types.begin(), super_types.end(), id);
    });
  }

  // Neither type is a union so they must be different concrete types.
  return false;
}