// Copyright 2026 The simple-engine-2 Authors.
//
// se_replay: runs a game module headless and reports per-tick hashes.
//
//   se_replay --module kernel_test [--scene gameplay] [--ticks N]
//             [--inputs FILE] [--edits FILE] [--seed S]
//             [--fixed-frame-dt MS] [--jitter-ms MS]
//             [--hash-out FILE] [--projection-out FILE] [--record FILE]
//             [--det-crosscheck K] [--jobs W] [--expect-golden NAME]
//
// --inputs takes a replay written by --record (the module's session log
// comes with it, and the recorded hashes are checked tick by tick) or a
// legacy .seinput text file (lines "tick held pressed released" in hex,
// bit i = action i). Without --fixed-frame-dt each frame runs one tick;
// with it, a synthetic clock at 1000/MS Hz drives the FixedStepper. The
// rampage module arrives in M3. Exit status: 0 ok, 1 a check failed,
// 2 bad arguments or input.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <entt/entity/registry.hpp>
#include <exception>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/core_components.h"
#include "se_core/fp.h"
#include "se_core/guid.h"
#include "se_core/input_frame.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_sim/game_state.h"
#include "se_sim/replay.h"
#include "support/golden.h"
#include "support/kernel_session.h"
#include "support/kernel_test_module.h"
#include "support/test_roles.h"

namespace se {
namespace {

struct Args {
  std::string module = "kernel_test";
  std::string scene = "gameplay";
  std::optional<Tick> ticks;
  std::string inputs;
  std::string edits;
  std::uint64_t seed = 42;
  double frame_dt_ms = 0.0;
  double jitter_ms = 0.0;
  std::string hash_out;
  std::string projection_out;
  std::string record;
  std::uint32_t det_crosscheck = 0;
  int jobs = 0;
  std::string expect_golden;
};

void Usage() {
  std::fprintf(stderr,
               "usage: se_replay --module kernel_test|rampage [--scene NAME] "
               "[--ticks N]\n"
               "                 [--inputs FILE] [--edits FILE] [--seed S]\n"
               "                 [--fixed-frame-dt MS] [--jitter-ms MS]\n"
               "                 [--hash-out FILE] [--projection-out FILE] "
               "[--record FILE]\n"
               "                 [--det-crosscheck K] [--jobs W] "
               "[--expect-golden NAME]\n");
}

std::optional<Args> Parse(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (i + 1 >= argc) {
      std::fprintf(stderr, "se_replay: %s needs a value\n", argv[i]);
      return std::nullopt;
    }
    const std::string value = argv[++i];
    if (flag == "--module") {
      args.module = value;
    } else if (flag == "--scene") {
      args.scene = value;
    } else if (flag == "--ticks") {
      args.ticks = std::stoull(value);
    } else if (flag == "--inputs") {
      args.inputs = value;
    } else if (flag == "--edits") {
      args.edits = value;
    } else if (flag == "--seed") {
      args.seed = std::stoull(value);
    } else if (flag == "--fixed-frame-dt") {
      args.frame_dt_ms = std::stod(value);
    } else if (flag == "--jitter-ms") {
      args.jitter_ms = std::stod(value);
    } else if (flag == "--hash-out") {
      args.hash_out = value;
    } else if (flag == "--projection-out") {
      args.projection_out = value;
    } else if (flag == "--record") {
      args.record = value;
    } else if (flag == "--det-crosscheck") {
      args.det_crosscheck = static_cast<std::uint32_t>(std::stoul(value));
    } else if (flag == "--jobs") {
      args.jobs = std::stoi(value);
    } else if (flag == "--expect-golden") {
      args.expect_golden = value;
    } else {
      std::fprintf(stderr, "se_replay: unknown flag %s\n", argv[i - 1]);
      return std::nullopt;
    }
  }
  return args;
}

std::optional<std::vector<std::byte>> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  const std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(chars.size());
  for (std::size_t i = 0; i < chars.size(); ++i) {
    bytes[i] = static_cast<std::byte>(chars[i]);
  }
  return bytes;
}

bool WriteFile(const std::string& path, const std::vector<std::byte>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

// A legacy .seinput file as a replay: one InputFrame per line.
std::optional<std::vector<std::byte>> SeinputAsReplay(const std::string& path) {
  std::ifstream in(path);
  if (!in) return std::nullopt;
  ReplayRecorder recorder;
  recorder.Begin(MakeReplayHeader({}, 0, kDefaultTickRate), {});
  std::string line;
  std::optional<Tick> expected;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::uint64_t tick = 0;
    std::uint64_t held = 0;
    std::uint64_t pressed = 0;
    std::uint64_t released = 0;
    fields >> std::hex >> tick >> held >> pressed >> released;
    if (!fields || (expected && tick != *expected)) return std::nullopt;
    InputFrame frame;
    frame.tick = tick;
    frame.held = held;
    for (int a = 0; a < kMaxActions; ++a) {
      frame.pressed[a] = static_cast<std::uint8_t>((pressed >> a) & 1U);
      frame.released[a] = static_cast<std::uint8_t>((released >> a) & 1U);
    }
    recorder.Record(frame);
    expected = tick + 1;
  }
  recorder.RecordSession(SessionLog());
  return recorder.Finish();
}

int Run(const Args& args) SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner) {
  if (args.module == "rampage") {
    std::fprintf(stderr,
                 "se_replay: the rampage module is not available until M3\n");
    return 2;
  }
  if (args.module != "kernel_test") {
    std::fprintf(stderr, "se_replay: unknown module '%s'\n",
                 args.module.c_str());
    return 2;
  }
  if (args.scene != "gameplay") {
    std::fprintf(stderr, "se_replay: kernel_test has no scene '%s'\n",
                 args.scene.c_str());
    return 2;
  }
  if (args.jobs > 0) {
    std::fprintf(stderr,
                 "se_replay: --jobs %d accepted; the threaded pool arrives in "
                 "M14, so forks run inline\n",
                 args.jobs);
  }

  testing::KernelSessionOptions options;
  options.seed = args.seed;
  options.edits_path = args.edits;
  options.det_crosscheck = args.det_crosscheck;
  options.record = !args.record.empty();
  if (args.frame_dt_ms > 0.0) {
    options.hz = 1000.0 / args.frame_dt_ms;
    options.jitter_sec = args.jitter_ms / 1000.0;
  }

  std::optional<ReplayReader> reader;
  if (!args.inputs.empty()) {
    std::optional<std::vector<std::byte>> bytes =
        args.inputs.ends_with(".seinput") ? SeinputAsReplay(args.inputs)
                                          : ReadFile(args.inputs);
    if (!bytes) {
      std::fprintf(stderr, "se_replay: cannot read %s\n", args.inputs.c_str());
      return 2;
    }
    auto opened = ReplayReader::Open(*bytes);
    if (!opened) {
      std::fprintf(stderr, "se_replay: %s: %s\n", args.inputs.c_str(),
                   opened.error().message.c_str());
      return 2;
    }
    reader = std::move(*opened);
    if (!reader->MathMatches()) {
      std::fprintf(stderr,
                   "se_replay: warning: the replay's math self-test hash "
                   "differs from this build's\n");
    }
    options.replay = &*reader;
    options.seed =
        reader->Header().seed != 0 ? reader->Header().seed : args.seed;
  }
  options.ticks = args.ticks.value_or(
      reader ? static_cast<Tick>(reader->Frames().size()) : Tick{600});

  std::ofstream projection;
  if (!args.projection_out.empty()) {
    projection.open(args.projection_out, std::ios::trunc);
    options.on_tick = [&projection](Tick tick, const GameStateStack& states) {
      for (const World* world : states.RunningWorlds()) {
        for (const EntityDigest& digest : world->Digests()) {
          const entt::entity e = *world->Find(digest.guid);
          const auto* t = world->CReg().try_get<Transform>(e);
          if (t == nullptr) continue;
          projection << std::format("{} {} {:.9g} {:.9g} {:.9g}\n", tick,
                                    ToString(digest.guid), t->pos.x, t->pos.y,
                                    t->rot);
        }
      }
    };
  }

  const testing::KernelSessionResult result =
      testing::RunKernelSession(options);
  if (!result.error.empty()) {
    std::fprintf(stderr, "se_replay: %s\n", result.error.c_str());
    return 2;
  }

  int status = 0;
  if (!args.hash_out.empty()) {
    std::ofstream out(args.hash_out, std::ios::trunc);
    for (std::size_t i = 0; i < result.hashes.size(); ++i) {
      out << std::format("{} {:016x}\n", i + 1, result.hashes[i]);
    }
  }
  if (!args.record.empty() && !WriteFile(args.record, result.replay)) {
    std::fprintf(stderr, "se_replay: cannot write %s\n", args.record.c_str());
    status = 2;
  }
  if (reader && !reader->Hashes().empty()) {
    const auto recorded = reader->Hashes();
    const Tick first = reader->FirstHashTick();
    for (std::size_t i = 0; i < result.hashes.size(); ++i) {
      const Tick tick = i + 1;
      if (tick < first || tick - first >= recorded.size()) continue;
      if (recorded[tick - first] != result.hashes[i]) {
        std::fputs(std::format("se_replay: tick {} diverges from the "
                               "recording: {:016x}, recorded {:016x}\n",
                               tick, result.hashes[i], recorded[tick - first])
                       .c_str(),
                   stderr);
        status = 1;
        break;
      }
    }
  }
  if (args.det_crosscheck > 0) {
    std::printf("det-crosscheck: %u checked, %u skipped (boundary work)%s\n",
                result.crosschecks, result.crosschecks_skipped,
                result.crosscheck_ok ? "" : ", DIVERGED");
    if (!result.crosscheck_ok) {
      std::fprintf(stderr, "se_replay: %s\n", result.crosscheck_report.c_str());
      // Both digest lists, sorted by Guid, to locate the divergence.
      std::fputs("guid serial-digest real-digest\n", stderr);
      const std::size_t n = std::max(result.crosscheck_expected.size(),
                                     result.crosscheck_actual.size());
      for (std::size_t i = 0; i < n; ++i) {
        const auto row = [i](const std::vector<EntityDigest>& digests) {
          return i < digests.size()
                     ? std::format("{} {:016x}", ToString(digests[i].guid),
                                   digests[i].digest)
                     : std::string("-");
        };
        std::fputs(std::format("{} | {}\n", row(result.crosscheck_expected),
                               row(result.crosscheck_actual))
                       .c_str(),
                   stderr);
      }
      status = 1;
    }
  }
  if (!args.expect_golden.empty()) {
    const testing::GoldenResult golden =
        testing::CheckGolden(args.expect_golden, result.hashes);
    if (!golden.ok) {
      std::fprintf(stderr, "se_replay: %s\n", golden.message.c_str());
      status = 1;
    }
  }
  std::fputs(std::format("se_replay: {} ticks in {} frames, final hash "
                         "{:016x}\n",
                         result.hashes.size(), result.steps_per_frame.size(),
                         result.hashes.empty() ? 0 : result.hashes.back())
                 .c_str(),
             stdout);
  return status;
}

}  // namespace
}  // namespace se

int main(int argc, char** argv) {
  // A tool may catch (ADR-006 exempts test and tool code): bad numbers in
  // the arguments throw from std::stoull.
  try {
    const std::optional<se::Args> args = se::Parse(argc, argv);
    if (!args) {
      se::Usage();
      return 2;
    }
    se::fp::ConfigureProcess();
    const se::testing::MainThreadScope main_thread;
    return se::Run(*args);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "se_replay: %s\n", e.what());
    return 2;
  }
}
