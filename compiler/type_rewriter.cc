// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/type_rewriter.h"

#include "compiler/type_context.h"
#include "compiler/type_registry.h"

namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

}  // namespace

TypeRewriter::TypeRewriter(TypeRegistry& type_registry,
                           TypeContext& type_context)
    : type_registry_(type_registry), type_context_(type_context) {}

TypeId TypeRewriter::Rewrite(TypeId type_id,
                             const SubstitutionMap& substitutions) {
  if (auto it = substitutions.find(type_id); it != substitutions.end()) {
    if (it->second != type_id)
      return Rewrite(it->second, substitutions);
  }

  return std::visit(
      Overloaded{
          [&](const BuiltInType&) { return type_id; },
          [&](const FunctionType& type) {
            bool is_updated = false;

            std::vector<TypeId> rewritten_arg_types;
            for (const auto& arg_type_id : type.arg_types) {
              TypeId rewrite_type_id = Rewrite(arg_type_id, substitutions);
              rewritten_arg_types.push_back(rewrite_type_id);
              is_updated |= (arg_type_id != rewrite_type_id);
            }

            TypeId rewritten_return_type =
                Rewrite(type.return_type, substitutions);
            is_updated |= (rewritten_return_type != type.return_type);

            std::optional<TypeId> rewritten_variadic_type;
            if (type.variadic_type) {
              rewritten_variadic_type =
                  Rewrite(*type.variadic_type, substitutions);
              is_updated |= (type.variadic_type != rewritten_variadic_type);
            }

            if (!is_updated)
              return type_id;

            return type_registry_.NewFunctionType(
                FunctionType{std::move(rewritten_arg_types),
                             rewritten_return_type, rewritten_variadic_type});
          },
          [&](const StructType& type) {
            bool is_updated = false;

            std::vector<TypeId> rewritten_template_types;
            for (const auto& template_type_id :
                 type.instance_template_type_ids) {
              TypeId rewritten_type_id =
                  Rewrite(template_type_id, substitutions);
              rewritten_template_types.push_back(rewritten_type_id);
              is_updated |= (template_type_id != rewritten_type_id);
            }

            if (!is_updated)
              return type_id;

            return type_registry_.NewStructType(
                StructType{type.symbol, rewritten_template_types},
                std::nullopt);
          },
          [&](const UnionType& type) {
            bool is_updated = false;

            std::vector<TypeId> rewritten_member_types;
            for (const auto& member_id : type.types) {
              TypeId rewritten_type_id = Rewrite(member_id, substitutions);
              rewritten_member_types.push_back(rewritten_type_id);
              is_updated |= (member_id != rewritten_type_id);
            }

            if (!is_updated)
              return type_id;

            return type_registry_.NewUnionType(
                UnionType{std::move(rewritten_member_types)});
          },
          [&](const IntersectionType& type) {
            bool is_updated = false;

            std::vector<TypeId> rewritten_member_types;
            for (const auto& member_id : type.types) {
              TypeId rewritten_type_id = Rewrite(member_id, substitutions);
              rewritten_member_types.push_back(rewritten_type_id);
              is_updated |= (member_id != rewritten_type_id);
            }

            if (!is_updated)
              return type_id;

            return type_registry_.NewIntersectionType(
                IntersectionType{std::move(rewritten_member_types)});
          },
          [&](const OptionalType& type) {
            TypeId rewritten_wrapped_type =
                Rewrite(type.wrapped_type, substitutions);

            return rewritten_wrapped_type != type.wrapped_type
                       ? type_registry_.NewOptionalType(rewritten_wrapped_type)
                       : type_id;
          },
          // TODO: Rewrite `target_type_id` if we support `alias Foo[T] = ...`
          // or we start allowing `alias` within a template environment (in a
          // struct/function) and it can use the outer template variables.
          [&](const AliasType&) { return type_id; },
          [&](const PlaceholderType&) { return type_id; },
          [&](const TemplateVariableType&) { return type_id; }},
      type_registry_.type_table().at(type_id));
}