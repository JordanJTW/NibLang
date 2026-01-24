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
  bool PushValue(const Token& value);
  void WriteAssign(const Token& var, const Token& value);
  void WriteBinaryAssign(const Token& var,
                         const Token& v1,
                         const Token& op,
                         const Token& v2);
  std::optional<Token> ParseFunction(Token& current_token,
                                     std::vector<Token>& args);

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
    // Function definition (fn $name $args...)
    if (token.kind == TokenKind::kKwFn) {
      token = tokenizer_.next();
      std::vector<Token> args;
      if (auto name = ParseFunction(token, args); name.has_value()) {
        Function& function = functions_.emplace_back();
        function.name = name->value;
        for (const auto& arg : args)
          function.var_to_id[arg.value] = function.next_id++;
        func_ids_[name->value] = next_func_id_++;
      }
      continue;
    }

    // Assignment ($var = ...)
    if (token.kind == TokenKind::kIdent) {
      token = tokenizer_.next();

      if (token.kind != TokenKind::kAssign) {
        print_error(text_, token, "expected assign (=)");
        token = tokenizer_.next();
        continue;
      }

      token = tokenizer_.next();
      // Assign to result of function call ($var = call $name $args...)
      if (token.kind == TokenKind::kKwCall) {
        token = tokenizer_.next();
        std::vector<Token> args;
        if (auto name = ParseFunction(token, args); name.has_value()) {
          auto it = func_ids_.find(name->value);
          if (it == func_ids_.cend()) {
            print_error(text_, *name, "unknown function");
            continue;
          }

          for (Token arg : args) {
            PushValue(arg);
          }
          functions_.back().code.Call(it->second);

          std::optional<int> var_id =
              GetIdFor(start.value, /*create_if_missing=*/true);
          functions_.back().code.StoreLocal(*var_id);
        }
        continue;
      }
      // Assign to literal value ($var = $other, $var = 10, $var = "hello")
      else if (token.kind != TokenKind::kIdent &&
               token.kind != TokenKind::kNumber &&
               token.kind != TokenKind::kString &&
               token.kind != TokenKind::kChar) {
        print_error(text_, token, "expected $var or Number");
        token = tokenizer_.next();
        continue;
      }

      Token v1 = token;
      token = tokenizer_.next();

      // Assign to binary expression ($var = 10 + ...)
      if (token.kind != TokenKind::kAdd && token.kind != TokenKind::kSubtract &&
          token.kind != TokenKind::kCompareEq &&
          token.kind != TokenKind::kCompareNe &&
          token.kind != TokenKind::kCompareLt &&
          token.kind != TokenKind::kCompareLe &&
          token.kind != TokenKind::kCompareGt &&
          token.kind != TokenKind::kCompareGe) {
        std::cout << "is not op!" << std::endl;

        if (token.kind != TokenKind::kEndExpr) {
          print_error(text_, token, "expected ;");
          continue;
        } else {
          token = tokenizer_.next();  // consume ;
        }
        WriteAssign(start, v1);
        continue;
      }

      Token op = token;
      token = tokenizer_.next();

      if (token.kind != TokenKind::kIdent && token.kind != TokenKind::kNumber) {
        print_error(text_, token, "expected $var or Number");
        token = tokenizer_.next();
        continue;
      }

      Token v2 = token;
      token = tokenizer_.next();

      if (token.kind != TokenKind::kEndExpr) {
        print_error(text_, token, "expected ;");
        continue;
      } else {
        token = tokenizer_.next();  // consume ;
      }

      WriteBinaryAssign(start, v1, op, v2);
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
      if (token.kind != TokenKind::kIdent) {
        print_error(text_, token, "expected $var name");
        token = tokenizer_.next();
        continue;
      }

      Token if_var = token;
      std::optional<int> var_id =
          GetIdFor(if_var.value, /*create_if_missing=*/false);

      if (!var_id.has_value()) {
        print_error(text_, if_var, "unknown $var");
        token = tokenizer_.next();
        continue;
      }

      token = tokenizer_.next();
      if (token.kind != TokenKind::kKwElse) {
        if (token.kind != TokenKind::kEndExpr) {
          print_error(text_, token, "expected ;");
          continue;
        } else {
          token = tokenizer_.next();  // consume ;
        }
        functions_.back().code.PushLocal(*var_id);
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
      functions_.back().code.PushLocal(*var_id);
      functions_.back().code.JumpIfFalse(if_label.value);
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

bool Compiler::PushValue(const Token& value) {
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
    case TokenKind::kChar: {
      if (value.value.length() != 1) {
        print_error(text_, value, "char should be a single byte");
        return false;
      }

      int i32 = value.value[0];
      functions_.back().code.PushInt32(i32);
      return true;
    }
    default:
      print_error(text_, value, "not able to assign this type");
      return false;
  }
}

void Compiler::WriteAssign(const Token& var, const Token& value) {
  if (PushValue(value)) {
    std::optional<int> var_id = GetIdFor(var.value, /*create_if_missing=*/true);
    functions_.back().code.StoreLocal(*var_id);
  }
}

std::optional<Token> Compiler::ParseFunction(Token& current_token,
                                             std::vector<Token>& args) {
  const Token name = current_token;
  if (name.kind != TokenKind::kIdent) {
    print_error(text_, name, "requires a function name");
    current_token = tokenizer_.next();
    return std::nullopt;
  }

  current_token = tokenizer_.next();
  while (current_token.kind != TokenKind::kEndExpr) {
    args.push_back(current_token);
    current_token = tokenizer_.next();
  }
  current_token = tokenizer_.next();  // skip ;
  return name;
}

void Compiler::WriteBinaryAssign(const Token& var,
                                 const Token& v1,
                                 const Token& op,
                                 const Token& v2) {
  if (PushValue(v1) && PushValue(v2)) {
    switch (op.kind) {
      case TokenKind::kAdd:
        functions_.back().code.Add();
        break;
      case TokenKind::kSubtract:
        functions_.back().code.Subtract();
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

    std::optional<int> var_id = GetIdFor(var.value, /*create_if_missing=*/true);
    functions_.back().code.StoreLocal(*var_id);
  }
}