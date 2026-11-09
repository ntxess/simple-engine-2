// Copyright 2026 The simple-engine-2 Authors.
//
// The scene files' JSON model: parsing with positions in errors, numbers
// kept as written, writing, and RFC 7386 merge patches (the RFC's own
// examples).

#include "game_rampage/json.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>

namespace se::rampage::json {
namespace {

std::string Canonical(std::string_view text) {
  auto value = Parse(text);
  REQUIRE(value.has_value());
  return Write(*value);
}

std::string Merge(std::string_view target, std::string_view patch) {
  auto t = Parse(target);
  auto p = Parse(patch);
  REQUIRE(t.has_value());
  REQUIRE(p.has_value());
  MergePatch(*t, *p);
  return Write(*t);
}

TEST_CASE("JSON parses and writes back", "[rampage][json]") {
  REQUIRE(Canonical(R"( {"a": [1, 2.50, -0, 1e-7, true, null],
                       "b": {"c": "x\"\\/\u00e9\ud83d\ude00"}} )") ==
          "{\"a\":[1,2.50,-0,1e-7,true,null],\"b\":{\"c\":\"x\\\"\\\\/"
          "\xc3\xa9\xf0\x9f\x98\x80\"}}");
  REQUIRE(Canonical("[]") == "[]");
  REQUIRE(Canonical("{}") == "{}");
  REQUIRE(Canonical("\"\\n\\t\"") == "\"\\n\\t\"");
  const auto number = Parse("302.083344");
  REQUIRE(number.has_value());
  REQUIRE(number->Text() == "302.083344");
  CHECK(number->AsDouble() == 302.083344);
  CHECK_FALSE(number->AsInt64().has_value());
  REQUIRE(Parse("-42")->AsInt64() == -42);
  REQUIRE(Write(*Parse(R"({"a":[1,{"b":2}]})"), 2) ==
          "{\n  \"a\": [\n    1,\n    {\n      \"b\": 2\n    }\n  ]\n}");
}

TEST_CASE("JSON errors carry the line and column", "[rampage][json]") {
  const auto bad = Parse("{\n  \"a\": [1, 2,]\n}");
  REQUIRE_FALSE(bad.has_value());
  REQUIRE(bad.error().line == 2);
  REQUIRE(bad.error().column == 14);
  CHECK_FALSE(Parse(R"({"a": 1, "a": 2})").has_value());
  CHECK_FALSE(Parse("[1] 2").has_value());
  CHECK_FALSE(Parse("01").has_value());
  CHECK_FALSE(Parse("\"\\ud800\"").has_value());
  CHECK_FALSE(Parse("nul").has_value());
  CHECK_FALSE(Parse("").has_value());
  CHECK_FALSE(Parse(std::string(300, '[')).has_value());
}

TEST_CASE("RFC 7386 merge patches", "[rampage][json]") {
  // The examples of RFC 7386, section 3.
  REQUIRE(Merge(R"({"a":"b"})", R"({"a":"c"})") == R"({"a":"c"})");
  REQUIRE(Merge(R"({"a":"b"})", R"({"b":"c"})") == R"({"a":"b","b":"c"})");
  REQUIRE(Merge(R"({"a":"b"})", R"({"a":null})") == R"({})");
  REQUIRE(Merge(R"({"a":"b","b":"c"})", R"({"a":null})") == R"({"b":"c"})");
  REQUIRE(Merge(R"({"a":["b"]})", R"({"a":"c"})") == R"({"a":"c"})");
  REQUIRE(Merge(R"({"a":"c"})", R"({"a":["b"]})") == R"({"a":["b"]})");
  REQUIRE(Merge(R"({"a":{"b":"c"}})", R"({"a":{"b":"d","c":null}})") ==
          R"({"a":{"b":"d"}})");
  REQUIRE(Merge(R"({"a":[{"b":"c"}]})", R"({"a":[1]})") == R"({"a":[1]})");
  REQUIRE(Merge(R"(["a","b"])", R"(["c","d"])") == R"(["c","d"])");
  REQUIRE(Merge(R"({"a":"b"})", R"(["c"])") == R"(["c"])");
  REQUIRE(Merge(R"({"a":"foo"})", "null") == "null");
  REQUIRE(Merge(R"({"a":"foo"})", R"("bar")") == R"("bar")");
  REQUIRE(Merge(R"({"e":null})", R"({"a":1})") == R"({"e":null,"a":1})");
  REQUIRE(Merge(R"([1,2])", R"({"a":"b","c":null})") == R"({"a":"b"})");
  REQUIRE(Merge(R"({})", R"({"a":{"bb":{"ccc":null}}})") ==
          R"({"a":{"bb":{}}})");
}

}  // namespace
}  // namespace se::rampage::json
