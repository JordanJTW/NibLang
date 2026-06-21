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

std::ostream& operator<<(std::ostream& os, NamedBinding::Kind kind) {
  switch (kind) {
    case NamedBinding::Function:
      return os << "Function";
    case NamedBinding::Struct:
      return os << "Struct";
    case NamedBinding::Field:
      return os << "Field";
    case NamedBinding::Argument:
      return os << "Argument";
    case NamedBinding::Variable:
      return os << "Variable";
    case NamedBinding::Capture:
      return os << "Capture";
    case NamedBinding::Narrowed:
      return os << "Narrowed";
    case NamedBinding::Template:
      return os << "Template";
  }

  __builtin_unreachable();  // All Symbol::Kind MUST be handled above.
  return os;
}

}  // namespace

bool FunctionSymbol::IsExtern() const {
  if (parent_declaration.has_value())
    return parent_declaration.value()->is_extern && !declaration.body;

  return declaration.function_kind == FunctionKind::Extern;
}

std::string FunctionSymbol::GetName() const {
  if (parent_declaration.has_value())
    return parent_declaration.value()->name + "_" + declaration.name;

  return declaration.name;
}

std::ostream& operator<<(std::ostream& os, const NamedBinding& symbol) {
  os << "{kind=" << symbol.kind << ", type_id="
     << (symbol.realized_type_id ? std::to_string(*symbol.realized_type_id)
                                 : "?")
     << ", symbol_id="
     << (symbol.symbol_id ? std::to_string(*symbol.symbol_id) : "?");
  if (symbol.idx.has_value())
    os << ", idx=" << symbol.idx.value();
  if (symbol.parent_type_id.has_value())
    os << ", parent_type_id=" << *symbol.parent_type_id;
  os << "}";
  return os;
}

std::ostream& operator<<(std::ostream& os, const ParsedType& type) {
  std::visit(
      Overloaded{[&](const std::string& type_name) { os << "#" << type_name; },
                 [&](const ParsedUnionType& type) {
                   os << "#";
                   for (size_t i = 0; i < type.names.size(); ++i) {
                     if (i > 0) {
                       os << "|";
                     }
                     os << type.names[i];
                   }
                 },
                 [&](const ParsedFunctionType& type) {
                   os << "#fn (";
                   for (size_t i = 0; i < type.arguments.size(); ++i) {
                     if (i > 0)
                       os << ", ";
                     os << type.arguments[i];
                   }
                   if (type.return_value)
                     os << ") -> " << *type.return_value;
                   else
                     os << ")";
                 },
                 [&](const ParsedOptionalType& type) {
                   os << "#" << *type.wrapped_type << "?";
                 },
                 [&](const ParsedParameterizedType& type) {
                   os << *type.type << "[";
                   for (size_t i = 0; i < type.parameters.size(); ++i) {
                     if (i > 0) {
                       os << ", ";
                     }
                     os << type.parameters[i];
                   }
                   os << "]";
                 }},
      type.type);
  return os;
}