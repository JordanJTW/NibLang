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
#include "type_rewriter.h"

namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

}  // namespace

TypeResolver::TypeResolver(TypeRegistry& type_registry,
                           TypeContext& type_context,
                           ErrorCollector& error_collector)
    : type_registry_(type_registry),
      type_context_(type_context),
      error_collector_(error_collector) {}

TypeResolver::~TypeResolver() {
  for (const auto& [slot_id, inferred_type] : bindings_) {
    const auto& template_variable =
        type_registry_.GetTypeChecked<TemplateVariableType>(
            template_variables_.at(slot_id));

    if (!template_variable.constraint_type_id)
      continue;

    if (!type_context_.IsTypeSubsetOf(
            inferred_type.type_id,
            template_variable.constraint_type_id->type_id)) {
      std::string inferred_type_name =
          type_registry_.GetNameFromTypeId(inferred_type.type_id);
      std::string constraint_type_name = type_registry_.GetNameFromTypeId(
          template_variable.constraint_type_id->type_id);
      error_collector_
          .Add(inferred_type_name + " does not satisfy interface constraint " +
                   constraint_type_name,
               inferred_type.metadata)
          .WithNote("constraint declared here",
                    template_variable.constraint_type_id->metadata);
    }
  }
}

bool TypeResolver::Resolve(TypeId pattern_type_id,
                           TypeId concrete_type_id,
                           Metadata resolution_span) {
  pattern_type_id = Prune(pattern_type_id);
  concrete_type_id = Prune(concrete_type_id);

  if (pattern_type_id == concrete_type_id)
    return true;

  const TypeVariant& pattern_type =
      type_registry_.type_table().at(pattern_type_id).variant;
  const TypeVariant& concrete_type =
      type_registry_.type_table().at(concrete_type_id).variant;

  if (const auto* placeholder = std::get_if<PlaceholderType>(&pattern_type)) {
    CHECK(placeholder->idx < template_variables_.size());
    bindings_[placeholder->idx] =
        SpannedType{concrete_type_id, resolution_span};
    return true;
  }

  if (const auto* placeholder = std::get_if<PlaceholderType>(&concrete_type)) {
    CHECK(placeholder->idx < template_variables_.size());
    bindings_[placeholder->idx] = SpannedType{pattern_type_id, resolution_span};
    return true;
  }

  if (std::holds_alternative<OptionalType>(pattern_type) &&
      concrete_type_id == TypeRegistry::Nil) {
    return true;
  }

  // Allows binding concrete-types to an optional pattern T? -- handles Nil.
  if (std::holds_alternative<OptionalType>(pattern_type) &&
      !std::holds_alternative<OptionalType>(concrete_type)) {
    return Resolve(std::get<OptionalType>(pattern_type).wrapped_type,
                   concrete_type_id, resolution_span);
  }

  if (pattern_type.index() != concrete_type.index())
    return false;

  return std::visit(
      Overloaded{
          [&](const AliasType& p, const AliasType& c) {
            return Resolve(p.target_type_id, c.target_type_id, resolution_span);
          },
          [&](const BuiltInType&, const BuiltInType&) {
            return pattern_type_id == concrete_type_id;
          },
          [&](const FunctionType& p, const FunctionType& c) {
            if (p.arg_types.size() != c.arg_types.size())
              return false;
            for (size_t i = 0; i < p.arg_types.size(); ++i) {
              if (!Resolve(p.arg_types[i], c.arg_types[i], resolution_span))
                return false;
            }
            return Resolve(p.return_type, c.return_type, resolution_span);
          },
          [&](const OptionalType& p, const OptionalType& c) {
            return Resolve(p.wrapped_type, c.wrapped_type, resolution_span);
          },
          [&](const TemplateVariableType&, const TemplateVariableType&) {
            return pattern_type_id == concrete_type_id;
          },
          [&](const StructType& p, const StructType& c) {
            // Ensures the same base class by comparing the stable AST pointers.
            if (p.symbol != c.symbol)
              return false;

            if (p.instance_template_type_ids.size() !=
                c.instance_template_type_ids.size())
              return false;

            for (size_t i = 0; i < p.instance_template_type_ids.size(); ++i) {
              if (!Resolve(p.instance_template_type_ids[i],
                           c.instance_template_type_ids[i], resolution_span)) {
                return false;
              }
            }
            return true;
          },
          [&](const UnionType& p, const UnionType& c) {
            if (p.types.size() != c.types.size())
              return false;

            for (size_t i = 0; i < p.types.size(); ++i) {
              if (!Resolve(p.types[i], c.types[i], resolution_span))
                return false;
            }
            return true;
          },
          [&](const auto&, const auto&) { return false; },
      },
      pattern_type, concrete_type);
}

TypeId TypeResolver::Prune(TypeId type_id) {
  if (const auto* placeholder =
          type_registry_.GetType<PlaceholderType>(type_id)) {
    if (auto it = bindings_.find(placeholder->idx); it != bindings_.end()) {
      TypeId root = Prune(it->second.type_id);
      bindings_[placeholder->idx] = SpannedType{root, it->second.metadata};
      return root;
    }
  }
  return type_id;
}

TypeId TypeResolver::Rewrite(TypeId type_id) {
  std::unordered_map<TypeId, TypeId> rewritten_types;
  rewritten_types.reserve(bindings_.size());
  for (const auto& [key, value] : bindings_) {
    rewritten_types.insert(
        {type_registry_.NewPlaceholderType(key), value.type_id});
  }

  return TypeRewriter(type_registry_, type_context_)
      .Rewrite(type_id, rewritten_types);
}
