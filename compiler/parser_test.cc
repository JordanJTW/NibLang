// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "compiler/parser.h"

#include "compiler/types.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::ExplainMatchResult;
using ::testing::Field;
using ::testing::Optional;
using ::testing::Pair;
using ::testing::Pointee;
using ::testing::ResultOf;

namespace {

MATCHER_P(IsFunctionThat, matcher, "") {
  const auto* fn = std::get_if<FunctionDeclaration>(&arg.as);
  if (!fn) {
    *result_listener << "is not an FunctionDeclaration";
    return false;
  }

  return ExplainMatchResult(matcher, *fn, result_listener);
}

MATCHER_P(IsNamedType, name_matcher, "") {
  const auto* name = std::get_if<std::string>(&arg.type);
  if (!name) {
    *result_listener << "is not a simple named type string";
    return false;
  }
  return ExplainMatchResult(name_matcher, *name, result_listener);
}

auto HasName(const std::string& name) {
  return Field(&FunctionDeclaration::name, Field(&SpannedText::text, Eq(name)));
}

template <typename... Args>
auto HasArgs(const Args&... matchers) {
  return Field(&FunctionDeclaration::arguments, ElementsAre(matchers...));
}

auto HasReturnType(const ::testing::Matcher<ParsedType>& m) {
  return Field(&FunctionDeclaration::return_type, m);
}

auto WithBody(const ::testing::Matcher<Block>& m) {
  return Field(&FunctionDeclaration::body, Pointee(m));
}

class ParserWrapper {
 public:
  ParserWrapper(std::string_view source_text)
      : source_text_(source_text.data()),
        parser_(source_text_, error_collector, /*file_id=*/0) {}

  Block Parse() { return parser_.Parse(); }

  std::string TextAt(const Metadata& meta) {
    return source_text_.substr(meta.column_range.start,
                               meta.column_range.end - meta.column_range.start);
  }

 protected:
  ErrorCollector error_collector;

 private:
  const std::string source_text_;
  Parser parser_;
};

auto IsSpannedText(const std::string& text) {
  return Field(&SpannedText::text, Eq(text));
}

TEST(ParserTest, Parse_FunctionSignature) {
  auto parser = ParserWrapper("fn foo(bar: i32) -> bool { return True; }");

  auto block = parser.Parse();
  ASSERT_EQ(block.statements.size(), 1u);

  auto* stmt = std::get_if<FunctionDeclaration>(&block.statements[0]->as);
  ASSERT_TRUE(stmt);

  EXPECT_THAT(stmt->name, IsSpannedText("foo"));
  EXPECT_THAT(stmt->arguments,
              ElementsAre(Pair(IsSpannedText("bar"), IsNamedType("i32"))));
  EXPECT_THAT(stmt->return_type, IsNamedType("bool"));
}

TEST(ParserTest, Metadata_FunctionSignature) {
  auto parser = ParserWrapper("fn foo(bar: i32) -> bool { return True; }");

  auto block = parser.Parse();
  ASSERT_EQ(block.statements.size(), 1u);

  auto* stmt = std::get_if<FunctionDeclaration>(&block.statements[0]->as);
  ASSERT_TRUE(stmt);

  EXPECT_THAT(parser.TextAt(block.statements[0]->meta),
              Eq("fn foo(bar: i32) -> bool { return True; }"));

  auto TextAt = [&parser](const Metadata& m) { return parser.TextAt(m); };

  EXPECT_THAT(stmt->arguments,
              ElementsAre(Pair(
                  Field(&SpannedText::metadata, ResultOf(TextAt, Eq("bar"))),
                  Field(&ParsedType::metadata, ResultOf(TextAt, Eq("i32"))))));

  EXPECT_THAT(parser.TextAt(stmt->return_type.metadata), Eq("bool"));
}

MATCHER_P3(IsStructFieldMetadata, expected_name, expected_type, parser, "") {
  std::string source_name = parser->TextAt(arg.first.metadata);
  if (source_name != expected_name) {
    *result_listener << "got field name \"" << source_name << "\"";
    return false;
  }

  std::string source_type = parser->TextAt(arg.second.metadata);
  if (source_type != expected_type) {
    *result_listener << "got source type text \"" << source_type << "\"";
    return false;
  }

  return true;
}

TEST(ParserTest, Metadata_StructFields) {
  static constexpr std::string_view kText = "struct Point { x: i32; y: i32; }";
  auto parser = ParserWrapper(kText);

  auto block = parser.Parse();
  ASSERT_EQ(block.statements.size(), 1u);

  auto* stmt = std::get_if<StructDeclaration>(&block.statements[0]->as);
  ASSERT_TRUE(stmt);

  EXPECT_THAT(parser.TextAt(block.statements[0]->meta), Eq(kText));

  EXPECT_THAT(stmt->fields,
              ElementsAre(IsStructFieldMetadata("x", "i32", &parser),
                          IsStructFieldMetadata("y", "i32", &parser)));
}

TEST(ParserTest, Metadata_AssignStatement) {
  static constexpr std::string_view kText = "let x: i32 = 10 + 2;";
  auto parser = ParserWrapper(kText);

  auto block = parser.Parse();
  ASSERT_EQ(block.statements.size(), 1u);

  auto* stmt = std::get_if<AssignStatement>(&block.statements[0]->as);
  ASSERT_TRUE(stmt);

  EXPECT_THAT(parser.TextAt(block.statements[0]->meta), Eq(kText));
  EXPECT_THAT(parser.TextAt(stmt->type->metadata), Eq("i32"));
  EXPECT_THAT(parser.TextAt(stmt->value->meta), Eq("10 + 2"));
}

TEST(ParserTest, Metadata_OptionalParsedType) {
  static constexpr std::string_view kText = "alias Foo = String?;";
  auto parser = ParserWrapper(kText);

  auto block = parser.Parse();
  ASSERT_EQ(block.statements.size(), 1u);

  auto* stmt = std::get_if<TypeAliasStatement>(&block.statements[0]->as);
  ASSERT_TRUE(stmt);

  EXPECT_THAT(parser.TextAt(block.statements[0]->meta), Eq(kText));

  EXPECT_THAT(parser.TextAt(stmt->type->metadata), "String?");

  auto* optional_type = std::get_if<ParsedOptionalType>(&stmt->type->type);
  ASSERT_TRUE(optional_type);

  auto TextAt = [&parser](const Metadata& m) { return parser.TextAt(m); };

  EXPECT_THAT(
      optional_type->wrapped_type,
      Pointee(Field(&ParsedType::metadata, ResultOf(TextAt, "String"))));
}

TEST(ParserTest, Metadata_UnionParsedType) {
  static constexpr std::string_view kText = "alias Foo = String | i32 | bool;";
  auto parser = ParserWrapper(kText);

  auto block = parser.Parse();
  ASSERT_EQ(block.statements.size(), 1u);

  auto* stmt = std::get_if<TypeAliasStatement>(&block.statements[0]->as);
  ASSERT_TRUE(stmt);

  EXPECT_THAT(parser.TextAt(block.statements[0]->meta), Eq(kText));

  EXPECT_THAT(parser.TextAt(stmt->type->metadata), "String | i32 | bool");

  auto* union_type = std::get_if<ParsedUnionType>(&stmt->type->type);
  ASSERT_TRUE(union_type);

  auto TextAt = [&parser](const Metadata& m) { return parser.TextAt(m); };

  EXPECT_THAT(
      union_type->names,
      ElementsAre(Field(&ParsedType::metadata, ResultOf(TextAt, "String")),
                  Field(&ParsedType::metadata, ResultOf(TextAt, "i32")),
                  Field(&ParsedType::metadata, ResultOf(TextAt, "bool"))));
}

TEST(ParserTest, Metadata_FunctionParsedType) {
  static constexpr std::string_view kText =
      "alias Foo = fn (i32, String) -> bool;";
  auto parser = ParserWrapper(kText);

  auto block = parser.Parse();
  ASSERT_EQ(block.statements.size(), 1u);

  auto* stmt = std::get_if<TypeAliasStatement>(&block.statements[0]->as);
  ASSERT_TRUE(stmt);

  EXPECT_THAT(parser.TextAt(block.statements[0]->meta), Eq(kText));

  EXPECT_THAT(parser.TextAt(stmt->type->metadata), "fn (i32, String) -> bool");

  auto* fn_type = std::get_if<ParsedFunctionType>(&stmt->type->type);
  ASSERT_TRUE(fn_type);

  auto TextAt = [&parser](const Metadata& m) { return parser.TextAt(m); };

  EXPECT_THAT(
      fn_type->arguments,
      ElementsAre(Field(&ParsedType::metadata, ResultOf(TextAt, "i32")),
                  Field(&ParsedType::metadata, ResultOf(TextAt, "String"))));

  EXPECT_THAT(fn_type->return_value,
              Pointee(Field(&ParsedType::metadata, ResultOf(TextAt, "bool"))));
}

TEST(ParserTest, Metadata_ParameterizedParsedType) {
  static constexpr std::string_view kText = "alias Foo = Map[kEy, VaLuE];";
  auto parser = ParserWrapper(kText);

  auto block = parser.Parse();
  ASSERT_EQ(block.statements.size(), 1u);

  auto* stmt = std::get_if<TypeAliasStatement>(&block.statements[0]->as);
  ASSERT_TRUE(stmt);

  EXPECT_THAT(parser.TextAt(block.statements[0]->meta), Eq(kText));

  EXPECT_THAT(parser.TextAt(stmt->type->metadata), "Map[kEy, VaLuE]");

  auto* param_type = std::get_if<ParsedParameterizedType>(&stmt->type->type);
  ASSERT_TRUE(param_type);

  auto TextAt = [&parser](const Metadata& m) { return parser.TextAt(m); };

  EXPECT_THAT(
      param_type->parameters,
      ElementsAre(Field(&ParsedType::metadata, ResultOf(TextAt, "kEy")),
                  Field(&ParsedType::metadata, ResultOf(TextAt, "VaLuE"))));

  EXPECT_THAT(param_type->type,
              Pointee(Field(&ParsedType::metadata, ResultOf(TextAt, "Map"))));
}

}  // namespace