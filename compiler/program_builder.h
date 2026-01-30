#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stack>
#include <string>
#include <vector>

#include "compiler/assembler.h"

class ProgramBuilder {
 public:
  explicit ProgramBuilder();

  void EnterFunctionScope(const std::string& name,
                          std::vector<std::string> arguments);
  void ExitFunctionScope();

  enum class CreateIfMissing { No, Yes };
  std::optional<uint32_t> GetIdFor(const std::string& name,
                                   CreateIfMissing create);
  uint32_t GetIdForConstant(const std::string& value);

  Assembler& GetCurrentCode();
  void CallFunction(const std::string& name, size_t argc);
  std::vector<uint8_t> GenerateImage() const;

 private:
  struct Function {
    std::string name;
    std::map<std::string, int> var_to_id;
    uint16_t next_id{0};
    uint16_t argc{0};
    Assembler code;
    std::map<size_t, std::string> unresolved_calls;
  };
  std::vector<Function> functions_;
  std::map<std::string, int> func_ids_;
  int next_func_id_{0};
  std::stack<int> function_decl_stack_;

  std::map<std::string, int> const_ids_;
  int next_const_id_{0};

  Function& GetCurrentScope() {
    int fn_id = function_decl_stack_.top();
    return functions_.at(fn_id);
  }
};
