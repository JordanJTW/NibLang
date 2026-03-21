#include "compiler/type_context.h"

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

inline void ComputeHash(size_t& seed, TypeId value) {
  seed ^= std::hash<TypeId>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

}  // namespace

TypeContext::TypeContext(ErrorCollector& error_collector)
    : error_collector_(error_collector) {
  // Add dummy entries for the built-in types to simplify logic.
  for (size_t i = 0; i < LiteralType::kCount; ++i)
    type_lookup_[i] = BuiltInType{};

  static FunctionDeclaration main_fn{
      .name = "main",
      .arguments = {},
      .return_type = ParsedType{"Void"},
      .function_kind = FunctionKind::Free,
      // Technically all top-level statements are in <main> but TypeContext
      // only cares if a function _has_ a body or not for type checking.
      .body = std::make_unique<Block>(),
  };

  // Enter "global" function scope by default to allow for top-level statements.
  EnterScope(ScopeType::FunctionScope, &main_fn);
  auto main_symbol = DefineFunction(main_fn);
  CHECK(main_symbol.has_value());
}

void TypeContext::EnterScope(ScopeType type, FunctionDeclaration* fn) {
  if (type == ScopeType::FunctionScope) {
    current_fn_scope_idx_ = scopes_.size();
    // LOG(INFO) << "Entering function scope for: " << (fn ? fn->name :
    // "<main>");
  }

  scopes_.push_back({type, fn});
}

void TypeContext::ExitScope() {
  if (scopes_.empty())
    return;

  // if (scopes_.back().scope_type == ScopeType::FunctionScope) {
  //   LOG(INFO) << "Exiting function scope for: "
  //             << (scopes_.back().fn ? scopes_.back().fn->name : "<main>");
  // }

  scopes_.pop_back();

  current_fn_scope_idx_ = -1;
  for (int i = scopes_.size() - 1; i >= 0; --i) {
    if (scopes_[i].scope_type == ScopeType::FunctionScope) {
      current_fn_scope_idx_ = i;
      break;
    }
  }
}

FunctionDeclaration& TypeContext::GetCurrentFunction() {
  CHECK(current_fn_scope_idx_ < scopes_.size());
  CHECK(scopes_[current_fn_scope_idx_].fn != nullptr);
  return *scopes_[current_fn_scope_idx_].fn;
}

Symbol TypeContext::DeclareStructSymbol(std::string_view name) {
  return InsertSymbol(name, Symbol::Struct, next_type_id_++);
}

void TypeContext::DefineStructType(TypeId self_id, StructDeclaration& decl) {
  StructType struct_type;
  // Structs are nominally typed so we can simplify later logic by storing
  // "symbolic" information directly on the type (i.e. StructDeclaration).
  struct_type.struct_declaration = &decl;

  Symbol::Idx field_idx = 0;
  for (const auto& [name, type] : decl.fields) {
    auto type_id = GetTypeIdFor(type);
    if (!type_id.has_value()) {
      error_collector_.Add("Unknown type for struct field: " + name,
                           type.metadata);
      continue;
    }

    auto it = struct_type.member_symbols.find(name);
    if (it != struct_type.member_symbols.end()) {
      error_collector_.Add("Duplicate field name in struct: " + name,
                           type.metadata);
      continue;
    }

    struct_type.member_symbols[name] =
        Symbol{Symbol::Field, type_id.value(), field_idx++};
    struct_type.field_types.push_back(type_id.value());
  }

  for (auto& [name, fn] : decl.methods) {
    // Errors are logged from within `DefineFunction`.
    if (auto method_symbol = DefineFunction(fn, &decl, self_id);
        method_symbol.has_value()) {
      struct_type.member_symbols[name] = method_symbol.value();
    }
  }

  type_lookup_[self_id] = struct_type;
}

std::optional<Symbol> TypeContext::DefineFunction(
    FunctionDeclaration& fn,
    std::optional<StructDeclaration*> object,
    std::optional<TypeId> self_id) {
  std::string qualified_name = fn.name;
  bool is_function_extern = fn.function_kind == FunctionKind::Extern;
  if (object) {
    qualified_name = object.value()->name + "_" + fn.name;
    is_function_extern = object.value()->is_extern && !fn.body;
  }

  std::optional<Symbol::Idx> call_idx;
  // Extern (native) functions must have a hardcoded CallIdx already assigned.
  if (is_function_extern) {
    call_idx = GetCallIdxFor(qualified_name, CreateIfMissing::NO);
    if (!call_idx) {
      LOG(ERROR) << "extern function '" << qualified_name << "' is not found";
      return std::nullopt;
    }
  } else {
    if (!fn.body) {
      LOG(ERROR) << "non-extern functions MUST have a body: " << fn.name;
      return std::nullopt;
    }

    call_idx = GetCallIdxFor(qualified_name, CreateIfMissing::YES);
  }

  if (std::optional<TypeId> type_id = DeclareFunctionType(fn, self_id)) {
    Symbol symbol = InsertSymbol(fn.name, Symbol::Function, type_id.value(),
                                 call_idx.value());
    function_lookup_[call_idx.value()] = &fn;
    fn.resolved = ResolvedFunction{symbol};
    return symbol;
  }
  return std::nullopt;
}

Symbol TypeContext::DeclareVariableSymbol(std::string_view name,
                                          TypeId type_id) {
  return InsertSymbol(name, Symbol::Variable, type_id,
                      scopes_[current_fn_scope_idx_].next_symbol_idx++);
}

Symbol TypeContext::DeclareCaptureSymbol(std::string_view name,
                                         TypeId type_id) {
  return InsertSymbol(name, Symbol::Capture, type_id,
                      scopes_[current_fn_scope_idx_].next_symbol_idx++);
}

Symbol TypeContext::InsertSymbol(std::string_view name,
                                 Symbol::Kind kind,
                                 TypeId type_id,
                                 std::optional<Symbol::Idx> idx) {
  Symbol symbol = {kind, type_id, idx};
  scopes_.back().symbols[name.data()] = symbol;
  return symbol;
}

std::optional<Symbol> TypeContext::GetSymbolFor(std::string_view name,
                                                ScopeToCheck scope) const {
  bool encountered_first_function_scope = false;
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    if (auto found = it->symbols.find(name.data());
        found != it->symbols.end()) {
      return found->second;
    }

    if (it->scope_type == ScopeType::FunctionScope) {
      if (scope == ScopeToCheck::Function)
        break;
      
      if (scope == ScopeToCheck::Closure && encountered_first_function_scope)
        break;

      encountered_first_function_scope = true;
    }

    if (scope == ScopeToCheck::Current)
      break;
  }
  return std::nullopt;
}

std::optional<TypeId> TypeContext::GetTypeIdFor(const ParsedType& type) {
  return std::visit(
      Overloaded{
          [&](const std::string& type_name) -> std::optional<TypeId> {
            static const std::unordered_map<std::string, TypeId> kBuiltInTypes =
                {
                    {"Void", LiteralType::Void}, {"i32", LiteralType::i32},
                    {"f32", LiteralType::f32},   {"bool", LiteralType::Bool},
                    {"any", LiteralType::Any}, {"Nil", LiteralType::Nil},
                };

            // Fast path for built-in type names which are used frequently.
            if (const auto& it = kBuiltInTypes.find(type_name);
                it != kBuiltInTypes.end()) {
              return it->second;
            }

            // Structure types are resolved nominally, while Functions would be
            // resolved structurally to allow for flexible callbacks, etc.
            if (auto symbol = GetSymbolFor(type_name);
                symbol.has_value() && symbol->kind == Symbol::Struct) {
              return symbol->type_id;
            }
            return std::nullopt;
          },
          [&](const ParsedUnionType& type) -> std::optional<TypeId> {
            // Look-up member types and normalize (sort/dedupe/flatten).
            std::set<TypeId> normalized_types;  // std::set sorts and dedupes
            for (const auto& name : type.names) {
              std::optional<TypeId> member_type = GetTypeIdFor(name);
              if (!member_type.has_value())
                return std::nullopt;

              // If the member itself is a union, flatten it.
              if (auto* u = GetTypeInfo<UnionType>(member_type.value())) {
                normalized_types.insert(u->types.begin(), u->types.end());
              } else {
                normalized_types.insert(member_type.value());
              }
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
            interned_union_type_[key] = type_id;
            type_lookup_[type_id] = key;
            return type_id;
          },
          [&](const ParsedFunctionType& type) -> std::optional<TypeId> {
            std::vector<TypeId> arg_types;
            for (const auto& arg : type.arguments) {
              if (auto id = GetTypeIdFor(arg); id.has_value()) {
                arg_types.push_back(*id);
              } else {
                return std::nullopt;
              }
            }

            std::optional<TypeId> return_type =
                type.return_value ? GetTypeIdFor(*type.return_value)
                                  : LiteralType::Void;
            if (!return_type.has_value())
              return std::nullopt;

            FunctionType key = {std::move(arg_types), return_type.value()};
            if (const auto& it = interned_fn_type_.find(key);
                it != interned_fn_type_.end()) {
              return it->second;
            }

            TypeId type_id = next_type_id_++;
            interned_fn_type_[key] = type_id;
            type_lookup_[type_id] = key;
            return type_id;
          }},
      type.type);
}

std::optional<TypeId> TypeContext::DeclareFunctionType(
    const FunctionDeclaration& fn,
    std::optional<TypeId> self_id) {
  std::vector<TypeId> argument_types;
  for (const auto& [name, type] : fn.arguments) {
    if (auto id = GetTypeIdFor(type); id.has_value()) {
      argument_types.push_back(*id);
    } else {
      // TODO: Print error message on bad argument type.
      return std::nullopt;
    }
  }

  if (fn.function_kind == FunctionKind::Method &&
      (argument_types.size() == 0 || argument_types[0] != self_id)) {
    error_collector_.Add("Methods must begin with a `self` argument",
                         fn.argument_range);
    return std::nullopt;
  }

  std::optional<TypeId> return_type = GetTypeIdFor(fn.return_type);
  // Missing return types are already handled in the Parser (resolving to
  // Void) so if `return_type` has no value here it is truly an unknown type.
  if (!return_type.has_value()) {
    // TODO: Print error message on bad return type.
    return std::nullopt;
  }

  auto key = FunctionType{std::move(argument_types), return_type.value(),
                          fn.is_variadic};
  if (const auto& it = interned_fn_type_.find(key);
      it != interned_fn_type_.end()) {
    return it->second;
  }

  TypeId type_id = next_type_id_++;
  interned_fn_type_[key] = type_id;
  type_lookup_[type_id] = key;
  return type_id;
}

// static
bool TypeContext::IsTypeEquivalent(TypeId lhs, TypeId rhs) {
  if (lhs == rhs)
    return true;

  return (lhs == TypeContext::LiteralType::i32 ||
          lhs == TypeContext::LiteralType::f32) &&
         (rhs == TypeContext::LiteralType::i32 ||
          rhs == TypeContext::LiteralType::f32);
}

bool TypeContext::IsTypeSubsetOf(TypeId sub_type_id, TypeId super_type_id) {
  if (IsTypeEquivalent(sub_type_id, super_type_id) ||
      super_type_id == LiteralType::Any)
    return true;

  const TypeInfo& sub_type = type_lookup_.at(sub_type_id);
  const TypeInfo& super_type = type_lookup_.at(super_type_id);

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

std::optional<Symbol::Idx> TypeContext::GetCallIdxFor(const std::string& name,
                                                      CreateIfMissing create) {
  static const std::map<std::string, Symbol::Idx> kBuiltInCallIdx = {
      {"Array_get", VM_BUILTIN_ARRAY_GET},
      {"Array_init", VM_BUILTIN_ARRAY_INIT},
      {"Array_length", VM_BUILTIN_ARRAY_LENGTH},
      {"Array_new", VM_BUILTIN_ARRAY_NEW},
      // Alias for the argument version of the native function.
      {"Array_withSize", VM_BUILTIN_ARRAY_NEW},
      {"Array_push", VM_BUILTIN_ARRAY_PUSH},
      {"Array_set", VM_BUILTIN_ARRAY_SET},
      {"Map_new", VM_BUILTIN_MAP_NEW},
      {"Map_get", VM_BUILTIN_MAP_GET},
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
      {"log", VM_BUILTIN(18)},
      {"fetch", VM_BUILTIN(19)},
      {"to_string", VM_BUILTIN(20)},
  };

  if (auto it = kBuiltInCallIdx.find(name); it != kBuiltInCallIdx.end())
    return it->second;

  if (create == CreateIfMissing::YES)
    return next_call_idx_++;

  return std::nullopt;
}

bool FunctionType::operator==(const FunctionType& other) const {
  return other.arg_types == arg_types && other.return_type == return_type;
}

size_t FunctionType::Hash::operator()(const FunctionType& key) const {
  size_t hash = 0;
  ComputeHash(hash, key.arg_types.size());
  ComputeHash(hash, key.return_type);
  ComputeHash(hash, key.is_variadic);

  for (TypeId arg : key.arg_types)
    ComputeHash(hash, arg);
  return hash;
}

bool UnionType::operator==(const UnionType other) const {
  return other.types == types;
}

size_t UnionType::Hash::operator()(const UnionType& key) const {
  size_t hash = 0;
  for (TypeId arg : key.types)
    ComputeHash(hash, arg);
  return hash;
}

std::string TypeContext::GetNameFromTypeId(TypeId type_id) const {
  auto it = type_lookup_.find(type_id);
  if (it == type_lookup_.end()) {
    return "Unknown";
  }

  return std::visit(
      Overloaded{
          [&](const BuiltInType& type) {
            static const std::unordered_map<TypeId, std::string>
                kBuiltInTypeNames = {
                    {LiteralType::Void, "Void"}, {LiteralType::i32, "i32"},
                    {LiteralType::f32, "f32"},   {LiteralType::Bool, "bool"},
                    {LiteralType::Any, "any"}, {LiteralType::Nil, "Nil"},
                };
            return kBuiltInTypeNames.at(type_id);
          },
          [&](const FunctionType type) {
            std::stringstream ss;
            ss << "fn (";
            for (size_t i = 0; i < type.arg_types.size(); ++i) {
              if (i > 0)
                ss << ", ";
              ss << GetNameFromTypeId(type.arg_types[i]);
            }
            if (type.is_variadic) {
              if (!type.arg_types.empty())
                ss << ", ";
              ss << "...";
            }
            ss << ") -> " << GetNameFromTypeId(type.return_type);
            return ss.str();
          },
          [&](const StructType type) {
            return "struct " + type.struct_declaration->name;
          },
          [&](const UnionType type) {
            std::stringstream ss;
            ss << "(";
            for (size_t i = 0; i < type.types.size(); ++i) {
              if (i > 0)
                ss << ", ";
              ss << GetNameFromTypeId(type.types[i]);
            }
            ss << ")";
            return ss.str();
          }},
      it->second);
}