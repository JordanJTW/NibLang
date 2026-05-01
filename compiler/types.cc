// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/types.h"

#include <ostream>

namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

std::ostream& operator<<(std::ostream& os, Symbol::Kind kind) {
  switch (kind) {
    case Symbol::Function:
      return os << "Function";
    case Symbol::Struct:
      return os << "Struct";
    case Symbol::Field:
      return os << "Field";
    case Symbol::Variable:
      return os << "Variable";
    case Symbol::Capture:
      return os << "Capture";
    case Symbol::Narrowed:
      return os << "Narrowed";
  }

  __builtin_unreachable();  // All Symbol::Kind MUST be handled above.
  return os;
}

}  // namespace

std::ostream& operator<<(std::ostream& os, const Symbol& symbol) {
  os << "{kind=" << symbol.kind << ", type_id=" << symbol.type_id;
  if (symbol.idx.has_value())
    os << ", idx=" << symbol.idx.value();
  os << "}";
  return os;
}

std::ostream& operator<<(std::ostream& os, const ParsedType& type) {
  std::visit(Overloaded{[&](const std::string& type_name) { os << type_name; },
                        [&](const ParsedUnionType& type) {
                          for (const auto& name : type.names) {
                            for (size_t i = 0; i < type.names.size(); ++i) {
                              if (i > 0) {
                                os << "|";
                              }
                              os << type.names[i];
                            }
                          }
                        },
                        [&](const ParsedFunctionType& type) {
                          os << "fn (";
                          for (const auto& arg : type.arguments)
                            os << arg;
                          if (type.return_value)
                            os << ") -> " << *type.return_value;
                          else
                            os << ")";
                        },
                        [&](const ParsedOptionalType& type) {
                          os << *type.wrapped_type << "?";
                        }},
             type.type);
  return os;
}