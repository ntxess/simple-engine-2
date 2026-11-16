// Copyright 2026 The simple-engine-2 Authors.
//
// The editor's test script (M10a): its parser.

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "se_editor/editor_script.h"

namespace se::editor {
namespace {

TEST_CASE("Scripts parse into actions with their lines", "[script]") {
  const auto actions = ParseScript(
      "# a comment\n"
      "\n"
      "play\r\n"
      "  wait-ticks 300  \n"
      "set 5a4db0c5-0000-4000-8000-000000000000 Transform "
      "{\"pos\": {\"x\": 1, \"y\": 2}}\n"
      "dialog-callback a.json;b.json from-thread\n"
      "quit save\n");
  REQUIRE(actions.has_value());
  REQUIRE(actions->size() == 5);
  REQUIRE((*actions)[0].verb == "play");
  REQUIRE((*actions)[0].line == 3);
  REQUIRE((*actions)[1].args == std::vector<std::string>{"300"});
  REQUIRE((*actions)[2].args.size() == 3);
  REQUIRE((*actions)[2].args[2] == "{\"pos\": {\"x\": 1, \"y\": 2}}");
  REQUIRE((*actions)[3].args ==
          std::vector<std::string>{"a.json;b.json", "from-thread"});
  REQUIRE((*actions)[4].args == std::vector<std::string>{"save"});
}

TEST_CASE("Unknown actions and wrong argument counts are errors", "[script]") {
  const auto unknown = ParseScript("play\nfly away\n");
  REQUIRE_FALSE(unknown.has_value());
  REQUIRE(unknown.error().line == 2);
  REQUIRE_FALSE(ParseScript("wait-ticks\n").has_value());
  REQUIRE_FALSE(ParseScript("undo now\n").has_value());
  REQUIRE(ParseScript("step\nstep 4\n").has_value());
  REQUIRE(ParseScript("key ctrl+z\n").has_value());
  REQUIRE_FALSE(ParseScript("key\n").has_value());
}

}  // namespace
}  // namespace se::editor
