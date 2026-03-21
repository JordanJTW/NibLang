#include <cassert>
#include <cstdio>
#include <fstream>

#include "compiler/assembler.h"
#include "compiler/logging.h"
#include "compiler/parser.h"
#include "compiler/printer.h"
#include "compiler/program_builder.h"
#include "compiler/semantic_analyzer.h"
#include "compiler/tokenizer.h"
#include "compiler/type_context.h"
#include "src/prog_types.h"
#include "src/vm.h"

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

void emit_op(const Token& op, bool is_string, ProgramBuilder& builder) {
  switch (op.kind) {
    case TokenKind::kPlus:
      if (is_string)
        builder.GetCurrentCode().Concat();
      else
        builder.GetCurrentCode().Add();
      break;
    case TokenKind::kMinus:
      builder.GetCurrentCode().Subtract();
      break;
    case TokenKind::kMultiply:
      builder.GetCurrentCode().Multiply();
      break;
    case TokenKind::kDivide:
      builder.GetCurrentCode().Divide();
      break;
    case TokenKind::kCompareEq:
      builder.GetCurrentCode().Compare(OP_EQUAL);
      break;
    case TokenKind::kCompareNe:
      builder.GetCurrentCode().Compare(OP_EQUAL);
      builder.GetCurrentCode().Not();
      break;
    case TokenKind::kCompareGt:
      builder.GetCurrentCode().Compare(OP_GREATER_THAN);
      break;
    case TokenKind::kCompareGe:
      builder.GetCurrentCode().Compare(OP_GREAT_OR_EQ);
      break;
    case TokenKind::kCompareLt:
      builder.GetCurrentCode().Compare(OP_LESS_THAN);
      break;
    case TokenKind::kCompareLe:
      builder.GetCurrentCode().Compare(OP_LESS_OR_EQ);
      break;
    default:
      LOG(ERROR) << "unsupported binary operator: " << op.kind;
      return;
  }
}

struct LoopContext {
  std::string break_label;
  std::string continue_label;
};

void compile(const Block& root,
             ProgramBuilder& builder,
             TypeContext& type_context,
             std::optional<LoopContext> loop_ctx = std::nullopt);

void compile_expr(const std::unique_ptr<Expression>& expr,
                  ProgramBuilder& builder,
                  TypeContext& type_context,
                  bool wants_result = true);

static size_t kUniqueBlockId = 0;

void compile_call(const CallExpression& call,
                  ProgramBuilder& builder,
                  TypeContext& type_context) {
  if (call.resolved) {
    switch (call.resolved->kind) {
      case Free:
      case Extern:
      case StaticMethod:
        for (const auto& arg : call.arguments)
          compile_expr(arg, builder, type_context);
        builder.GetCurrentCode().Call(call.resolved->function_idx,
                                      call.arguments.size());

        break;
      case Anonymous:
        for (const auto& arg : call.arguments)
          compile_expr(arg, builder, type_context);
        compile_expr(call.callee, builder, type_context);
        builder.GetCurrentCode().CallDynamic(call.arguments.size());
        break;
      case Method:
        if (const auto* const member_access =
                std::get_if<MemberAccessExpression>(&call.callee->as)) {
          compile_expr(member_access->object, builder, type_context);
          for (const auto& arg : call.arguments)
            compile_expr(arg, builder, type_context);

          builder.GetCurrentCode().Call(call.resolved->function_idx,
                                        call.arguments.size() + 1);
        }
        break;
      case Constructor:
        for (const auto& arg : call.arguments)
          compile_expr(arg, builder, type_context);
        builder.GetCurrentCode().Call(VM_BUILTIN_ARRAY_INIT,
                                      call.arguments.size());
        break;
    }
  }
}

void compile_fn(const FunctionDeclaration& fn,
                ProgramBuilder& builder,
                TypeContext& type_context,
                std::optional<std::string> override_name = std::nullopt) {
  builder.EnterFunctionScope(
      override_name.value_or(fn.name), fn.resolved->function_symbol.idx.value(),
      fn.resolved->arguments, fn.resolved->capture_arguments);
  compile(*fn.body, builder, type_context);

  const auto& type = type_context.GetTypeInfo<FunctionType>(
      fn.resolved->function_symbol.type_id);

  // Insert an return; to unwind the stack
  if (type->return_type == TypeContext::LiteralType::Void)
    builder.GetCurrentCode().Return();
  builder.ExitFunctionScope();
}

void compile_expr(const std::unique_ptr<Expression>& expr,
                  ProgramBuilder& builder,
                  TypeContext& type_context,
                  bool wants_result) {
  if (expr == nullptr) {
    LOG(ERROR) << "Failed to compile null expression";
    return;
  }

  // builder.GetCurrentCode().DebugString(
  //     std::to_string(expr->meta.line_range.start) + " -> " +
  //     std::to_string(expr->meta.line_range.end));

  std::visit(
      Overloaded{
          [&](const PrimaryExpression& primary) {
            std::visit(
                Overloaded{
                    [&](const StringLiteral& str) {
                      builder.GetCurrentCode().PushConstRef(
                          builder.GetIdForConstant(str.value));
                    },
                    [&](const Identifier& ident) {
                      CHECK(ident.resolved)
                          << "Unresolved identifier: " << ident.name;

                      switch (ident.resolved->symbol.kind) {
                        case Symbol::Function:
                          CHECK(ident.resolved->symbol.idx.has_value())
                              << "Function symbol missing index: "
                              << ident.name;
                          builder.GetCurrentCode().Bind(
                              ident.resolved->symbol.idx.value(),
                              /*argc=*/0);
                          break;
                        case Symbol::Variable:
                        case Symbol::Capture: {
                          builder.PushSymbol(ident.resolved->symbol);
                          break;
                        }
                        default:
                          LOG(ERROR) << "Unsupported identifier kind: "
                                     << ident.resolved->symbol.kind;
                          break;
                      }
                    },
                    [&](int32_t i32) {
                      builder.GetCurrentCode().PushInt32(i32);
                    },
                    [&](float f32) { builder.GetCurrentCode().PushFloat(f32); },
                    [&](bool b) { builder.GetCurrentCode().PushBool(b); },
                    [&](Nil) { builder.GetCurrentCode().PushNil(); }},
                primary.value);
          },
          [&](const BinaryExpression& binary) {
            CHECK(binary.resolved) << "Unresolved binary expression";

            if (binary.resolved->specialization ==
                ResolvedBinary::Specialization::Nil) {
              const std::unique_ptr<Expression>& target =
                  binary.lhs->type == TypeContext::LiteralType::Nil
                      ? binary.rhs
                      : binary.lhs;
              compile_expr(target, builder, type_context);
              switch (binary.op) {
                case TokenKind::kCompareEq:
                  builder.GetCurrentCode().Is(vm_value_t::VALUE_TYPE_NULL);
                  break;
                case TokenKind::kCompareNe:
                  builder.GetCurrentCode().Is(vm_value_t::VALUE_TYPE_NULL);
                  builder.GetCurrentCode().Not();
                  break;
                default:
                  NOTREACHED()
                      << "only equality comparisons allowed against Nil";
                  break;
              }
            } else {
              compile_expr(binary.lhs, builder, type_context);
              compile_expr(binary.rhs, builder, type_context);
              emit_op(Token{.kind = binary.op},
                      binary.resolved->specialization ==
                          ResolvedBinary::Specialization::String,
                      builder);
            }
          },
          [&](const CallExpression& call) {
            compile_call(call, builder, type_context);
          },
          [&](const AssignmentExpression& assign) {
            if (std::holds_alternative<PrimaryExpression>(assign.lhs->as) &&
                std::holds_alternative<Identifier>(
                    std::get<PrimaryExpression>(assign.lhs->as).value)) {
              compile_expr(assign.rhs, builder, type_context);
              builder.GetCurrentCode().StackDup();

              const auto& ident = std::get<Identifier>(
                  std::get<PrimaryExpression>(assign.lhs->as).value);
              const auto& resolved = ident.resolved;
              CHECK(resolved) << "Unresolved identifier on LHS of assignment: "
                              << ident.name;
              builder.StoreSymbol(resolved->symbol);
            } else if (std::holds_alternative<ArrayAccessExpression>(
                           assign.lhs->as)) {
              const auto& array_access =
                  std::get<ArrayAccessExpression>(assign.lhs->as);
              compile_expr(array_access.array, builder, type_context);
              compile_expr(array_access.index, builder, type_context);
              compile_expr(assign.rhs, builder, type_context);

              builder.GetCurrentCode().Call(VM_BUILTIN_ARRAY_SET, 3);
            } else if (auto* const member_access =
                           std::get_if<MemberAccessExpression>(
                               &assign.lhs->as)) {
              if (!member_access->resolved) {
                LOG(ERROR) << "Member was never resolved "
                              "during type checking";
                return;
              }

              compile_expr(member_access->object, builder, type_context);
              builder.GetCurrentCode().StackDup();
              builder.GetCurrentCode().PushInt32(
                  member_access->resolved->index);
              compile_expr(assign.rhs, builder, type_context);
              builder.GetCurrentCode().Call(VM_BUILTIN_ARRAY_SET, 3);
            } else {
              LOG(ERROR) << "unsupported LHS in assignment expression";
            }
          },
          [&](const MemberAccessExpression& member_access) {
            if (const auto& resolved = member_access.resolved) {
              compile_expr(member_access.object, builder, type_context);
              builder.GetCurrentCode().PushInt32(resolved->index);
              builder.GetCurrentCode().Call(VM_BUILTIN_ARRAY_GET, 2);
            } else {
              LOG(FATAL) << "Unresolved member access: "
                         << member_access.member_name
                         << " at line: " << expr->meta.line_range.start;
            }
          },
          [&](const ArrayAccessExpression& array_access) {
            compile_expr(array_access.array, builder, type_context);
            compile_expr(array_access.index, builder, type_context);
            builder.GetCurrentCode().Call(VM_BUILTIN_ARRAY_GET, 2);
          },
          [&](const LogicExpression& logic) {
            compile_expr(logic.lhs, builder, type_context);

            static size_t id = 0;
            std::string lhs_label = "logic_lhs_" + std::to_string(id++);
            std::string end_label = "logic_end_" + std::to_string(id);

            // Handle short-circuiting to skip evaluating the RHS if possible.
            if (logic.kind == LogicExpression::AND) {
              builder.GetCurrentCode().JumpIfFalse(lhs_label);
            } else {
              builder.GetCurrentCode().JumpIfTrue(lhs_label);
            }

            compile_expr(logic.rhs, builder, type_context);
            builder.GetCurrentCode().Jump(end_label);

            // Short-circuiting consumes the LHS value on the stack, so we need
            // to push the correct value for the result of the logic expression.
            builder.GetCurrentCode().Label(lhs_label);
            if (logic.kind == LogicExpression::AND) {
              builder.GetCurrentCode().PushBool(false);
            } else {
              builder.GetCurrentCode().PushBool(true);
            }

            builder.GetCurrentCode().Label(end_label);
          },
          [&](const ClosureExpression& closure) {
            CHECK(closure.fn.resolved) << "Unresolved function!";
            compile_fn(closure.fn, builder, type_context);

            for (const auto& capture :
                 closure.fn.resolved->variables_to_capture) {
              builder.PushSymbol(capture);
            }
            builder.GetCurrentCode().Bind(
                closure.fn.resolved->function_symbol.idx.value(),
                /*argc=*/closure.fn.resolved->variables_to_capture.size());
          },
          [&](PrefixUnaryExpression& prefix) {
            compile_expr(prefix.operand, builder, type_context);
            switch (prefix.op) {
              case TokenKind::kPlus:  // no-op
                break;
              case TokenKind::kMinus:
                // TODO: Add OP_NEGATE?
                builder.GetCurrentCode().PushInt32(-1);
                builder.GetCurrentCode().Multiply();
                break;
              case TokenKind::kPlusPlus:
              case TokenKind::kMinusMinus:
                LOG(FATAL) << "Prefix ++/-- are not yet supported!";
                break;
              case TokenKind::kNot:
                builder.GetCurrentCode().Not();
                break;
              default:
                LOG(FATAL) << "Unsupported TokenKind op: " << prefix.op;
                break;
            }
          },
          [&](PostfixUnaryExpression& postfix) {
            LOG(FATAL) << "PostFix are not yet supported!";
          },
          [&](TypeCastExpression& cast) {
            compile_expr(cast.expr, builder, type_context);
          },
      },
      expr->as);

  if (!wants_result && expr->type != TypeContext::LiteralType::Void) {
    builder.GetCurrentCode().StackDel();
  }
}

void compile(const Block& root,
             ProgramBuilder& builder,
             TypeContext& type_context,
             std::optional<LoopContext> loop_ctx) {
  for (const auto& stmt : root.statements) {
    std::visit(
        Overloaded{
            [&](const std::unique_ptr<Expression>& expr) {
              compile_expr(expr, builder, type_context, /*wants_result=*/false);
            },
            [&](const FunctionDeclaration& fn) {
              if (fn.body) {
                CHECK(fn.resolved->variables_to_capture.empty())
                    << "Captures found for function.";

                compile_fn(fn, builder, type_context);
              }
            },
            [&](const ReturnStatement& ret) {
              compile_expr(ret.value, builder, type_context);
              builder.GetCurrentCode().Return();
            },
            [&](const ThrowStatement& thr) {
              compile_expr(thr.value, builder, type_context);
              builder.GetCurrentCode().Throw();
            },
            [&](const IfStatement& if_stmt) {
              compile_expr(if_stmt.condition, builder, type_context);
              std::string id = std::to_string(kUniqueBlockId++);
              builder.GetCurrentCode().JumpIfFalse("else" + id);
              compile(if_stmt.then_body, builder, type_context, loop_ctx);
              builder.GetCurrentCode().Jump("end_if" + id);
              builder.GetCurrentCode().Label("else" + id);
              compile(if_stmt.else_body, builder, type_context, loop_ctx);
              builder.GetCurrentCode().Label("end_if" + id);
            },
            [&](const WhileStatement& while_stmt) {
              std::string id = std::to_string(kUniqueBlockId++);
              std::string condition_label = "while_cond" + id;
              std::string end_label = "while_end" + id;

              builder.GetCurrentCode().Label(condition_label);
              compile_expr(while_stmt.condition, builder, type_context);
              builder.GetCurrentCode().JumpIfFalse(end_label);
              compile(while_stmt.body, builder, type_context,
                      LoopContext{end_label, condition_label});
              builder.GetCurrentCode().Jump(condition_label);
              builder.GetCurrentCode().Label(end_label);
            },
            [&](const BreakStatement&) {
              if (!loop_ctx) {
                LOG(ERROR) << "break statement not within a loop";
                return;
              }
              builder.GetCurrentCode().Jump(loop_ctx->break_label);
            },
            [&](const ContinueStatement&) {
              if (!loop_ctx) {
                LOG(ERROR) << "continue statement not within a loop";
                return;
              }
              builder.GetCurrentCode().Jump(loop_ctx->continue_label);
            },
            [&](const AssignStatement& assign) {
              compile_expr(assign.value, builder, type_context);
              CHECK(assign.resolved)
                  << "Unresolved variable in assignment: " << assign.name;

              builder.StoreSymbol(assign.resolved->symbol);
            },
            [&](const StructDeclaration& struct_decl) {
              for (const auto& method : struct_decl.methods) {
                if (!method.second.body)
                  continue;

                CHECK(method.second.resolved)
                    << "Unresolved method: " << method.first;
                CHECK(method.second.resolved->variables_to_capture.empty())
                    << "Captures found for method: " << method.first;

                compile_fn(method.second, builder, type_context,
                           struct_decl.name + "_" + method.first);
              }
            }},
        stmt->as);
  }
}

int DumpImage(const uint8_t* program, size_t program_size) {
  if (sizeof(vm_prog_header_t) > program_size) {
    LOG(ERROR) << "Program too small to contain header";
    return -1;
  }

  vm_prog_header_t header;
  memcpy(&header, program, sizeof(vm_prog_header_t));

  if (header.magic[0] != 'I' || header.magic[1] != 'N' ||
      header.magic[2] != 'K' || header.magic[3] != '!') {
    LOG(ERROR) << "Invalid magic";
    return -1;
  }

  LOG(INFO) << "Program version: " << header.version;
  LOG(INFO) << "Constants: " << header.constant_count;
  LOG(INFO) << "Functions: " << header.function_count;
  LOG(INFO) << "Bytecode Size: " << header.bytecode_size;
  LOG(INFO) << "Debug Size: " << header.debug_size;

  size_t offset = sizeof(vm_prog_header_t);

  size_t parsed_constants = 0;
  size_t parsed_functions = 0;
  std::vector<uint8_t> debug_data;

  struct FunctionInfo {
    uint32_t arg_count;
    uint32_t local_count;
    uint32_t name_offset;
    std::vector<uint8_t> bytecode;
  };
  std::vector<FunctionInfo> functions;

  while (offset + sizeof(vm_section_t) < program_size) {
    vm_section_t section;
    memcpy(&section, program + offset, sizeof(vm_section_t));
    offset += sizeof(vm_section_t);

    if (offset + section.size > program_size) {
      LOG(ERROR) << "Section size exceeds program size";
      return -1;
    }

    switch (section.type) {
      case vm_section_t::CONST_STR: {
        const char* str = reinterpret_cast<const char*>(program + offset);
        LOG(INFO) << "Constant " << parsed_constants++ << ": \""
                  << std::string(str, section.size) << "\"";
        break;
      }

      case vm_section_t::FUNCTION: {
        FunctionInfo fn;
        fn.arg_count = section.as.fn.argument_count;
        fn.local_count = section.as.fn.local_count;
        fn.name_offset = section.as.fn.name_offset;
        fn.bytecode.assign(program + offset, program + offset + section.size);
        functions.push_back(std::move(fn));
        parsed_functions++;
        break;
      }

      case vm_section_t::DEBUG: {
        debug_data.assign(program + offset, program + offset + section.size);
        break;
      }

      default:
        LOG(ERROR) << "Unknown section type: "
                   << static_cast<int>(section.type);
        return -1;
    }
    offset += section.size;
  }

  for (size_t i = 0; i < functions.size(); i++) {
    const auto& fn = functions[i];

    const char* name = "<unknown>";

    if (!debug_data.empty() && fn.name_offset < debug_data.size()) {
      name = reinterpret_cast<const char*>(debug_data.data() + fn.name_offset);
    }

    LOG(INFO) << "Function " << i << ": " << name << " (args: " << fn.arg_count
              << ", locals: " << fn.local_count
              << ", bytecode size: " << fn.bytecode.size() << ")";
    DumpByteCode(fn.bytecode);
    printf("\n");
  }

  if (parsed_constants != header.constant_count ||
      parsed_functions != header.function_count) {
    LOG(WARNING) << "counts mismatch";
    LOG(WARNING) << "Constants: " << parsed_constants << " / "
                 << header.constant_count;
    LOG(WARNING) << "Functions: " << parsed_functions << " / "
                 << header.function_count;
  }
  return 0;
}

enum class OutputMode { Ast, DumpImage, Image };

struct Options {
  OutputMode mode = OutputMode::Image;
  std::string input_path;
  std::string output_path;
};

Options ParseArgs(int argc, char* argv[]) {
  Options opts;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "--ast") {
      opts.mode = OutputMode::Ast;
    } else if (arg == "--dump") {
      opts.mode = OutputMode::DumpImage;
    } else if (arg == "--output" && i + 1 < argc) {
      opts.output_path = argv[++i];
    } else {
      opts.input_path = arg;
    }
  }

  if (opts.input_path.empty()) {
    fprintf(stderr, "Usage: %s [--ast|--dump] <file>\n", argv[0]);
    exit(1);
  }

  return opts;
}

int main(int argc, char* argv[]) {
  Options opts = ParseArgs(argc, argv);

  std::ifstream file(opts.input_path);
  if (!file) {
    fprintf(stderr, "Error opening: %s\n", opts.input_path.c_str());
    return 1;
  }

  std::string contents{std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>()};

  ProgramBuilder builder;
  Parser parser(contents);

  Block root = parser.Parse();

  auto error_collector = DefaultErrorCollector(contents);

  TypeContext type_context(*error_collector);
  SemanticAnalyzer analyzer(type_context, *error_collector);
  analyzer.Check(root);

  if (error_collector->HasErrors()) {
    error_collector->PrintAllErrors();
    return 1;
  }

  if (opts.mode == OutputMode::Ast) {
    Printer(&type_context).Print(root);
    return 0;
  }

  compile(root, builder, type_context);
  builder.GetCurrentCode().Return();

  std::vector<uint8_t> program_image = builder.GenerateImage();

  if (opts.mode == OutputMode::DumpImage) {
    return DumpImage(program_image.data(), program_image.size());
  }

  if (opts.output_path.empty()) {
    std::cout.write(reinterpret_cast<const char*>(program_image.data()),
                    program_image.size());
    std::cout.flush();
  } else {
    std::ofstream out(opts.output_path, std::ios::binary);
    if (out.is_open()) {
      out.write(reinterpret_cast<const char*>(program_image.data()),
                program_image.size());
      out.close();
    }
  }
  return 0;
}
