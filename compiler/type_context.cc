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

using LiteralType = TypeRegistry::LiteralType;

}  // namespace

TypeContext::TypeContext(ScopeManager& scope_manager,
                         TypeRegistry& type_registry,
                         ErrorCollector& error_collector)
    : scope_manager_(scope_manager),
      type_registry_(type_registry),
      error_collector_(error_collector) {}

void TypeContext::DefineStructType(
    TypeId self_id,
    StructSymbol& symbol,
    const std::vector<TypeId> template_arguemnts) {
  StructType struct_type(symbol.declaration);
  struct_type.template_arguments = template_arguemnts;
  struct_type.scope_id = scope_manager_.EnterScope(
      ScopeManager::StructScope, "struct " + symbol.declaration.name.text);

  // Cache the instance early in case a member/method is refers to self.
  symbol.instances[template_arguemnts] =
      TypeInstance{self_id, struct_type.scope_id};

  NamedBinding::Idx field_idx = 0;
  for (const auto& [name, type] : symbol.declaration.fields) {
    auto type_id = GetTypeIdFor(type);
    if (!type_id.has_value()) {
      error_collector_.Add("Unknown type for struct field: " + name.text,
                           type.metadata);
      continue;
    }

    if (scope_manager_.FindBindingFor(name.text, ScopeManager::Current)) {
      error_collector_.Add("Duplicate field name in struct: " + name.text,
                           name.metadata);
      continue;
    }

    scope_manager_.InsertNameIntoScope(name, NamedBinding::Field, type_id,
                                       /*symbol_id=*/std::nullopt, field_idx++);
    struct_type.field_types.push_back(type_id.value());
  }

  for (const auto& binding :
       scope_manager_.GetBindingsForScope(symbol.self_scope_id)) {
    if (binding.kind != NamedBinding::Function)
      continue;

    SymbolId symbol_id = binding.symbol_id.value();

    // Errors are logged from within `DefineFunction`.
    DefineFunction(symbol_id, self_id);
  }
  scope_manager_.ExitScope();

  type_registry_.NewStructType(std::move(struct_type), self_id);
}

std::optional<NamedBinding> TypeContext::DefineFunction(
    SymbolId symbol_id,
    std::optional<TypeId> self_id) {
  FunctionSymbol* symbol = type_registry_.GetSymbol<FunctionSymbol>(symbol_id);
  CHECK(symbol) << "DefineFunction passed an invalid `symbol_id`";

  FunctionDeclaration& fn = symbol->declaration;

  if (!symbol->IsExtern()) {
    if (!fn.body) {
      error_collector_.Add(
          "non-extern functions MUST have a body: " + fn.name.text,
          fn.name.metadata);
      return std::nullopt;
    }

    if (fn.variadic_type.has_value()) {
      error_collector_.Add("'...' is only allowed in extern functions",
                           fn.variadic_type->variadic_span);
      return std::nullopt;
    }
  }

  fn.resolved = ResolvedFunction{};

  std::optional<TypeInstance> instance;
  if (fn.template_arguments.empty()) {
    if ((instance = DeclareFunctionType(fn, self_id))) {
      std::vector<TypeId> instance_key =
          self_id ? std::vector<TypeId>{*self_id} : std::vector<TypeId>{};
      symbol->instances[std::move(instance_key)] = *instance;
    } else {
      // Errors logged in DeclareFunctionType()
      return std::nullopt;
    }
  } else {
    for (const auto& argument : fn.template_arguments) {
      if (!argument.default_type.has_value())
        continue;

      // Default template names must be resolved in their declaration context.
      auto type_id = GetTypeIdFor(*argument.default_type);
      if (!type_id.has_value()) {
        error_collector_.Add("unknown type provided as template argument",
                             argument.default_type->metadata);
        continue;
      }

      symbol->default_template_type_ids[argument.name.text] = *type_id;
    }
  }

  auto type_id = instance ? instance->type_id : std::optional<TypeId>{};
  NamedBinding binding = scope_manager_.InsertNameIntoScope(
      fn.name, NamedBinding::Function, std::move(type_id), symbol_id,
      /*idx=*/std::nullopt, std::move(self_id));
  fn.resolved->function_symbol = binding;
  return binding;
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
                    {"never", LiteralType::Never},
                    // Nil is used with Optional types (T?), NEVER standalone
                    // {"Nil", LiteralType::Nil},
                };

            // Fast path for built-in type names which are used frequently.
            if (const auto& it = kBuiltInTypes.find(type_name);
                it != kBuiltInTypes.end()) {
              return it->second;
            }

            // Structure types are resolved nominally, while Functions would be
            // resolved structurally to allow for flexible callbacks, etc.
            if (auto binding = scope_manager_.FindBindingFor(
                    type_name, ScopeManager::All)) {
              if (binding->IsType()) {
                // Add an error when trying to get a TypeId of a base template.
                if (!binding->realized_type_id.has_value()) {
                  error_collector_.Add("invalid use of template-name '" +
                                           type_name +
                                           "' without an argument list",
                                       type.metadata);
                }

                return binding->realized_type_id;
              } else {
                // This can only happen in edge-cases like a variable appearing
                // in the scope from an import or when instantiating a template.
                error_collector_.Add("variable can not be used as a type",
                                     type.metadata);
                return std::nullopt;
              }
            }

            error_collector_.Add("unknown type '" + type_name + "'",
                                 type.metadata);
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
            return type_registry_.NewFunctionType(std::move(key));
          },
          [&](const ParsedOptionalType& optional_type)
              -> std::optional<TypeId> {
            CHECK(optional_type.wrapped_type)
                << "Parser should never return an OptionalType without a "
                   "wrapped type";

            auto wrapped_type_id = GetTypeIdFor(*optional_type.wrapped_type);
            if (!wrapped_type_id.has_value())
              return std::nullopt;

            return type_registry_.NewOptionalType(*wrapped_type_id);
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

            return GetTemplateOf(binding.value(), std::move(argument_type_ids));
          }},
      type.type);
}

TypeId TypeContext::GetOptionalOf(TypeId type_id) {
  return type_registry_.NewOptionalType(type_id);
}

std::optional<TypeId> TypeContext::UnwrapOptional(TypeId type_id) const {
  if (const auto* optional = type_registry_.GetType<OptionalType>(type_id))
    return optional->wrapped_type;

  return std::nullopt;
}

TypeId TypeContext::GetUnionOf(const std::vector<TypeId>& types) {
  // Look-up member types and normalize (sort/dedupe/flatten).
  std::set<TypeId> normalized_types;  // std::set sorts and dedupes
  for (const auto& type_id : types) {
    // If the member itself is a union, flatten it.
    if (auto* u = type_registry_.GetType<UnionType>(type_id)) {
      normalized_types.insert(u->types.begin(), u->types.end());
    } else {
      normalized_types.insert(type_id);
    }
  }

  // Never represents an impossible value, so T | Never simplifies to T.
  normalized_types.erase(LiteralType::Never);
  // If all members collapsed away, the result is Never.
  if (normalized_types.empty())
    return LiteralType::Never;

  // If after normalization the union is just a single type, then return it.
  if (normalized_types.size() == 1) {
    return *normalized_types.begin();
  }

  // Intern unions structurally to a TypeId for faster comparisons.
  // UnionType stores the types as a std::vector instead of a std::set
  // to take advatange of better cache-locality (due to contiguous memory).
  auto key = UnionType{{normalized_types.begin(), normalized_types.end()}};
  return type_registry_.NewUnionType(std::move(key));
}

TypeId TypeContext::GetAliasOf(SpannedText name, const ParsedType& type) {
  // All alias are nominally typed by definition so assign a new TypeId.
  TypeId type_id = type_registry_.NewTypeId();
  // The TypeId is assigned and added to the scope to allow for recursive type
  // aliases but the `target_type_id` is unknown until it has been resolved.
  scope_manager_.InsertNameIntoScope(name, NamedBinding::TypeAlias, type_id,
                                     /*symbol_id=*/std::nullopt);

  std::optional<TypeId> target_type_id = GetTypeIdFor(type);
  if (target_type_id) {
    type_registry_.NewAliasType(name.text, type_id, *target_type_id);
  }
  return type_id;
}

bool TypeContext::IsTypeNilable(TypeId type_id) const {
  return type_id == LiteralType::Nil || UnwrapOptional(type_id);
}

std::optional<TypeInstance> TypeContext::DeclareFunctionType(
    FunctionDeclaration& fn,
    std::optional<TypeId> self_id) {
  ScopeId scope_id = scope_manager_.EnterScope(ScopeManager::FunctionScope,
                                               "fn " + fn.name.text);

  std::vector<TypeId> argument_types;
  for (const auto& [name, type] : fn.arguments) {
    if (auto type_id = GetTypeIdFor(type); type_id.has_value()) {
      scope_manager_.DeclareArgumentBinding(name, *type_id);
      argument_types.push_back(*type_id);
    } else {
      return std::nullopt;
    }
  }

  if (fn.function_kind == FunctionKind::Method &&
      (argument_types.size() == 0 || argument_types[0] != self_id)) {
    error_collector_.Add("Methods must begin with a `self` argument", {});
  }

  std::optional<TypeId> return_type = GetTypeIdFor(fn.return_type);
  // Missing return types are already handled in the Parser (resolving to
  // Void) so if `return_type` has no value here it is truly an unknown type.
  if (!return_type.has_value())
    return std::nullopt;

  std::optional<TypeId> variadic_type;
  if (fn.variadic_type.has_value()) {
    variadic_type = GetTypeIdFor(fn.variadic_type->type);

    if (!variadic_type.has_value())
      return std::nullopt;
  }

  scope_manager_.ExitScope();

  realized_functions_.push_back(RealizedFunction{scope_id, fn, *return_type});

  auto key = FunctionType{std::move(argument_types), return_type.value(),
                          std::move(variadic_type)};
  TypeId type_id = type_registry_.NewFunctionType(std::move(key));
  return TypeInstance{type_id, scope_id};
}

bool TypeContext::IsTypeSubsetOf(TypeId sub_type_id,
                                 TypeId super_type_id) const {
  // Fast-path without unwrapping aliases just compare the IDs.
  if (sub_type_id == super_type_id)
    return true;

  auto follow_alias = [this](TypeId type_id) {
    while (const auto* alias = type_registry_.GetType<AliasType>(type_id))
      type_id = alias->target_type_id;

    return type_id;
  };

  sub_type_id = follow_alias(sub_type_id);
  super_type_id = follow_alias(super_type_id);

  // Re-compare the type IDs after unwrapping aliases.
  if (sub_type_id == super_type_id)
    return true;

  if (sub_type_id == LiteralType::Never || super_type_id == LiteralType::Any)
    return true;

  const Type& sub_type = type_registry_.type_table().at(sub_type_id);
  const Type& super_type = type_registry_.type_table().at(super_type_id);

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

std::optional<TypeId> TypeContext::GetTemplateOf(
    NamedBinding binding,
    const std::vector<TypeId>& argument_type_ids) {
  CHECK(binding.symbol_id) << "Provided binding is missing SymbolId";

  std::stringstream ss;
  for (size_t i = 0; i < argument_type_ids.size(); ++i) {
    if (i > 0)
      ss << ", ";

    ss << type_registry_.GetNameFromTypeId(argument_type_ids[i]);
  }

  if (StructSymbol* symbol =
          type_registry_.GetSymbol<StructSymbol>(*binding.symbol_id)) {
    if (auto it = symbol->instances.find(argument_type_ids);
        it != symbol->instances.end()) {
      return it->second.type_id;
    }

    TypeId self_id = type_registry_.NewTypeId();
    LOG(INFO) << "New Struct.TemplateOf(" << symbol->declaration.name.text
              << ") + [" << ss.str() << "] => " << self_id;

    const auto& template_arguments = symbol->declaration.template_arguments;

    if (argument_type_ids.size() < template_arguments.size()) {
      error_collector_.Add(
          "Template struct " + symbol->declaration.name.text + " requires " +
              std::to_string(template_arguments.size()) +
              " template arguments but only " +
              std::to_string(argument_type_ids.size()) + " were provided",
          {});
      return std::nullopt;
    }

    return scope_manager_.WithScope(
        symbol->self_scope_id, [&]() -> std::optional<TypeId> {
          return scope_manager_.NewScope(
              ScopeManager::TemplateScope,
              "struct " + symbol->declaration.name.text, [&]() {
                for (size_t i = 0; i < template_arguments.size(); ++i)
                  scope_manager_.DeclareTemplateBinding(
                      template_arguments[i].name, argument_type_ids[i]);

                DefineStructType(self_id, *symbol, argument_type_ids);
                return self_id;
              });
        });
  }

  if (FunctionSymbol* symbol =
          type_registry_.GetSymbol<FunctionSymbol>(*binding.symbol_id)) {
    if (auto it = symbol->instances.find(argument_type_ids);
        it != symbol->instances.end()) {
      return it->second.type_id;
    }

    const auto& template_arguments = symbol->declaration.template_arguments;

    if (argument_type_ids.size() < template_arguments.size()) {
      error_collector_.Add(
          "Template fn " + symbol->declaration.name.text + " requires " +
              std::to_string(template_arguments.size()) +
              " template arguments but only " +
              std::to_string(argument_type_ids.size()) + " were provided",
          {});
      return std::nullopt;
    }

    std::optional<ScopeId> parent_scope_id;
    if (binding.parent_type_id) {
      const auto* const struct_type =
          type_registry_.GetType<StructType>(*binding.parent_type_id);
      CHECK(struct_type) << "`parent_type_id` did not map to StructType";
      parent_scope_id = struct_type->scope_id;
    }

    const ScopeId lexical_scope_id =
        parent_scope_id.value_or(symbol->environment_scope_id);
    return scope_manager_.WithScope(
        lexical_scope_id, [&]() -> std::optional<TypeId> {
          // TODO: Handle `self_id` better. Define it early to prevent cycles.
          auto instance = scope_manager_.NewScope(
              ScopeManager::TemplateScope,
              "fn " + symbol->declaration.name.text, [&]() {
                for (size_t i = 0; i < template_arguments.size(); ++i)
                  scope_manager_.DeclareTemplateBinding(
                      template_arguments[i].name, argument_type_ids[i]);

                return DeclareFunctionType(symbol->declaration,
                                           binding.parent_type_id);
              });

          if (instance) {
            symbol->instances[argument_type_ids] = *instance;

            LOG(INFO) << "New Function.TemplateOf("
                      << symbol->declaration.name.text << ") + [" << ss.str()
                      << "] => " << instance->type_id;
          }
          return instance->type_id;
        });
  }

  NOTREACHED() << "Do not know how to realize binding: " << binding;
  return std::nullopt;
}

ParsedType TypeContext::GetParsedTypeFromId(TypeId type_id) const {
  const auto& type_table = type_registry_.type_table();
  const auto it = type_table.find(type_id);
  CHECK(it != type_table.end()) << "Invalid TypeId: " << type_id;

  return std::visit(
      Overloaded{[&](const BuiltInType& type) {
                   return ParsedType{type_registry_.GetNameFromTypeId(type_id)};
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
                     auto base_type = ParsedType{type.declaration.name.text};

                     std::vector<ParsedType> parameter_types;
                     for (TypeId type_id : type.template_arguments)
                       parameter_types.push_back(GetParsedTypeFromId(type_id));

                     return ParsedType{ParsedParameterizedType{
                         std::make_shared<ParsedType>(std::move(base_type)),
                         std::move(parameter_types)}};
                   }

                   return ParsedType{type.declaration.name.text};
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
                 },
                 [&](const AliasType& type) {
                   // AliasType's do not map 1:1 back to a ParsedType but it is
                   // safe to simply unwrap them and return the target TypeId.
                   return GetParsedTypeFromId(type.target_type_id);
                 }},
      it->second);
}