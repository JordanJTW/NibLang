#include "test/ir/tokenizer.h"

#include <cstdio>
#include <fstream>
#include <iomanip>

#include "src/vm.h"
#include "test/assmbler.h"

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
    Function& main = functions_.emplace_back();
    main.name = "(main)";

    func_ids_["StringGet"] = VM_BUILTIN_STRINGS_GET;
    func_ids_["StringSubstr"] = VM_BUILTIN_STRINGS_SUBSTRING;
    func_ids_["StringStartsWith"] = VM_BUILTIN_STRINGS_STARTWITH;
  }
  ~Compiler() {
    for (Function fn : functions_) {
      std::cout << "fn " << fn.name << " (" << fn.next_id << ") vars: ";
      for (const auto& [name, id] : fn.var_to_id) {
        std::cout << name << ", ";
      }
      std::cout << "\n";
      DumpByteCode(fn.code.Build());
      std::cout << std::endl;
    }
  }

  void Compile();

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

  std::optional<int> GetIdFor(const std::string& name, bool create_if_missing);

  const std::string& text_;
  Tokenizer tokenizer_;

  struct Function {
    std::string name;
    std::map<std::string, int> var_to_id;
    int next_id{0};
    Assembler code;
  };
  std::vector<Function> functions_;

  std::map<std::string, int> const_ids_;
  int next_const_id_{0};
  std::map<std::string, int> func_ids_;
  int next_func_id_{0};
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
  compiler.Compile();
  return 0;
}

void Compiler::Compile() {
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

      func_ids_[name.value] = next_func_id_++;
      Function& function = functions_.emplace_back();
      function.name = name.value;

      token = tokenizer_.next();  // after '('
      if (token.kind != TokenKind::kCloseParen) {
        while (true) {
          if (token.kind != TokenKind::kIdent) {
            print_error(text_, token, "expected parameter name");
            break;
          }

          function.var_to_id[token.value] = function.next_id++;
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

      functions_.back().code.Label(label.value);
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
        functions_.back().code.Jump(if_label.value);
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
        functions_.back().code.JumpIfFalse(if_label.value);
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
      functions_.back().code.JumpIfFalse(if_label.value);
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
}

std::optional<int> Compiler::GetIdFor(const std::string& name,
                                      bool create_if_missing) {
  Function& fn = functions_.back();
  if (auto it = fn.var_to_id.find(name); it != fn.var_to_id.cend())
    return it->second;

  if (!create_if_missing)
    return std::nullopt;

  fn.var_to_id[name] = fn.next_id;
  return fn.next_id++;
}

bool Compiler::EmitValue(const Token& value) {
  switch (value.kind) {
    case TokenKind::kNumber: {
      if ((value.value.find('.') != std::string::npos) ||
          value.value.back() == 'f') {
        float f32 = std::stof(value.value);
        functions_.back().code.PushFloat(f32);
      } else {
        int i32 = std::stoi(value.value);
        functions_.back().code.PushInt32(i32);
      }
    }
      return true;
    case TokenKind::kIdent: {
      std::optional<int> ident_id =
          GetIdFor(value.value, /*create_if_missing=*/false);
      if (!ident_id.has_value()) {
        print_error(text_, value, "this id is not defined");
        return false;
      }
      functions_.back().code.PushLocal(*ident_id);
      return true;
    }
    case TokenKind::kString: {
      if (auto it = const_ids_.find(value.value); it != const_ids_.cend()) {
        functions_.back().code.PushConstRef(it->second);
      } else {
        functions_.back().code.PushConstRef(next_const_id_);
        const_ids_[value.value] = next_const_id_;
      }
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
      functions_.back().code.Add();
      break;
    case TokenKind::kSubtract:
      functions_.back().code.Subtract();
      break;
    case TokenKind::kMultiply:
      functions_.back().code.Multiply();
      break;
    case TokenKind::kDivide:
      functions_.back().code.Divide();
      break;
    case TokenKind::kCompareEq:
      functions_.back().code.Compare(OP_EQUAL);
      break;
    case TokenKind::kCompareNe:
      functions_.back().code.Compare(OP_EQUAL);
      functions_.back().code.Not();
      break;
    case TokenKind::kCompareGt:
      functions_.back().code.Compare(OP_GREATER_THAN);
      break;
    case TokenKind::kCompareGe:
      functions_.back().code.Compare(OP_GREAT_OR_EQ);
      break;
    case TokenKind::kCompareLt:
      functions_.back().code.Compare(OP_LESS_THAN);
      break;
    case TokenKind::kCompareLe:
      functions_.back().code.Compare(OP_LESS_OR_EQ);
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
      std::optional<int> var_id =
          GetIdFor(entry_token.value, /*create_if_missing=*/true);
      functions_.back().code.StoreLocal(*var_id);
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
    case TokenKind::kIdent: {
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

  if (token.kind != TokenKind::kCloseParen) {
    while (true) {
      if (!ParseExpression(token))
        return false;

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

  auto it = func_ids_.find(fn_name.value);
  if (it == func_ids_.end()) {
    print_error(text_, fn_name, "unknown function");
    return true;  // semantic error, parse is correct
  }

  functions_.back().code.Call(it->second);
  return true;
}