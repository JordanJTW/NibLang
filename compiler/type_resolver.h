// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "compiler/error_collector.h"
#include "compiler/type_context.h"
#include "compiler/types.h"

class TypeResolver {
 public:
  using Bindings = std::unordered_map<std::string, ParsedType>;

  explicit TypeResolver(TypeContext& type_context,
                        ErrorCollector& error_collector);

  // Performs pattern matching on `concrete_type` using `pattern_type` to infer
  // any template parameter names found in `template_names`. If an error occurs
  // due to a mismatch between `concrete_type` and `pattern_type` then false is
  // returned. Any bindings that were determined will be added to `bindings`.
  // If a conflicting ParsedType is resolved for an existing template parameter
  // in `bindings` then false will be returned (and an error logged).
  // Array[Box[i32]] + Array[T] => {T: Box[i32]}
  bool Resolve(const ParsedType& pattern_type,
               const ParsedType& concrete_type,
               const std::vector<std::string>& template_names,
               Bindings& bindings);

 private:
  TypeContext& type_context_;
  ErrorCollector& error_collector_;
};