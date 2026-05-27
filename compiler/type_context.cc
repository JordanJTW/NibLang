// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/type_context.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "compiler/error_collector.h"
#include "compiler/logging.h"
#include "compiler/tokenizer.h"
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

TypeContext::TypeContext(ScopeManager& scope_manager,
                         std::span<const std::string_view> external_functions)
    : scope_manager_(scope_manager),
      external_functions_(external_functions.begin(),
                          external_functions.end()) {
  // Add dummy entries for the built-in types to simplify logic.
  for (size_t i = 0; i < LiteralType::kCount; ++i)
    type_lookup_[i] = BuiltInType{};

  static FunctionDeclaration main_fn{
      .name = "<<main>>",
      .arguments = {},
      .return_type = ParsedType{"any"},
      .function_kind = FunctionKind::Free,
      // Technically all top-level statements are in <main> but TypeContext
      // only cares if a function _has_ a body or not for type checking.
      .body = std::make_unique<Block>(),
  };

  // Enter "global" function scope by default to allow for top-level statements.
  auto main_symbol = DefineFunction(main_fn, /*error_collector=*/nullptr);
  CHECK(main_symbol.has_value());
}

NamedBinding TypeContext::DeclareStructSymbol(std::string_view name,
                                              StructDeclaration& declaration) {
  SymbolId symbol_id = next_symbol_id_++;
  symbol_table_.emplace(symbol_id, StructSymbol{declaration});

  std::optional<TypeId> type_id = std::nullopt;
  // If the `struct` is already realized at declaration (concrete) then assign
  // its TypeId so that conrete struct/function declarations will fully resolve.
  if (!declaration.IsTemplate())
    type_id = next_type_id_++;

  return scope_manager_.InsertNameIntoScope(name, NamedBinding::Struct, type_id,
                                            symbol_id);
}

void TypeContext::DefineStructType(TypeId self_id,
                                   StructDeclaration& declaration,
                                   const std::vector<TypeId> template_arguemnts,
                                   ErrorCollector& error_collector) {
  StructType struct_type(declaration);
  struct_type.template_arguments = template_arguemnts;
  struct_type.scope_id = scope_manager_.EnterScope(
      ScopeManager::StructScope, "struct " + declaration.name);

  NamedBinding::Idx field_idx = 0;
  for (const auto& [name, type] : declaration.fields) {
    auto type_id = GetTypeIdFor(type);
    if (!type_id.has_value()) {
      error_collector.Add("Unknown type for struct field: " + name,
                          type.metadata);
      continue;
    }

    if (scope_manager_.FindBindingFor(name, ScopeManager::Current)) {
      error_collector.Add("Duplicate field name in struct: " + name,
                          type.metadata);
      continue;
    }

    scope_manager_.InsertNameIntoScope(name, NamedBinding::Field, type_id,
                                       /*symbol_id=*/std::nullopt, field_idx++);
    struct_type.field_types.push_back(type_id.value());
  }

  scope_manager_.ExitScope();

  for (auto& [name, fn] : declaration.methods) {
    // Errors are logged from within `DefineFunction`.
    if (std::optional<NamedBinding> binding =
            DefineFunction(fn, &error_collector, &declaration, self_id)) {
      scope_manager_.DeclareBindingInScope(name, *binding,
                                           struct_type.scope_id);
    }
  }

  type_lookup_.emplace(self_id, std::move(struct_type));
}

std::optional<NamedBinding> TypeContext::DefineFunction(
    FunctionDeclaration& fn,
    ErrorCollector* error_collector,
    std::optional<const StructDeclaration*> object,
    std::optional<TypeId> self_id) {
  std::string qualified_name = fn.name;
  bool is_function_extern = fn.function_kind == FunctionKind::Extern;
  if (object) {
    qualified_name = object.value()->name + "_" + fn.name;
    is_function_extern = object.value()->is_extern && !fn.body;
  }

  std::optional<NamedBinding::Idx> call_idx;
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

    if (error_collector && fn.is_variadic) {
      LOG(ERROR) << "'...' is only allowed in extern functions";
      return std::nullopt;
    }

    call_idx = GetCallIdxFor(qualified_name, CreateIfMissing::YES);
  }

  LOG(INFO) << "Assign CallIdx: " << *call_idx << " for " << fn.name;

  FunctionSymbol symbol{fn, self_id, scope_manager_.GetActiveScopeId()};
  SymbolId symbol_id = next_symbol_id_++;
  symbol_table_.emplace(symbol_id, std::move(symbol));

  fn.resolved = ResolvedFunction{};

  if (!fn.template_names.empty()) {
    function_lookup_[call_idx.value()] = &fn;
    NamedBinding binding = scope_manager_.InsertNameIntoScope(
        qualified_name, NamedBinding::Function,
        /*type_id=*/std::nullopt, symbol_id, call_idx);
    fn.resolved->function_symbol = binding;
    return binding;
  }

  if (std::optional<TypeId> type_id =
          DeclareFunctionType(fn, error_collector, self_id)) {
    NamedBinding binding = scope_manager_.InsertNameIntoScope(
        qualified_name, NamedBinding::Function, type_id.value(), symbol_id,
        call_idx.value());
    function_lookup_[call_idx.value()] = &fn;
    fn.resolved->function_symbol = binding;
    return binding;
  }

  LOG(INFO) << "Failed to declare funciton type";
  return std::nullopt;
}

std::optional<TypeId> TypeContext::GetTypeIdFor(const ParsedType& type) {
  return std::visit(
      Overloaded{
          [&](const std::string& type_name) -> std::optional<TypeId> {
            static const std::unordered_map<std::string, TypeId> kBuiltInTypes =
                {
                    {"Void", LiteralType::Void},
                    {"i32", LiteralType::i32},
                    {"Codepoint", LiteralType::Codepoint},
                    {"f32", LiteralType::f32},
                    {"bool", LiteralType::Bool},
                    {"any", LiteralType::Any},
                    {"Nil", LiteralType::Nil},
                };

            // Fast path for built-in type names which are used frequently.
            if (const auto& it = kBuiltInTypes.find(type_name);
                it != kBuiltInTypes.end()) {
              return it->second;
            }

            // Structure types are resolved nominally, while Functions would be
            // resolved structurally to allow for flexible callbacks, etc.
            if (auto symbol =
                    scope_manager_.FindBindingFor(type_name, ScopeManager::All);
                symbol.has_value() &&
                (symbol->kind == NamedBinding::Struct ||
                 symbol->kind == NamedBinding::Template)) {
              return symbol->realized_type_id;
            }
            return std::nullopt;
          },
          [&](const ParsedUnionType& type) -> std::optional<TypeId> {
            std::vector<TypeId> type_ids;
            for (const auto& name : type.names) {
              std::optional<TypeId> type_id = GetTypeIdFor(name);
              if (!type_id.has_value())
                return std::nullopt;

              type_ids.push_back(type_id.value());
            }
            CHECK_GT(type_ids.size(), 1)
                << "Parser returned a weird ParsedUnionType";

            return GetUnionOf(type_ids);
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
          },
          [&](const ParsedOptionalType& optional_type)
              -> std::optional<TypeId> {
            CHECK(optional_type.wrapped_type)
                << "Parser should never return an OptionalType without a "
                   "wrapped type";

            auto wrapped_type_id = GetTypeIdFor(*optional_type.wrapped_type);
            if (!wrapped_type_id.has_value())
              return std::nullopt;

            auto key = wrapped_type_id.value();
            if (const auto& it = interned_optional_type_.find(key);
                it != interned_optional_type_.end()) {
              return it->second;
            }

            TypeId type_id = next_type_id_++;
            interned_optional_type_[key] = type_id;
            type_lookup_[type_id] = OptionalType{wrapped_type_id.value()};
            return type_id;
          },
          [&](const ParsedParameterizedType& parameterized_type)
              -> std::optional<TypeId> {
            std::string* base_type_identifier =
                std::get_if<std::string>(&parameterized_type.type->type);

            CHECK(base_type_identifier)
                << "ParameterizedType::type is not an identifier";

            auto binding = scope_manager_.FindBindingFor(*base_type_identifier,
                                                         ScopeManager::All);

            if (!binding || !binding->symbol_id)
              return std::nullopt;

            std::vector<TypeId> argument_type_ids;
            for (const auto& parsed_type : parameterized_type.parameters) {
              if (auto type_id = GetTypeIdFor(parsed_type)) {
                argument_type_ids.push_back(type_id.value());
              } else {
                return std::nullopt;
              }
            }

            ErrorCollector dummy_error_collector;
            return GetTemplateOf(*binding->symbol_id,
                                 std::move(argument_type_ids),
                                 dummy_error_collector);
          }},
      type.type);
}

TypeId TypeContext::WrapTypeIdAsOptional(TypeId type_id) {
  auto key = type_id;
  if (const auto& it = interned_optional_type_.find(key);
      it != interned_optional_type_.end()) {
    return it->second;
  }

  TypeId optional_type_id = next_type_id_++;
  interned_optional_type_[key] = optional_type_id;
  type_lookup_[optional_type_id] = OptionalType{type_id};
  return optional_type_id;
}

std::optional<TypeId> TypeContext::UnwrapOptionalTypeId(TypeId type_id) const {
  if (std::holds_alternative<OptionalType>(type_lookup_.at(type_id))) {
    return std::get<OptionalType>(type_lookup_.at(type_id)).wrapped_type;
  }
  return std::nullopt;
}

TypeId TypeContext::GetUnionOf(const std::vector<TypeId>& types) {
  // Look-up member types and normalize (sort/dedupe/flatten).
  std::set<TypeId> normalized_types;  // std::set sorts and dedupes
  for (const auto& type_id : types) {
    // If the member itself is a union, flatten it.
    if (auto* u = GetTypeInfo<UnionType>(type_id)) {
      normalized_types.insert(u->types.begin(), u->types.end());
    } else {
      normalized_types.insert(type_id);
    }
  }

  // If after normalization the union is just a single type, then return it.
  if (normalized_types.size() == 1) {
    return *normalized_types.begin();
  }

  // Intern unions structurally to a TypeId for faster comparisons.
  // UnionType stores the types as a std::vector instead of a std::set
  // to take advatange of better cache-locality (due to contiguous memory).
  auto key = UnionType{{normalized_types.begin(), normalized_types.end()}};
  if (const auto& it = interned_union_type_.find(key);
      it != interned_union_type_.end()) {
    return it->second;
  }

  TypeId type_id = next_type_id_++;
  interned_union_type_[key] = type_id;
  type_lookup_[type_id] = key;
  return type_id;
}

bool TypeContext::IsTypeNilable(TypeId type_id) const {
  return type_id == LiteralType::Nil || UnwrapOptionalTypeId(type_id);
}

std::optional<TypeId> TypeContext::DeclareFunctionType(
    FunctionDeclaration& fn,
    ErrorCollector* error_collector,
    std::optional<TypeId> self_id) {
  ScopeId scope_id =
      scope_manager_.EnterScope(ScopeManager::FunctionScope, "fn " + fn.name);

  std::vector<TypeId> argument_types;
  std::vector<NamedBinding> argument_bindings;
  for (const auto& [name, type] : fn.arguments) {
    if (auto type_id = GetTypeIdFor(type); type_id.has_value()) {
      argument_bindings.push_back(
          scope_manager_.DeclareVariableBinding(name, *type_id));
      argument_types.push_back(*type_id);
    } else {
      // TODO: Print error message on bad argument type.
      LOG(ERROR) << "Bad argument in: " << fn.name;
      return std::nullopt;
    }
  }
  fn.resolved->arguments = std::move(argument_bindings);

  if (fn.function_kind == FunctionKind::Method && error_collector &&
      (argument_types.size() == 0 || argument_types[0] != self_id)) {
    error_collector->Add("Methods must begin with a `self` argument",
                         fn.argument_range);
  }

  std::optional<TypeId> return_type = GetTypeIdFor(fn.return_type);
  // Missing return types are already handled in the Parser (resolving to
  // Void) so if `return_type` has no value here it is truly an unknown type.
  if (!return_type.has_value()) {
    LOG(ERROR) << "No return type " << fn.return_type;
    // TODO: Print error message on bad return type.
    return std::nullopt;
  }

  scope_manager_.ExitScope();

  realized_functions_.push_back(RealizedFunction{scope_id, fn});

  auto key = FunctionType{std::move(argument_types), return_type.value(),
                          fn.is_variadic, scope_id};
  if (const auto& it = interned_fn_type_.find(key);
      it != interned_fn_type_.end()) {
    return it->second;
  }

  TypeId type_id = next_type_id_++;
  interned_fn_type_[key] = type_id;
  type_lookup_[type_id] = key;
  return type_id;
}

bool TypeContext::IsTypeSubsetOf(TypeId sub_type_id, TypeId super_type_id) {
  if (sub_type_id == super_type_id || super_type_id == LiteralType::Any)
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

  if (sub_type_id == LiteralType::Nil &&
      std::holds_alternative<OptionalType>(super_type)) {
    return true;  // Nil is a subset of any Optional type.
  }

  if (std::holds_alternative<OptionalType>(sub_type) &&
      std::holds_alternative<OptionalType>(super_type)) {
    const auto& sub_wrapped = std::get<OptionalType>(sub_type).wrapped_type;
    const auto& super_wrapped = std::get<OptionalType>(super_type).wrapped_type;
    return IsTypeSubsetOf(sub_wrapped, super_wrapped);
  }

  if (!std::holds_alternative<OptionalType>(sub_type) &&
      std::holds_alternative<OptionalType>(super_type)) {
    const auto& super_wrapped = std::get<OptionalType>(super_type).wrapped_type;
    return IsTypeSubsetOf(sub_type_id, super_wrapped);
  }

  // Neither type is a union so they must be different concrete types.
  return false;
}

std::optional<NamedBinding::Idx> TypeContext::GetCallIdxFor(
    const std::string& name,
    CreateIfMissing create) {
  static const std::map<std::string, NamedBinding::Idx> kBuiltInCallIdx = {
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
      {"String_valueOf", VM_BUILTIN_STRINGS_VALUEOF},
  };

  // Since some built-in functions (i.e. Array_withSize) overload a single
  // native function, this count can differ from `kBuiltInCallIdx.size()`.
  static constexpr size_t kBuiltInCallCount = 19;

  if (auto it = kBuiltInCallIdx.find(name); it != kBuiltInCallIdx.end())
    return it->second;

  if (auto it = std::find(external_functions_.begin(),
                          external_functions_.end(), name);
      it != external_functions_.end()) {
    // Functions provided to the VM appear directly after built-ins within the
    // function look-up table (calculate the index and convert to VM_BUILTIN).
    return VM_BUILTIN(std::distance(external_functions_.begin(), it) +
                      kBuiltInCallCount);
  }

  if (auto it = assigned_call_idx_.find(name);
      it != assigned_call_idx_.end() && !name.empty()) {
    return it->second;
  }

  if (create == CreateIfMissing::YES) {
    CallIdx call_idx = next_call_idx_++;
    assigned_call_idx_[name] = call_idx;
    return call_idx;
  }

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
      Overloaded{[&](const BuiltInType& type) {
                   static const std::unordered_map<TypeId, std::string>
                       kBuiltInTypeNames = {
                           {LiteralType::Void, "Void"},
                           {LiteralType::i32, "i32"},
                           {LiteralType::f32, "f32"},
                           {LiteralType::Codepoint, "Codepoint"},
                           {LiteralType::Bool, "bool"},
                           {LiteralType::Any, "any"},
                           {LiteralType::Nil, "Nil"},
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
                   return "struct " + type.declaration.name;
                 },
                 [&](const UnionType type) {
                   std::stringstream ss;
                   ss << "Union[";
                   for (size_t i = 0; i < type.types.size(); ++i) {
                     if (i > 0)
                       ss << ", ";
                     ss << GetNameFromTypeId(type.types[i]);
                   }
                   ss << "]";
                   return ss.str();
                 },
                 [&](const OptionalType type) {
                   return GetNameFromTypeId(type.wrapped_type) + "?";
                 }},
      it->second);
}

std::optional<TypeId> TypeContext::GetTemplateOf(
    SymbolId symbol_id,
    const std::vector<TypeId>& argument_type_ids,
    ErrorCollector& error_collector) {
  auto it = symbol_table_.find(symbol_id);
  CHECK(it != symbol_table_.end()) << "Unable to lookup symbol: " << symbol_id;

  std::stringstream ss;
  for (size_t i = 0; i < argument_type_ids.size(); ++i) {
    if (i > 0)
      ss << ", ";

    ss << GetNameFromTypeId(argument_type_ids[i]);
  }

  if (StructSymbol* symbol = std::get_if<StructSymbol>(&it->second)) {
    if (auto it = symbol->instances.find(argument_type_ids);
        it != symbol->instances.end()) {
      return it->second;
    }

    TypeId self_id = next_type_id_++;
    symbol->instances[argument_type_ids] = self_id;

    LOG(INFO) << "New Struct.TemplateOf(" << symbol->declaration.name << ") + ["
              << ss.str() << "] => " << self_id;

    const auto& template_names = symbol->declaration.template_names;

    if (argument_type_ids.size() != template_names.size()) {
      error_collector.Add(
          "Template struct " + symbol->declaration.name + " requires " +
              std::to_string(template_names.size()) +
              " template arguments but only " +
              std::to_string(argument_type_ids.size()) + " were provided",
          {});
      return std::nullopt;
    }

    // TODO: Ensure scope stacking does not cause bugs since unrelated templates
    //       would inherrit this scope environment if a type is processed later
    scope_manager_.EnterScope(ScopeManager::TemplateScope,
                              "struct " + symbol->declaration.name);
    std::unordered_map<std::string, TypeId> bindings;
    for (size_t i = 0; i < template_names.size(); ++i) {
      scope_manager_.InsertNameIntoScope(
          template_names[i], NamedBinding::Kind::Template, argument_type_ids[i],
          /*symbol=*/std::nullopt);
      bindings[template_names[i]] = argument_type_ids[i];
    }

    DefineStructType(self_id, symbol->declaration, argument_type_ids,
                     error_collector);

    scope_manager_.ExitScope();

    return self_id;
  }

  if (FunctionSymbol* symbol = std::get_if<FunctionSymbol>(&it->second)) {
    if (auto it = symbol->instances.find(argument_type_ids);
        it != symbol->instances.end()) {
      return it->second;
    }

    const auto& template_names = symbol->declaration.template_names;

    if (argument_type_ids.size() != template_names.size()) {
      error_collector.Add(
          "Template fn " + symbol->declaration.name + " requires " +
              std::to_string(template_names.size()) +
              " template arguments but only " +
              std::to_string(argument_type_ids.size()) + " were provided",
          {});
      return std::nullopt;
    }

    ScopeId original_scope_id = scope_manager_.GetActiveScopeId();
    scope_manager_.SetActiveScopeId(symbol->scope_id);

    // TODO: Ensure scope stacking does not cause bugs since unrelated templates
    //       would inherrit this scope environment if a type is processed later
    scope_manager_.EnterScope(ScopeManager::TemplateScope,
                              "fn " + symbol->declaration.name);
    for (size_t i = 0; i < template_names.size(); ++i) {
      scope_manager_.InsertNameIntoScope(
          template_names[i], NamedBinding::Kind::Template, argument_type_ids[i],
          /*symbol=*/std::nullopt);
    }

    // TODO: Handle `self_id` better. Define it early to prevent cycles.
    std::optional<TypeId> self_id = DeclareFunctionType(
        symbol->declaration, &error_collector, symbol->parent_type_id);

    if (self_id) {
      symbol->instances[argument_type_ids] = *self_id;

      LOG(INFO) << "New Function.TemplateOf(" << symbol->declaration.name
                << ") + [" << ss.str() << "] => " << *self_id;
    }

    scope_manager_.ExitScope();
    scope_manager_.SetActiveScopeId(original_scope_id);

    return self_id;
  }

  NOTREACHED() << "Do not know how to realize SymbolId: " << symbol_id;
  return std::nullopt;
}

ParsedType TypeContext::GetParsedTypeFromId(TypeId type_id) const {
  const auto it = type_lookup_.find(type_id);
  CHECK(it != type_lookup_.end()) << "Invalid TypeId: " << type_id;

  return std::visit(
      Overloaded{[&](const BuiltInType& type) {
                   return ParsedType{GetNameFromTypeId(type_id)};
                 },
                 [&](const FunctionType& type) {
                   std::vector<ParsedType> arguments;
                   for (TypeId type_id : type.arg_types)
                     arguments.push_back(GetParsedTypeFromId(type_id));

                   auto return_type = GetParsedTypeFromId(type.return_type);

                   return ParsedType{ParsedFunctionType{
                       std::move(arguments),
                       std::make_shared<ParsedType>(std::move(return_type))}};
                 },
                 [&](const StructType& type) {
                   if (type.declaration.IsTemplate()) {
                     auto base_type = ParsedType{type.declaration.name};

                     std::vector<ParsedType> parameter_types;
                     for (TypeId type_id : type.template_arguments)
                       parameter_types.push_back(GetParsedTypeFromId(type_id));

                     return ParsedType{ParsedParameterizedType{
                         std::make_shared<ParsedType>(std::move(base_type)),
                         std::move(parameter_types)}};
                   }

                   return ParsedType{type.declaration.name};
                 },
                 [&](const UnionType& type) {
                   std::vector<ParsedType> union_types;
                   for (TypeId type_id : type.types)
                     union_types.push_back(GetParsedTypeFromId(type_id));

                   return ParsedType{ParsedUnionType{std::move(union_types)}};
                 },
                 [&](const OptionalType& type) {
                   auto wrapped_type = GetParsedTypeFromId(type.wrapped_type);

                   return ParsedType{ParsedOptionalType{
                       std::make_shared<ParsedType>(std::move(wrapped_type))}};
                 }},
      it->second);
}