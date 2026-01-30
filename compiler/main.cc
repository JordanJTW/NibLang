#include <cassert>
#include <cstdio>
#include <fstream>
#include <iomanip>

#include "compiler/assmbler.h"
#include "compiler/program_builder.h"
#include "compiler/tokenizer.h"
#include "src/vm.h"

void print_error(const std::string& file,
                 Token token,
                 std::string_view message) {
  size_t line_start = file.rfind('\n', token.idx);
  if (line_start == std::string_view::npos)
    line_start = 0;
  else
    ++line_start;

  size_t line_end = file.find('\n', token.idx);
  if (line_end == std::string_view::npos)
    line_end = file.size();

  std::string line = file.substr(line_start, line_end - line_start);
  size_t relative_offset = token.idx - line_start;

  constexpr std::string_view kErrorPrefix = "Error: ";
  std::cerr << kErrorPrefix << line << std::endl
            << std::setw(kErrorPrefix.length() + relative_offset) << " "
            << "^ " << message << std::endl;
}

class Compiler {
 public:
  explicit Compiler(const std::string& text) : text_(text), tokenizer_(text_) {
    builder_.EnterFunctionScope("(main)", /*arguments=*/{});
  }

  std::vector<uint8_t> Compile();

 private:
  bool EmitValue(const Token& value);
  void EmitOp(const Token& op);
  std::optional<Token> ParseFunction(Token& current_token,
                                     std::vector<Token>& args);

  bool ParseExpression(Token& current_token);
  bool ParseAssignment(Token& current_token);
  bool ParseComparison(Token& current_token);
  bool ParseMultiplicative(Token& current_token);
  bool ParseAdditive(Token& current_token);
  bool ParseValue(Token& current_token);
  bool ParseCall(Token& current_token, Token fn_name);

  const std::string& text_;
  Tokenizer tokenizer_;
  ProgramBuilder builder_;
};

int main(int argc, char* argv[]) {
  if (argc < 2) {
    fprintf(stderr, "%s <path>\n", argv[0]);
    return 1;
  }

  std::ifstream file(argv[1]);
  if (!file) {
    fprintf(stderr, "Error opening: %s\n", argv[1]);
    return 1;
  }

  std::string contents{std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>()};

  Compiler compiler(contents);

  std::vector<uint8_t> program_image = compiler.Compile();
  std::ofstream out("/tmp/prog.ink", std::ios::binary);

  if (out.is_open()) {
    out.write(reinterpret_cast<const char*>(program_image.data()),
              program_image.size());
    out.close();
  }
  return 0;
}

std::vector<uint8_t> Compiler::Compile() {
  Token token = tokenizer_.next();

  for (;;) {
    if (token.kind == TokenKind::kEndOfFile)
      break;

    if (token.kind == TokenKind::kUnknown) {
      print_error(text_, token, "unknown token");
      token = tokenizer_.next();
      continue;
    }

    if (token.kind == TokenKind::kComment) {
      token = tokenizer_.next();
      continue;
    }

    Token start = token;
    // Function definition i.e. fn $name($args,...):
    if (token.kind == TokenKind::kKwFn) {
      token = tokenizer_.next();

      const Token name = token;
      if (name.kind != TokenKind::kIdent) {
        print_error(text_, name, "requires a function name");
        token = tokenizer_.next();
        continue;
      }

      token = tokenizer_.next();
      if (token.kind != TokenKind::kOpenParen) {
        print_error(text_, name, "expected (");
        token = tokenizer_.next();
        continue;
      }
      token = tokenizer_.next();  // after '('

      std::vector<std::string> arguments;
      if (token.kind != TokenKind::kCloseParen) {
        while (true) {
          if (token.kind != TokenKind::kIdent) {
            print_error(text_, token, "expected parameter name");
            break;
          }

          arguments.push_back(token.value);
          token = tokenizer_.next();

          if (token.kind == TokenKind::kCloseParen)
            break;

          if (token.kind != TokenKind::kComma) {
            print_error(text_, token, "expected ',' or ')'");
            break;
          }

          token = tokenizer_.next();
        }
      }
      builder_.EnterFunctionScope(name.value, arguments);
      token = tokenizer_.next();  // consume ')'

      if (token.kind != TokenKind::kEndExpr) {
        print_error(text_, token, "expected :");
        continue;
      } else {
        token = tokenizer_.next();  // consume :
      }
      continue;
    }

    if (token.kind == TokenKind::kKwLabel) {
      token = tokenizer_.next();
      if (token.kind != TokenKind::kIdent) {
        print_error(text_, token, "expected label name");
        token = tokenizer_.next();
        continue;
      }

      Token label = token;
      token = tokenizer_.next();

      if (token.kind != TokenKind::kEndExpr) {
        print_error(text_, token, "expected :");
        continue;
      } else {
        token = tokenizer_.next();  // consume :
      }

      builder_.GetCurrentCode().Label(label.value);
      continue;
    }

    if (token.kind == TokenKind::kKwGoto) {
      token = tokenizer_.next();
      if (token.kind != TokenKind::kIdent) {
        print_error(text_, token, "expected label name");
        token = tokenizer_.next();
        continue;
      }

      Token if_label = token;
      token = tokenizer_.next();

      if (token.kind != TokenKind::kKwIf) {
        if (token.kind != TokenKind::kEndExpr) {
          print_error(text_, token, "expected ;");
          continue;
        } else {
          token = tokenizer_.next();  // consume ;
        }
        builder_.GetCurrentCode().Jump(if_label.value);
        continue;
      }

      token = tokenizer_.next();
      if (!ParseExpression(token))
        continue;

      if (token.kind != TokenKind::kKwElse) {
        if (token.kind != TokenKind::kEndExpr) {
          print_error(text_, token, "expected ;");
          continue;
        } else {
          token = tokenizer_.next();  // consume ;
        }
        builder_.GetCurrentCode().JumpIfTrue(if_label.value);
        continue;
      }

      token = tokenizer_.next();
      if (token.kind != TokenKind::kIdent) {
        print_error(text_, token, "expected $var name");
        token = tokenizer_.next();
        continue;
      }

      Token else_label = token;
      token = tokenizer_.next();

      if (token.kind != TokenKind::kEndExpr) {
        print_error(text_, token, "expected ;");
        continue;
      } else {
        token = tokenizer_.next();  // consume ;
      }
      builder_.GetCurrentCode().JumpIfTrue(if_label.value);
      continue;
    }

    if (token.kind == TokenKind::kKwEnd) {
      builder_.ExitFunctionScope();
      token = tokenizer_.next();
      continue;
    }

    if (token.kind == TokenKind::kKwReturn) {
      token = tokenizer_.next();
      if (ParseExpression(token)) {
        if (token.kind != TokenKind::kEndExpr) {
          print_error(text_, token, "expected ;");
          continue;
        } else {
          token = tokenizer_.next();  // consume ;
        }
        builder_.GetCurrentCode().Return();
      }
      continue;
    }

    if (token.kind == TokenKind::kKwThrow) {
      token = tokenizer_.next();
      if (ParseExpression(token)) {
        if (token.kind != TokenKind::kEndExpr) {
          print_error(text_, token, "expected ;");
          continue;
        } else {
          token = tokenizer_.next();  // consume ;
        }
        builder_.GetCurrentCode().Throw();
      }
      continue;
    }

    if (ParseExpression(token)) {
      if (token.kind != TokenKind::kEndExpr) {
        print_error(text_, token, "expected ;");
        continue;
      } else {
        token = tokenizer_.next();  // consume ;
      }
      continue;
    }

    print_error(text_, token, "unexpected token");
    token = tokenizer_.next();
  }
  return builder_.GenerateImage();
}

bool Compiler::EmitValue(const Token& value) {
  switch (value.kind) {
    case TokenKind::kNumber: {
      if ((value.value.find('.') != std::string::npos) ||
          value.value.back() == 'f') {
        float f32 = std::stof(value.value);
        builder_.GetCurrentCode().PushFloat(f32);
      } else {
        int i32 = std::stoi(value.value);
        builder_.GetCurrentCode().PushInt32(i32);
      }
    }
      return true;
    case TokenKind::kIdent: {
      std::optional<int> ident_id =
          builder_.GetIdFor(value.value, ProgramBuilder::CreateIfMissing::No);
      if (!ident_id.has_value()) {
        print_error(text_, value, "this id is not defined");
        return false;
      }
      builder_.GetCurrentCode().PushLocal(*ident_id);
      return true;
    }
    case TokenKind::kString: {
      builder_.GetCurrentCode().PushConstRef(
          builder_.GetIdForConstant(value.value));
      return true;
    }
    case TokenKind::kKwTrue: {
      builder_.GetCurrentCode().PushBool(true);
      return true;
    }
    case TokenKind::kKwFalse: {
      builder_.GetCurrentCode().PushBool(false);
      return true;
    }
    default:
      print_error(text_, value, "not able to assign this type");
      return false;
  }
}

void Compiler::EmitOp(const Token& op) {
  switch (op.kind) {
    case TokenKind::kAdd:
      builder_.GetCurrentCode().Add();
      break;
    case TokenKind::kSubtract:
      builder_.GetCurrentCode().Subtract();
      break;
    case TokenKind::kMultiply:
      builder_.GetCurrentCode().Multiply();
      break;
    case TokenKind::kDivide:
      builder_.GetCurrentCode().Divide();
      break;
    case TokenKind::kCompareEq:
      builder_.GetCurrentCode().Compare(OP_EQUAL);
      break;
    case TokenKind::kCompareNe:
      builder_.GetCurrentCode().Compare(OP_EQUAL);
      builder_.GetCurrentCode().Not();
      break;
    case TokenKind::kCompareGt:
      builder_.GetCurrentCode().Compare(OP_GREATER_THAN);
      break;
    case TokenKind::kCompareGe:
      builder_.GetCurrentCode().Compare(OP_GREAT_OR_EQ);
      break;
    case TokenKind::kCompareLt:
      builder_.GetCurrentCode().Compare(OP_LESS_THAN);
      break;
    case TokenKind::kCompareLe:
      builder_.GetCurrentCode().Compare(OP_LESS_OR_EQ);
      break;
    default:
      print_error(text_, op, "unknown op");
      return;
  }
}

bool Compiler::ParseExpression(Token& token) {
  return ParseAssignment(token);
}

bool Compiler::ParseAssignment(Token& token) {
  Token entry_token = token;
  if (token.kind == TokenKind::kIdent) {
    token = tokenizer_.next();
    if (token.kind == TokenKind::kAssign) {
      token = tokenizer_.next();  // consume '='

      if (!ParseExpression(token))
        return false;

      // Store the value on the stack into the appropriate local.
      std::optional<int> var_id = builder_.GetIdFor(
          entry_token.value, ProgramBuilder::CreateIfMissing::Yes);
      builder_.GetCurrentCode().StoreLocal(*var_id);
      return true;
    }
  }

  token = tokenizer_.seekTo(entry_token);
  return ParseComparison(token);
}

bool Compiler::ParseComparison(Token& token) {
  if (!ParseAdditive(token))
    return false;

  while (token.kind == TokenKind::kCompareEq ||
         token.kind == TokenKind::kCompareNe ||
         token.kind == TokenKind::kCompareLt ||
         token.kind == TokenKind::kCompareLe ||
         token.kind == TokenKind::kCompareGt ||
         token.kind == TokenKind::kCompareGe) {
    Token op = token;
    token = tokenizer_.next();

    if (!ParseAdditive(token))
      return false;

    EmitOp(op);
  }
  return true;
}

bool Compiler::ParseAdditive(Token& token) {
  if (!ParseMultiplicative(token))
    return false;

  while (token.kind == TokenKind::kAdd || token.kind == TokenKind::kSubtract) {
    Token op = token;
    token = tokenizer_.next();

    if (!ParseMultiplicative(token))
      return false;

    EmitOp(op);
  }

  return true;
}

bool Compiler::ParseMultiplicative(Token& token) {
  if (!ParseValue(token))
    return false;

  while (token.kind == TokenKind::kMultiply ||
         token.kind == TokenKind::kDivide) {
    Token op = token;
    token = tokenizer_.next();

    if (!ParseValue(token))
      return false;

    EmitOp(op);
  }

  return true;
}

bool Compiler::ParseValue(Token& token) {
  switch (token.kind) {
    case TokenKind::kNumber:
    case TokenKind::kString:
    case TokenKind::kIdent:
    case TokenKind::kKwTrue:
    case TokenKind::kKwFalse: {
      Token value = token;
      token = tokenizer_.next();

      // Function call?
      if (value.kind == TokenKind::kIdent &&
          token.kind == TokenKind::kOpenParen) {
        return ParseCall(token, value);
      }

      return EmitValue(value);
    }

    case TokenKind::kOpenParen: {
      token = tokenizer_.next();  // consume '('

      if (!ParseExpression(token))
        return false;

      if (token.kind != TokenKind::kCloseParen) {
        print_error(text_, token, "expected ')'");
        return false;
      }

      token = tokenizer_.next();  // consume ')'
      return true;
    }

    default:
      print_error(text_, token, "expected expression");
      return false;
  }
}

bool Compiler::ParseCall(Token& token, Token fn_name) {
  token = tokenizer_.next();  // consume '('

  uint32_t argc = 0;
  if (token.kind != TokenKind::kCloseParen) {
    while (true) {
      if (!ParseExpression(token))
        return false;

      ++argc;

      if (token.kind == TokenKind::kCloseParen)
        break;

      if (token.kind != TokenKind::kComma) {
        print_error(text_, token, "expected ',' or ')'");
        return false;
      }

      token = tokenizer_.next();
    }
  }

  token = tokenizer_.next();  // consume ')'

  builder_.CallFunction(fn_name.value, argc);
  return true;
}
