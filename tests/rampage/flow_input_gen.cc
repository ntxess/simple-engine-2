// Copyright 2026 The simple-engine-2 Authors.
//
// Writes the flow's recorded session, tests/fixtures/flow/flow.seinput
// (M13b's determinism_rampage_flow): the scripted edges of
// rampage/support/flow_session.cc latched by StampedInput at 120 Hz.
//
//   rampage_flow_input_gen OUT_FILE
//
// rampage.flow_input_fixture runs it and compares the result with the
// checked-in file, so the fixture always matches its generator.

#include <cstdio>
#include <exception>
#include <fstream>
#include <string>

#include "rampage/support/flow_session.h"

namespace {

int Run(int argc, char** argv) {
  if (argc != 2) {
    std::fputs("usage: rampage_flow_input_gen OUT_FILE\n", stderr);
    return 2;
  }
  const std::string text = se::testing::WriteSeinput(
      se::testing::FlowScriptFrames(se::testing::kFlowScriptTicks));
  std::ofstream out(argv[1], std::ios::binary);
  out << text;
  if (!out) {
    std::fprintf(stderr, "rampage_flow_input_gen: cannot write %s\n", argv[1]);
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // A tool may catch (ADR-006 exempts test and tool code).
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "rampage_flow_input_gen: %s\n", error.what());
    return 1;
  }
}
