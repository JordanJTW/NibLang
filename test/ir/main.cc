#include "test/ir/tokenizer.h"

#include <cstdio>
#include <fstream>
#include <iomanip>

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
  explicit Compiler(const std::string& text)
      : text_(text),
        tokenizer_(text_),
        current_function_(std::make_unique<Assembler>()) {}
  ~Compiler() { DumpByteCode(current_function_->Build()); }

  void Compile();

 private:
  bool PushValue(const Token& value);
  void WriteAssign(const Token& var, const Token& value);
  void WriteBinaryAssign(const Token& var,
                         const Token& v1,
                         const Token& op,
                         const Token& v2);

  std::optional<int> GetIdFor(const std::string& name, bool create_if_missing);

  const std::string& text_;
  Tokenizer tokenizer_;

  std::map<std::string, int> var_to_id_;
  int next_id_{0};
  std::map<std::string, int> const_ids_;
  int next_const_id_{0};

  std::unique_ptr<Assembler> current_function_;
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
    if (token.kind == TokenKind::kIdent) {
      token = tokenizer_.next();

      if (token.kind != TokenKind::kAssign) {
        print_error(text_, token, "expected assign (=)");
        token = tokenizer_.next();
        continue;
      }

      token = tokenizer_.next();
      if (token.kind != TokenKind::kIdent && token.kind != TokenKind::kNumber &&
          token.kind != TokenKind::kString) {
        print_error(text_, token, "expected $var or Number");
        token = tokenizer_.next();
        continue;
      }

      Token v1 = token;
      token = tokenizer_.next();

      if (token.kind != TokenKind::kAdd && token.kind != TokenKind::kSubtract) {
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

    print_error(text_, token, "unexpected token");
    token = tokenizer_.next();
  }
}

std::optional<int> Compiler::GetIdFor(const std::string& name,
                                      bool create_if_missing) {
  if (auto it = var_to_id_.find(name); it != var_to_id_.cend())
    return it->second;

  if (!create_if_missing)
    return std::nullopt;

  var_to_id_[name] = next_id_;
  return next_id_++;
}

bool Compiler::PushValue(const Token& value) {
  switch (value.kind) {
    case TokenKind::kNumber: {
      if ((value.value.find('.') != std::string::npos) ||
          value.value.back() == 'f') {
        float f32 = std::stof(value.value);
        current_function_->PushFloat(f32);
      } else {
        int i32 = std::stoi(value.value);
        current_function_->PushInt32(i32);
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
      current_function_->PushLocal(*ident_id);
      return true;
    }
    case TokenKind::kString: {
      if (auto it = const_ids_.find(value.value); it != const_ids_.cend()) {
        current_function_->PushConstRef(it->second);
      } else {
        current_function_->PushConstRef(next_const_id_);
        const_ids_[value.value] = next_const_id_;
      }
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
    current_function_->StoreLocal(*var_id);
  }
}

void Compiler::WriteBinaryAssign(const Token& var,
                                 const Token& v1,
                                 const Token& op,
                                 const Token& v2) {
  if (PushValue(v1) && PushValue(v2)) {
    switch (op.kind) {
      case TokenKind::kAdd:
        current_function_->Add();
        break;
      case TokenKind::kSubtract:
        current_function_->Subtract();
        break;
      default:
        print_error(text_, op, "unknown op");
        return;
    }

    std::optional<int> var_id = GetIdFor(var.value, /*create_if_missing=*/true);
    current_function_->StoreLocal(*var_id);
  }
}