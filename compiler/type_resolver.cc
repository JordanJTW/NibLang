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

// bool TypeResolver::Resolve(
//     NamedBinding binding,
//     const std::vector<std::optional<SpannedType>>& argument_types,
//     std::optional<SpannedType> hint_return_type,
//     std::vector<TypeId>& bindings,
//     std::vector<Metadata>& bound_spans,
//     Metadata expression_metadata) {
//   auto create_pattern_type =
//       [this,
//        &binding](const std::vector<TemplateVariable>& template_variables) {
//         std::vector<TypeId> placeholder_type_ids(template_variables.size(),
//         0); std::vector<Metadata> placeholder_metadata;
//         placeholder_metadata.reserve(template_variables.size());
//         for (size_t idx = 0; idx < template_variables.size(); ++idx) {
//           placeholder_type_ids[idx] =
//               type_registry_.NewPlaceholderType(next_placeholder_idx_++);
//           placeholder_metadata.push_back(template_variables[idx].name.metadata);
//         }
//         return type_context_.GetTemplateOf(binding, placeholder_type_ids,
//                                            placeholder_metadata,
//                                            TypeContext::CheckFunctionBody::NO);
//       };
//
//   Bindings deduced_bindings;
//   if (const auto* symbol =
//           type_registry_.GetSymbol<FunctionSymbol>(*binding.symbol_id)) {
//     auto variables = symbol->declaration.template_variables;
//
//     if (auto pattern_type_id = create_pattern_type(variables)) {
//       const auto* pattern_type =
//           type_registry_.GetType<FunctionType>(*pattern_type_id);
//       if (!pattern_type)
//         return false;
//
//       for (size_t i = 0; i < pattern_type->arg_types.size(); ++i) {
//         if (i >= argument_types.size())
//           break;
//
//         if (!argument_types[i].has_value())
//           continue;
//
//         Resolve(pattern_type->arg_types[i], argument_types[i]->type_id,
//                 argument_types[i]->metadata, deduced_bindings);
//       }
//
//       if (pattern_type->variadic_type) {
//         for (size_t i = pattern_type->arg_types.size();
//              i < argument_types.size(); ++i) {
//           if (!argument_types[i].has_value())
//             continue;
//
//           Resolve(*pattern_type->variadic_type, argument_types[i]->type_id,
//                   argument_types[i]->metadata, deduced_bindings);
//         }
//       }
//
//       if (hint_return_type) {
//         Resolve(pattern_type->return_type, hint_return_type->type_id,
//                 hint_return_type->metadata, deduced_bindings);
//       }
//
//       // Ensure `bindings` is sized correctly and cleared
//       bindings.assign(variables.size(), 0);
//       bound_spans.assign(variables.size(), {});
//
//       bool bound_all_variables = true;
//       for (size_t idx = 0; idx < variables.size(); ++idx) {
//         if (deduced_bindings.contains(idx)) {
//           bindings[idx] = deduced_bindings[idx].type_id;
//           bound_spans[idx] = deduced_bindings[idx].metadata;
//         }
//         //
//         //   error_collector_
//         //       .Add("unable to resolve template variable '" +
//         //                variables[idx].name.text + "'",
//         //            expression_metadata)
//         //       .WithNote("declared here", variables[idx].name.metadata);
//         //   bound_all_variables = false;
//       }
//
//       return bound_all_variables;
//     }
//   }
//
//   else if (const auto* symbol =
//                type_registry_.GetSymbol<StructSymbol>(*binding.symbol_id)) {
//     const auto& variables = symbol->declaration.template_variables;
//     if (auto pattern_type_id = create_pattern_type(variables)) {
//       const auto* pattern_type =
//           type_registry_.GetType<StructType>(*pattern_type_id);
//
//       for (size_t i = 0; i < pattern_type->field_types.size(); ++i) {
//         if (i >= argument_types.size())
//           break;
//
//         if (!argument_types[i].has_value())
//           continue;
//
//         Resolve(pattern_type->field_types[i], argument_types[i]->type_id,
//                 argument_types[i]->metadata, deduced_bindings);
//       }
//
//       if (hint_return_type) {
//         Resolve(*pattern_type_id, hint_return_type->type_id,
//                 hint_return_type->metadata, deduced_bindings);
//       }
//
//       // Ensure `bindings` is sized correctly and cleared
//       bindings.assign(variables.size(), 0);
//       bound_spans.assign(variables.size(), {});
//
//       bool bound_all_variables = true;
//       for (size_t idx = 0; idx < variables.size(); ++idx) {
//         if (deduced_bindings.contains(idx)) {
//           bindings[idx] = deduced_bindings[idx].type_id;
//           bound_spans[idx] = deduced_bindings[idx].metadata;
//         }
//
//         // error_collector_
//         //     .Add("unable to resolve template variable '" +
//         //              variables[idx].name.text + "'",
//         //          expression_metadata)
//         //     .WithNote("declared here", variables[idx].name.metadata);
//         // bound_all_variables = false;
//       }
//
//       return bound_all_variables;
//     }
//   }
//
//   else {
//     LOG(FATAL) << "non-template binding attempting to be instantiated: "
//                << binding;
//   }
//   return false;
// }

bool TypeResolver::Resolve(TypeId pattern_type_id,
                           TypeId concrete_type_id,
                           Metadata resolution_span) {
  pattern_type_id = Prune(pattern_type_id);
  concrete_type_id = Prune(concrete_type_id);

  if (pattern_type_id == concrete_type_id)
    return true;

  const Type& pattern_type = type_registry_.type_table().at(pattern_type_id);
  const Type& concrete_type = type_registry_.type_table().at(concrete_type_id);

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
    return type_context_.IsTypeSubsetOf(concrete_type_id, pattern_type_id);

  bool structural_match = std::visit(
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
              if (!Resolve(c.arg_types[i], p.arg_types[i], resolution_span))
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
            if (&p.symbol != &c.symbol)
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

  if (structural_match)
    return true;

  return type_context_.IsTypeSubsetOf(concrete_type_id, pattern_type_id);
}

std::optional<TypeId> TypeResolver::NewPlaceholderTemplateOf(
    const NamedBinding& binding) {
  auto retrieve_template_variables = [&]() {
    if (const auto* fn_symbol =
            type_registry_.GetSymbol<FunctionSymbol>(*binding.symbol_id)) {
      return std::pair{fn_symbol->declaration.template_variables,
                       fn_symbol->template_variable_type_ids};
    }

    if (const auto* struct_symbol =
            type_registry_.GetSymbol<StructSymbol>(*binding.symbol_id)) {
      return std::pair{struct_symbol->declaration.template_variables,
                       struct_symbol->template_variable_type_ids};
    }
    NOTREACHED() << "Unknown Symbol type";
    return std::pair{std::vector<TemplateVariable>{}, std::vector<TypeId>{}};
  };

  const auto& [template_variables, template_type_ids] =
      retrieve_template_variables();
  std::vector<TypeId> placeholder_type_ids(template_variables.size(), 0);
  std::vector<Metadata> placeholder_metadata;
  placeholder_metadata.reserve(template_variables.size());
  for (size_t idx = 0; idx < template_variables.size(); ++idx) {
    placeholder_type_ids[idx] =
        type_registry_.NewPlaceholderType(next_placeholder_idx_++);
    placeholder_metadata.push_back(template_variables.at(idx).name.metadata);
    template_variables_.push_back(template_type_ids.at(idx));
  }
  return type_context_.GetTemplateOf(binding, placeholder_type_ids,
                                     placeholder_metadata,
                                     TypeContext::CheckFunctionBody::NO);
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

std::string TypeResolver::ToString() const {
  std::stringstream ss;
  for (const auto& [key, value] : bindings_) {
    ss << "$" << key << " => "
       << type_registry_.GetNameFromTypeId(value.type_id);
  }
  return ss.str();
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
