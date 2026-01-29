#include <cassert>
#include <cstdio>
#include <fstream>
#include <iomanip>

#include "compiler/assmbler.h"
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
    Function& main = functions_.emplace_back();
    main.name = "(main)";
    function_decl_stack_.push(0);

    func_ids_["String_charAt"] = VM_BUILTIN_STRINGS_GET;
    func_ids_["String_substr"] = VM_BUILTIN_STRINGS_SUBSTRING;
    func_ids_["String_startsWith"] = VM_BUILTIN_STRINGS_STARTWITH;
    func_ids_["String_length"] = VM_BUILTIN_STRING_LENGTH;
    func_ids_["Map_new"] = VM_BUILTIN_MAP_NEW;
    func_ids_["Map_set"] = VM_BUILTIN_MAP_SET;
    func_ids_["Array_new"] = VM_BUILTIN_ARRAY_NEW;
    func_ids_["Array_get"] = VM_BUILTIN_ARRAY_GET;
    func_ids_["Array_set"] = VM_BUILTIN_ARRAY_SET;
    func_ids_["log"] = VM_BUILTIN_LOG;
  }

  void Compile();
  std::vector<uint8_t> GenerateImage() const;

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
  Assembler& GetCurrentCode();

  const std::string& text_;
  Tokenizer tokenizer_;

  struct Function {
    std::string name;
    std::map<std::string, int> var_to_id;
    uint16_t next_id{0};
    uint16_t argc{0};
    Assembler code;
    std::map<size_t, std::string> unresolved_calls;
  };
  std::vector<Function> functions_;

  std::map<std::string, int> const_ids_;
  int next_const_id_{0};
  std::map<std::string, int> func_ids_;
  int next_func_id_{1};  // main() is always idx 0
  std::stack<int> function_decl_stack_;
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

  std::vector<uint8_t> program_image = compiler.GenerateImage();
  std::ofstream out("/tmp/prog.ink", std::ios::binary);

  if (out.is_open()) {
    out.write(reinterpret_cast<const char*>(program_image.data()),
              program_image.size());
    out.close();
  }
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

      size_t fn_id = next_func_id_++;
      func_ids_[name.value] = fn_id;
      Function& function = functions_.emplace_back();
      function.name = name.value;
      function_decl_stack_.push(fn_id);

      token = tokenizer_.next();  // after '('
      if (token.kind != TokenKind::kCloseParen) {
        while (true) {
          if (token.kind != TokenKind::kIdent) {
            print_error(text_, token, "expected parameter name");
            break;
          }

          function.var_to_id[token.value] = function.next_id++;
          ++function.argc;
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

      GetCurrentCode().Label(label.value);
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
        GetCurrentCode().Jump(if_label.value);
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
        GetCurrentCode().JumpIfTrue(if_label.value);
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
      GetCurrentCode().JumpIfTrue(if_label.value);
      continue;
    }

    if (token.kind == TokenKind::kKwEnd) {
      function_decl_stack_.pop();
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
        GetCurrentCode().Return();
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
        GetCurrentCode().Throw();
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
}

std::optional<int> Compiler::GetIdFor(const std::string& name,
                                      bool create_if_missing) {
  Function& fn = functions_[function_decl_stack_.top()];
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
        GetCurrentCode().PushFloat(f32);
      } else {
        int i32 = std::stoi(value.value);
        GetCurrentCode().PushInt32(i32);
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
      GetCurrentCode().PushLocal(*ident_id);
      return true;
    }
    case TokenKind::kString: {
      if (auto it = const_ids_.find(value.value); it != const_ids_.cend()) {
        GetCurrentCode().PushConstRef(it->second);
      } else {
        GetCurrentCode().PushConstRef(next_const_id_);
        const_ids_[value.value] = next_const_id_;
        const_ids_[value.value] = next_const_id_++;
      }
      return true;
    }
    case TokenKind::kKwTrue: {
      GetCurrentCode().PushBool(true);
      return true;
    }
    case TokenKind::kKwFalse: {
      GetCurrentCode().PushBool(false);
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
      GetCurrentCode().Add();
      break;
    case TokenKind::kSubtract:
      GetCurrentCode().Subtract();
      break;
    case TokenKind::kMultiply:
      GetCurrentCode().Multiply();
      break;
    case TokenKind::kDivide:
      GetCurrentCode().Divide();
      break;
    case TokenKind::kCompareEq:
      GetCurrentCode().Compare(OP_EQUAL);
      break;
    case TokenKind::kCompareNe:
      GetCurrentCode().Compare(OP_EQUAL);
      GetCurrentCode().Not();
      break;
    case TokenKind::kCompareGt:
      GetCurrentCode().Compare(OP_GREATER_THAN);
      break;
    case TokenKind::kCompareGe:
      GetCurrentCode().Compare(OP_GREAT_OR_EQ);
      break;
    case TokenKind::kCompareLt:
      GetCurrentCode().Compare(OP_LESS_THAN);
      break;
    case TokenKind::kCompareLe:
      GetCurrentCode().Compare(OP_LESS_OR_EQ);
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
      GetCurrentCode().StoreLocal(*var_id);
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

  auto it = func_ids_.find(fn_name.value);
  if (it == func_ids_.end()) {
    functions_.at(function_decl_stack_.top())
        .unresolved_calls[GetCurrentCode().offset()] = fn_name.value;
    GetCurrentCode().Call(UINT32_MAX, argc);
    return true;
  }

  GetCurrentCode().Call(it->second, argc);
  return true;
}

Assembler& Compiler::GetCurrentCode() {
  int fn_id = function_decl_stack_.top();
  return functions_.at(fn_id).code;
}

#pragma pack(push, 1)
typedef struct vm_prog_header_t {
  uint32_t version;
  char magic[4];
  uint16_t function_count;
  uint16_t constant_count;
  uint32_t bytecode_size;
} vm_prog_header_t;

typedef struct vm_prog_function_t {
  uint16_t argument_count;
  uint16_t local_count;
  uint8_t bytecode[];
} vm_prog_function_t;

typedef struct vm_section_t {
  enum : uint8_t { CONST_STR, FUNCTION } type;
  uint32_t size;
  union {
    vm_prog_function_t fn;
    char str[];
  } as;
} vm_section_t;
#pragma pack(pop)

std::vector<uint8_t> Compiler::GenerateImage() const {
  vm_prog_header_t header = {
      .version = 0,
      .function_count = static_cast<uint16_t>(functions_.size()),
      .constant_count = static_cast<uint16_t>(const_ids_.size())};
  memcpy(&header.magic, "INK!", 4);

  size_t offset = sizeof(vm_prog_header_t);
  std::vector<uint8_t> program_image(offset);

  size_t bytecode_size = 0;
  for (Function fn : functions_) {
    std::vector<uint8_t> fn_bytecode = fn.code.Build();

    for (const auto& [offset, fn] : fn.unresolved_calls) {
      assert(fn_bytecode[offset] == OP_CALL && "expceted OP_CALL");

      auto it = func_ids_.find(fn);
      assert(it != func_ids_.end() && "unknown function");
      fn_bytecode[offset + 1] = uint8_t(it->second & 0xFF);
      fn_bytecode[offset + 2] = uint8_t((it->second >> 8) & 0xFF);
      fn_bytecode[offset + 3] = uint8_t((it->second >> 16) & 0xFF);
      fn_bytecode[offset + 4] = uint8_t((it->second >> 24) & 0xFF);
    }

    program_image.resize(offset + sizeof(vm_section_t) + fn_bytecode.size());

    vm_section_t section = {
        .type = vm_section_t::FUNCTION,
        .size = static_cast<uint32_t>(fn_bytecode.size()),
        .as.fn = {.argument_count = fn.argc, .local_count = fn.next_id}};

    memcpy(program_image.data() + offset, &section, sizeof(vm_section_t));
    memcpy(program_image.data() + offset + sizeof(vm_section_t),
           fn_bytecode.data(), fn_bytecode.size());
    bytecode_size += fn_bytecode.size();
    offset = program_image.size();
  }

  header.bytecode_size = bytecode_size;
  memcpy(program_image.data(), &header, sizeof(vm_prog_header_t));

  std::vector<std::string_view> constants_by_id(const_ids_.size());
  for (const auto& [value, id] : const_ids_) {
    constants_by_id[id] = value;
  }

  for (size_t id = 0; id < constants_by_id.size(); ++id) {
    const auto& value = constants_by_id[id];
    program_image.resize(offset + sizeof(vm_section_t) + value.size());

    vm_section_t section = {.type = vm_section_t::CONST_STR,
                            .size = static_cast<uint16_t>(value.size())};
    memcpy(program_image.data() + offset, &section, sizeof(vm_section_t));
    memcpy(program_image.data() + offset + sizeof(vm_section_t), value.data(),
           value.size());
    offset = program_image.size();
  }

  return program_image;
}