// Copyright 2026 The simple-engine-2 Authors.

#include "support/module_session.h"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"
#include "se_sim/replay.h"
#include "se_sim/session_log.h"
#include "se_sim/sim_driver.h"

namespace se::testing {
namespace {

// No input: an empty frame per tick.
class NoInput final : public InputSource {
 public:
  InputFrame Latch(Tick tick, std::uint64_t /*window_end*/,
                   bool /*last_tick_of_frame*/) override {
    InputFrame frame;
    frame.tick = tick;
    return frame;
  }
};

}  // namespace

ModuleSessionResult RunModuleSession(const ModuleSessionOptions& options) {
  ModuleSessionResult result;
  if (options.module == nullptr) {
    result.error = "no module";
    return result;
  }
  ModuleInstance module(*options.module);
  std::string_view start = options.start_state;
  if (start.empty()) start = options.module->game_start_state;
  if (start.empty()) start = options.module->start_state;
  if (!module.Factory().Contains(start)) {
    result.error = "the module has no state '" + std::string(start) + "'";
    return result;
  }
  StateEnv env;
  env.registry = &module.Registry();
  env.seed = options.seed;
  env.tick_rate = options.tick_rate;
  env.content = options.bundle != nullptr
                    ? static_cast<const ContentSource*>(options.bundle)
                    : options.content;
  GameStateStack states(module.Factory(), env);

  SessionLog log;
  if (!options.session.empty()) {
    auto read = SessionLog::Read(options.session);
    if (!read) {
      result.error = "session log: " + read.error().message;
      return result;
    }
    log = std::move(*read);
  }
  jobs::ForkJoinPool pool(options.pool);
  SimDriver driver(module.MutableSchedule(), pool);
  if (options.bundle != nullptr) {
    options.bundle->Follow(&log, &driver);
    states.SetContentGate(options.bundle);
  } else if (options.gate != nullptr) {
    states.SetContentGate(options.gate);
  }
  NoInput no_input;
  InputSource& input = options.input != nullptr ? *options.input : no_input;
  ReplayRecorder recorder;
  NullAudioSink audio;
  {
    const InlineOwnerScope owner(OnMainThread());
    states.Request(StackOp::kPush, start);
    while (driver.NextTick() <= options.ticks) {
      const InputFrame frame = input.Latch(driver.NextTick(), 0, true);
      const TickResult tick =
          driver.RunTick(states, frame, log, recorder, audio, 0);
      if (const Error* error = states.EnterError()) {
        // A state whose content did not load: the session ends here, as
        // the runtime's takes the fault path.
        result.error =
            std::format("state '{}' cannot be created: {} ({}:{}:{})",
                        states.EnterErrorState(), error->message, error->path,
                        error->line, error->column);
        break;
      }
      result.hashes.push_back(tick.hash);
      result.top_states.push_back(tick.top_state);
      if (options.on_tick) options.on_tick(tick.tick, states);
    }
    states.Clear();
  }
  if (options.bundle != nullptr) {
    result.swaps_selected = options.bundle->Selections();
    options.bundle->Follow(nullptr, nullptr);
  }
  result.session_failures = log.Failures();
  return result;
}

}  // namespace se::testing
