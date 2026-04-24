
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "compiler/program_builder.h"
#include "compiler/tokenizer.h"
#include "compiler/types.h"

class Parser {
 public:
  explicit Parser(const std::string& text) : text_(text), tokenizer_(text_) {
    AdvanceToken();
  }

  Block Parse();

 private:
  enum class BlockType { Root, Scope };
  void ParseBlock(Block& block, BlockType type = BlockType::Scope);

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

  enum class ExternStruct { YES, NO };
  std::optional<StructDeclaration> ParseStructDeclaration(
      ExternStruct is_extern);
  std::optional<FunctionDeclaration> ParseFunctionDeclaration(
      FunctionKind function_kind);

  std::optional<ParsedType> ParseType();
  std::optional<ParsedType> ParseUnionType();
  std::optional<ParsedType> ParseFunctionType();
  std::optional<ParsedType> ParsePrimaryType();

  std::optional<Token> ExpectNextToken(TokenKind expected_kind,
                                       std::string_view error_message);
  void AdvanceToken();
  void HandleError(std::string_view message);

  const std::string& text_;
  Tokenizer tokenizer_;
  Token current_token_;
};