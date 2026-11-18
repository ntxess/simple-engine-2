// Copyright 2026 The simple-engine-2 Authors.
//
// Runs any game module headless from a state whose Worlds come from a
// ContentSource (M13a): one tick per frame, SimDriver::RunTick with the
// input of an InputSource and a SessionLog applied, and, when the content
// is a content bundle, the bundle as the stack's gate so that the session's
// kAssetSwap entries select the versions the live session read. Shared by
// the content gate tests, the runtime tests' replays and tools/se_replay
// (multi-state replays and dumps).

#ifndef TESTS_SUPPORT_MODULE_SESSION_H_
#define TESTS_SUPPORT_MODULE_SESSION_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "se_core/input_frame.h"
#include "se_core/jobs.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"
#include "se_sim/content_bundle.h"
#include "se_sim/content_source.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"
#include "se_sim/sim_driver.h"

namespace se::testing {

struct ModuleSessionOptions {
  const GameModule* module = nullptr;
  // Empty: the module's game_start_state, else its start_state.
  std::string start_state;
  // Where the states read; |bundle| instead selects versions by the
  // session's kAssetSwap entries (it is then also the stack's gate).
  const ContentSource* content = nullptr;
  ContentBundleSource* bundle = nullptr;
  // A gate for the stack instead (tests of the gate itself).
  StateContentGate* gate = nullptr;
  std::uint64_t seed = 42;
  std::uint32_t tick_rate = kDefaultTickRate;
  Tick ticks = 600;
  InputSource* input = nullptr;
  // SessionLog::Write bytes to apply; empty: none.
  std::vector<std::byte> session;
  // Called after every tick.
  std::function<void(Tick, const GameStateStack&)> on_tick;
  jobs::ForkJoinPool::Config pool{};
};

struct ModuleSessionResult {
  // hashes[i] is the hash after tick i + 1; top_states[i] its top state.
  std::vector<std::uint64_t> hashes;
  std::vector<std::string> top_states;
  std::uint32_t session_failures = 0;
  std::uint32_t swaps_selected = 0;
  std::string error;
};

[[nodiscard]] ModuleSessionResult RunModuleSession(
    const ModuleSessionOptions& options) SE_REQUIRES(g_main)
    SE_EXCLUDES(g_sim_owner);

}  // namespace se::testing

#endif  // TESTS_SUPPORT_MODULE_SESSION_H_
