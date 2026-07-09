// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "compiler/error_collector.h"
#include "compiler/program_builder.h"
#include "compiler/tokenizer.h"
#include "compiler/types.h"

class Parser {
 public:
  explicit Parser(const std::string& text,
                  ErrorCollector& error_collector,
                  size_t file_id)
      : error_collector_(error_collector), tokenizer_(text, file_id) {
    AdvanceToken();
  }

  Block Parse();

 private:
  enum class BlockType { Root, Scope };
  void ParseBlock(Block& block, BlockType type = BlockType::Scope);

  std::unique_ptr<Expression> ParseBlockCondition();

  std::unique_ptr<Expression> ParseExpression();
  std::unique_ptr<Expression> ParseAssignment();
  std::unique_ptr<Expression> ParseLogical();
  std::unique_ptr<Expression> ParseComparison();
  std::unique_ptr<Expression> ParseAdditive();
  std::unique_ptr<Expression> ParseMultiplicative();
  std::unique_ptr<Expression> ParseUnary();
  std::unique_ptr<Expression> ParseInFix();
  std::unique_ptr<Expression> ParsePostFix();
  std::unique_ptr<Expression> ParsePrimary();
  std::unique_ptr<Expression> ParseValue();

  std::unique_ptr<Expression> ParseCall(std::unique_ptr<Expression> callee);

  std::optional<std::vector<TemplateArgument>> ParseTemplateDeclarationList();

  enum class ExternStruct { YES, NO };
  std::optional<StructDeclaration> ParseStructDeclaration(
      ExternStruct is_extern);
  std::optional<FunctionDeclaration> ParseFunctionDeclaration(
      FunctionKind function_kind);
  struct FunctionArgumentList {
    std::vector<std::pair<SpannedText, ParsedType>> arguments;
    std::optional<VariadicType> variadic_type;
  };
  std::optional<FunctionArgumentList> ParseFunctionArgumentList();

  std::optional<ParsedType> ParseType();
  std::optional<ParsedType> ParseUnionType();
  std::optional<ParsedType> ParseFunctionType();
  std::optional<ParsedType> ParsePrimaryType();
  std::vector<ParsedType> ParseTypeList(TokenKind end_of_list_token);

  bool ConsumeToken(TokenKind expected_kind, std::string error_message);
  void AdvanceToken();

  bool SynchronizeOnError(std::function<bool(TokenKind)> is_target =
                              [](TokenKind) { return false; });

  ErrorCollector& error_collector_;
  Tokenizer tokenizer_;

  Token current_token_;
  size_t anonymous_fn_counter_{0};
};