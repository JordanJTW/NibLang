// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/type_resolver.h"

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

TypeResolver::TypeResolver(TypeContext& type_context,
                           ErrorCollector& error_collector)
    : type_context_(type_context), error_collector_(error_collector) {}

bool TypeResolver::Resolve(const ParsedType& pattern_type,
                           const ParsedType& concrete_type,
                           const std::vector<std::string>& template_names,
                           Bindings& bindings) {
  if (const std::string* name = std::get_if<std::string>(&pattern_type.type)) {
    if (std::find(template_names.begin(), template_names.end(), *name) !=
        template_names.end()) {
      if (auto it = bindings.find(*name); it != bindings.end()) {
        if (type_context_.GetTypeIdFor(it->second) ==
            type_context_.GetTypeIdFor(concrete_type)) {
          return true;
        }

        LOG(ERROR) << "Binding already set for: " << *name << " (" << it->second
                   << " vs. " << concrete_type << ")";
        return false;
      }

      bindings[*name] = concrete_type;
      return true;
    }
  }

  // Allows binding concrete-types to an optional pattern T? -- handles Nil.
  if (std::holds_alternative<ParsedOptionalType>(pattern_type.type) &&
      !std::holds_alternative<ParsedOptionalType>(concrete_type.type)) {
    return Resolve(
        *std::get<ParsedOptionalType>(pattern_type.type).wrapped_type,
        concrete_type, template_names, bindings);
  }

  if (pattern_type.type.index() != concrete_type.type.index())
    return false;

  return std::visit(
      Overloaded{
          [&](const std::string& p, const std::string& c) { return p == c; },
          [&](const ParsedUnionType& p, const ParsedUnionType& c) {
            if (p.names.size() != c.names.size()) {
              return false;
            }
            for (size_t i = 0; i < p.names.size(); ++i) {
              if (!Resolve(p.names[i], c.names[i], template_names, bindings))
                return false;
            }
            return true;
          },
          [&](const ParsedFunctionType& p, const ParsedFunctionType& c) {
            if (p.arguments.size() != c.arguments.size())
              return false;
            for (size_t i = 0; i < p.arguments.size(); ++i) {
              if (!Resolve(p.arguments[i], c.arguments[i], template_names,
                           bindings)) {
                return false;
              }
            }
            return Resolve(*p.return_value, *c.return_value, template_names,
                           bindings);
          },
          [&](const ParsedOptionalType& p, const ParsedOptionalType& c) {
            return Resolve(*p.wrapped_type, *c.wrapped_type, template_names,
                           bindings);
          },
          [&](const ParsedParameterizedType& p,
              const ParsedParameterizedType& c) {
            if (!Resolve(*p.type, *c.type, template_names, bindings)) {
              return false;
            }
            if (p.parameters.size() != c.parameters.size()) {
              return false;
            }
            for (size_t i = 0; i < p.parameters.size(); ++i) {
              if (!Resolve(p.parameters[i], c.parameters[i], template_names,
                           bindings)) {
                return false;
              }
            }
            return true;
          },
          [&](const auto&, const auto&) {
            NOTREACHED() << "Overload list MUST be exhaustive.";
            return false;
          }},
      pattern_type.type, concrete_type.type);
}
