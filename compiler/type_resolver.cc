// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/type_resolver.h"

#include <algorithm>
#include <string>
#include <vector>

#include "compiler/error_collector.h"
#include "compiler/type_context.h"
#include "compiler/types.h"

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

TypeResolver::TypeResolver(TypeRegistry& type_registry,
                           TypeContext& type_context,
                           ErrorCollector& error_collector)
    : type_registry_(type_registry),
      type_context_(type_context),
      error_collector_(error_collector) {}

bool TypeResolver::Resolve(
    NamedBinding binding,
    const std::vector<std::optional<SpannedType>>& call_argument_types,
    std::vector<TypeId>& bindings,
    Metadata expression_metadata) {
  auto create_pattern_type =
      [this,
       &binding](const std::vector<TemplateArgument>& template_variables) {
        std::vector<TypeId> placeholder_type_ids(template_variables.size(), 0);
        for (size_t idx = 0; idx < template_variables.size(); ++idx) {
          placeholder_type_ids[idx] = type_registry_.NewPlaceholderType(idx);
        }
        return type_context_.GetTemplateOf(binding, placeholder_type_ids,
                                           TypeContext::CheckFunctionBody::NO);
      };

  Bindings deduced_bindings;
  if (const auto* symbol =
          type_registry_.GetSymbol<FunctionSymbol>(*binding.symbol_id)) {
    auto variables = symbol->declaration.template_arguments;

    if (auto pattern_type_id = create_pattern_type(variables)) {
      const auto* pattern_type =
          type_registry_.GetType<FunctionType>(*pattern_type_id);
      if (!pattern_type)
        return false;

      for (size_t i = 0; i < pattern_type->arg_types.size(); ++i) {
        if (i >= call_argument_types.size())
          break;

        if (!call_argument_types[i].has_value())
          continue;

        Resolve(pattern_type->arg_types[i], call_argument_types[i]->type_id,
                deduced_bindings);
      }

      if (pattern_type->variadic_type) {
        for (size_t i = pattern_type->arg_types.size();
             i < call_argument_types.size(); ++i) {
          if (!call_argument_types[i].has_value())
            continue;

          Resolve(*pattern_type->variadic_type, call_argument_types[i]->type_id,
                  deduced_bindings);
        }
      }

      // Ensure `bindings` is sized correctly and cleared
      bindings.assign(variables.size(), 0);

      bool bound_all_variables = true;
      for (size_t idx = 0; idx < variables.size(); ++idx) {
        if (deduced_bindings.contains(idx)) {
          bindings[idx] = deduced_bindings[idx];
          continue;
        }

        error_collector_
            .Add("unable to resolve template variable '" +
                     variables[idx].name.text + "'",
                 expression_metadata)
            .WithNote("declared here", variables[idx].name.metadata);
        bound_all_variables = false;
      }

      return bound_all_variables;
    }
  }

  else if (const auto* symbol =
               type_registry_.GetSymbol<StructSymbol>(*binding.symbol_id)) {
    const auto& variables = symbol->declaration.template_arguments;
    if (auto pattern_type_id = create_pattern_type(variables)) {
      const auto* pattern_type =
          type_registry_.GetType<StructType>(*pattern_type_id);

      for (size_t i = 0; i < pattern_type->field_types.size(); ++i) {
        if (i >= call_argument_types.size())
          break;

        if (!call_argument_types[i].has_value())
          continue;

        Resolve(pattern_type->field_types[i], call_argument_types[i]->type_id,
                deduced_bindings);
      }

      bool bound_all_variables = true;

      // Ensure `bindings` is sized correctly and cleared
      bindings.assign(variables.size(), 0);
      for (size_t idx = 0; idx < variables.size(); ++idx) {
        if (deduced_bindings.contains(idx)) {
          bindings[idx] = deduced_bindings[idx];
          continue;
        }

        error_collector_
            .Add("unable to resolve template variable '" +
                     variables[idx].name.text + "'",
                 expression_metadata)
            .WithNote("declared here", variables[idx].name.metadata);
        bound_all_variables = false;
      }

      return bound_all_variables;
    }
  }

  else {
    LOG(FATAL) << "non-template binding attempting to be instantiated: "
               << binding;
  }
  return false;
}

bool TypeResolver::Resolve(TypeId pattern_type_id,
                           TypeId concrete_type_id,
                           Bindings& bindings) {
  const Type& pattern_type = type_registry_.type_table().at(pattern_type_id);
  const Type& concrete_type = type_registry_.type_table().at(concrete_type_id);

  if (const auto* placeholder = std::get_if<PlaceholderType>(&pattern_type)) {
    if (bindings.contains(placeholder->idx)) {
      if (concrete_type_id == bindings.at(placeholder->idx))
        return true;

      LOG(ERROR) << "Binding already set for $" << placeholder->idx;
      return false;
    }

    bindings[placeholder->idx] = concrete_type_id;
    return true;
  }

  // Allows binding concrete-types to an optional pattern T? -- handles Nil.
  if (std::holds_alternative<OptionalType>(pattern_type) &&
      !std::holds_alternative<OptionalType>(concrete_type)) {
    return Resolve(std::get<OptionalType>(pattern_type).wrapped_type,
                   concrete_type_id, bindings);
  }

  if (pattern_type.index() != concrete_type.index())
    return false;

  return std::visit(
      Overloaded{
          [&](const AliasType& p, const AliasType& c) {
            return Resolve(p.target_type_id, c.target_type_id, bindings);
          },
          [&](const BuiltInType&, const BuiltInType&) {
            return pattern_type_id == concrete_type_id;
          },
          [&](const FunctionType& p, const FunctionType& c) {
            if (p.arg_types.size() != c.arg_types.size())
              return false;
            for (size_t i = 0; i < p.arg_types.size(); ++i) {
              if (!Resolve(p.arg_types[i], c.arg_types[i], bindings))
                return false;
            }
            return Resolve(p.return_type, c.return_type, bindings);
          },
          [&](const OptionalType& p, const OptionalType& c) {
            return Resolve(p.wrapped_type, c.wrapped_type, bindings);
          },
          [&](const PlaceholderType&, const PlaceholderType&) {
            NOTREACHED() << "concrete type MUST not have placeholders";
            return false;
          },
          [&](const StructType& p, const StructType& c) {
            // Ensures the same base class by comparing the stable AST pointers.
            if (&p.declaration != &c.declaration)
              return false;

            if (p.template_arguments.size() != c.template_arguments.size())
              return false;

            for (size_t i = 0; i < p.template_arguments.size(); ++i) {
              if (!Resolve(p.template_arguments[i], c.template_arguments[i],
                           bindings)) {
                return false;
              }
            }
            return true;
          },
          [&](const UnionType& p, const UnionType& c) {
            if (p.types.size() != c.types.size()) {
              return false;
            }
            // TODO: Ensure the ordering is consistent when interning
            for (size_t i = 0; i < p.types.size(); ++i) {
              if (!Resolve(p.types[i], c.types[i], bindings))
                return false;
            }
            return true;
          },
          [&](const auto&, const auto&) {
            NOTREACHED() << "Overload list MUST be exhaustive.";
            return false;
          },
      },
      pattern_type, concrete_type);
}
