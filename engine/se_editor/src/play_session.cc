// Copyright 2026 The simple-engine-2 Authors.

#include "se_editor/play_session.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_core/error.h"
#include "se_core/guid.h"
#include "se_core/hash.h"
#include "se_core/input_frame.h"
#include "se_core/log.h"
#include "se_core/schedule.h"
#include "se_core/thread_role.h"
#include "se_core/world.h"
#include "se_platform/fs.h"
#include "se_render_core/render_list.h"
#include "se_sim/audio_events.h"
#include "se_sim/content_bundle.h"
#include "se_sim/content_source.h"
#include "se_sim/game_module.h"
#include "se_sim/game_state.h"
#include "se_sim/replay.h"
#include "se_sim/session_log.h"
#include "se_sim/sim_driver.h"
#include "src/play_fault_boundary.h"
#include "src/play_states.h"

namespace se::editor {
namespace {

// The inject-throw tick hook: the tick's audio drain, which RunTick calls
// for each World after its Schedule step, throws once.
class InjectingAudio final : public AudioSink {
 public:
  InjectingAudio(AudioSink& inner, bool& armed)
      : inner_(&inner), armed_(&armed) {}
  void Drain(World& world, std::uint64_t tick_window_end_ns)
      SE_REQUIRES(g_sim_owner) override {
    if (*armed_) {
      *armed_ = false;
      ThrowInjected("a play tick");
    }
    inner_->Drain(world, tick_window_end_ns);
  }

 private:
  AudioSink* inner_;
  bool* armed_;
};

std::string_view TextOf(const std::vector<std::byte>& bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()),
                          bytes.size());
}

}  // namespace

class PlaySession::RecordingContent final : public ContentSource {
 public:
  // |scene_path|'s text is recorded under |scene_key| (both empty: none).
  RecordingContent(const ContentSource& inner, std::string_view start_state,
                   std::string_view scene_path, std::string_view scene_key)
      : inner_(&inner),
        scene_path_(scene_path.empty() ? std::string()
                                       : fs::Normalize(scene_path)),
        scene_key_(scene_key) {
    bundle_.SetStartState(start_state);
  }

  [[nodiscard]] std::expected<std::string_view, Error> Text(
      std::string_view path) const override {
    auto text = inner_->Text(path);
    // Keyed as se_replay reads it: the normalised path, or for the
    // Play-start file (a per-user path) its key.
    std::string key = fs::Normalize(path);
    if (!scene_key_.empty() && key == scene_path_) key = scene_key_;
    if (text) bundle_.Add(key, *text);
    return text;
  }

  [[nodiscard]] const ContentBundle& Bundle() const noexcept { return bundle_; }

 private:
  const ContentSource* inner_;
  std::string scene_path_;
  std::string scene_key_;
  // Text() is const (ContentSource); the bundle is what it served.
  mutable ContentBundle bundle_;
};

std::string_view PlayStateName(PlayState state) noexcept {
  switch (state) {
    case PlayState::kPlaying:
      return "Playing";
    case PlayState::kPaused:
      return "Paused";
    case PlayState::kStepping:
      return "Stepping";
    case PlayState::kFaulted:
      return "Faulted";
  }
  return "?";
}

PlaySession::PlaySession(ModuleInstance& instance, jobs::ForkJoinPool& pool,
                         PlayStart start)
    : instance_(&instance),
      start_(std::move(start)),
      kind_(start_.kind),
      driver_(instance.MutableSchedule(), pool) {}

std::expected<std::unique_ptr<PlaySession>, Error> PlaySession::Start(
    ModuleInstance& instance, jobs::ForkJoinPool& pool, const EditorGame& game,
    PlayStart start, TickGate* gate, StateContentGate* content_gate,
    const ContentSource* content) {
  AssertMainThread();
  const std::string_view start_state = instance.Module().start_state;
  if (start_state.empty()) {
    return Fail(ErrorCode::kInvalidArgument, "the module has no start state");
  }
  std::unique_ptr<PlaySession> session(
      new PlaySession(instance, pool, std::move(start)));
  PlaySession& s = *session;
  StateEnv env;
  env.registry = &instance.Registry();
  env.seed = s.start_.seed;
  env.tick_rate = s.start_.tick_rate;
  env.world_kind = WorldKind::kPlay;
  const InlineOwnerScope owner(OnMainThread());
  // Play pushes the one-World start state; 'Play game' the state a
  // player's session starts in (M13: rampage's Title).
  std::string_view first = start_state;
  if (s.kind_ == PlayKind::kPlay) {
    // The serializer round trip: the document's bytes into a fresh World.
    if (!game.load_scene) {
      return Fail(ErrorCode::kUnsupported, "the game has no scene loader");
    }
    WorldDesc desc;
    desc.name = std::string(start_state);
    desc.kind = WorldKind::kPlay;
    desc.seed = env.seed;
    desc.tick_rate = env.tick_rate;
    auto world = std::make_unique<World>(instance.Registry(), std::move(desc));
    {
      const MutationGate gate_open = world->OpenGate(GateReason::kLoad);
      // The Play-start bytes are flattened: no prefab is read.
      if (auto loaded = game.load_scene(*world, TextOf(s.start_.bytes),
                                        s.start_.asset_root, {});
          !loaded) {
        return std::unexpected(std::move(loaded.error()));
      }
      // The rules' sounds (M8); never part of the World's state.
      AddAudioEvents(*world);
      world->RecordHash();
    }
    RegisterPlayWorldState(s.factory_, start_state);
    SetPendingPlayWorld(std::move(world), &s.play_running_);
    s.states_ = std::make_unique<GameStateStack>(s.factory_, env);
  } else {
    if (game.set_play_game_scene) {
      game.set_play_game_scene(s.start_.scene_path, s.start_.asset_root);
    }
    const std::string_view game_start = instance.Module().game_start_state;
    if (!game_start.empty()) first = game_start;
    // The runtime's rules (M13): texts from the content source, every push
    // through the content gate; what the states read is kept for the
    // session's replay and dump (a content bundle).
    if (content != nullptr) {
      s.recording_ = std::make_unique<RecordingContent>(
          *content, first, s.start_.scene_path, s.start_.scene_key);
      env.content = s.recording_.get();
    }
    s.states_ = std::make_unique<GameStateStack>(instance.Factory(), env);
    s.states_->SetContentGate(content_gate);
  }
  s.states_->Request(StackOp::kPush, first);
  s.states_->ApplyPending();
  if (const Error* error = s.states_->EnterError()) {
    Error out = *error;
    out.message = std::format("the start state '{}' cannot be created: {}",
                              first, error->message);
    return std::unexpected(std::move(out));
  }
  if (s.states_->RejectedCount() != 0 ||
      (s.states_->Depth() != 1 && s.states_->Waiting() != first)) {
    return Fail(ErrorCode::kInvalidArgument,
                std::format("the start state '{}' could not be pushed", first));
  }
  if (s.states_->Waiting() == first) {
    SE_LOG_INFO(
        "play game: '{}' waits at the content gate (no tick before its "
        "texts and assets are resident)",
        first);
  }
  s.driver_.SetTickGate(gate);
  s.recorder_.Begin(
      MakeReplayHeader(s.start_.bytes, s.start_.seed, s.start_.tick_rate),
      s.start_.bytes);
  return session;
}

PlaySession::~PlaySession() {
  if (!states_) return;
  AssertMainThread();
  const InlineOwnerScope owner(OnMainThread());
  states_->Clear();
}

void PlaySession::SetRunning(bool running) noexcept { play_running_ = running; }

void PlaySession::CompleteBoundary() {
  AssertMainThread();
  if (!states_ || states_->Waiting().empty() || Faulted()) return;
  {
    const InlineOwnerScope owner(OnMainThread());
    driver_.CompleteBoundary(*states_, log_);
  }
  FaultOnEnterError();
}

void PlaySession::FaultOnEnterError() {
  if (!states_ || Faulted() || states_->EnterError() == nullptr) return;
  // A state whose content did not load (M13a): the session faults as for a
  // text that did not load; the editor goes on.
  const Error& error = *states_->EnterError();
  PlayFault fault;
  fault.where = "content";
  fault.what = std::format("state '{}' cannot be created: {} ({}:{}:{})",
                           states_->EnterErrorState(), error.message,
                           error.path, error.line, error.column);
  fault.tick = LastTick();
  Fault(std::move(fault));
}

void PlaySession::Marker(SessionOp op) {
  const InlineOwnerScope owner(OnMainThread());
  log_.Enqueue(MakeMarker(driver_.NextTick(), op), driver_.NextTick(),
               *states_);
}

void PlaySession::Pause() {
  if (state_ != PlayState::kPlaying) return;
  state_ = PlayState::kPaused;
  SetRunning(false);
  Marker(SessionOp::kPause);
}

void PlaySession::Resume() {
  if (state_ != PlayState::kPaused && state_ != PlayState::kStepping) return;
  state_ = PlayState::kPlaying;
  SetRunning(true);
  // Drawn at alpha 1 until this tick has run (DrawsStill()).
  resumed_at_tick_ = driver_.NextTick();
  Marker(SessionOp::kResume);
}

void PlaySession::Step() {
  if (state_ != PlayState::kPaused) return;
  state_ = PlayState::kStepping;
  Marker(SessionOp::kStep);
}

int PlaySession::TickFrame(const StepPlan& plan, InputSource& input,
                           AudioSink& audio, std::uint64_t now_ns) {
  AssertMainThread();
  StepPlan run = plan;
  const bool stepping = state_ == PlayState::kStepping;
  if (stepping) {
    // Exactly one tick with a fresh InputFrame, whatever the plan.
    run = StepPlan{};
    run.steps = 1;
    run.alpha = 1.0F;
    run.tick_window_end_ns[0] = now_ns;
    SetRunning(true);
  } else if (state_ != PlayState::kPlaying) {
    return 0;
  }
  if (run.steps <= 0) {
    if (stepping) SetRunning(false);
    return 0;
  }
  InjectingAudio sink(audio, inject_tick_throw_);
  auto body = [&] {
    AssertMainThread();
    driver_.TickInline(*states_, run, input, log_, recorder_, sink);
  };
  const ContainedFault fault = RunContained(body);
  const int ran = static_cast<int>(driver_.FrameResults().size());
  if (fault.faulted) {
    PlayFault info;
    info.where = "tick";
    info.what = fault.what;
    info.tick = driver_.NextTick();
    info.breadcrumb = fault.breadcrumb;
    const auto systems = instance_->MutableSchedule().Stats();
    if (fault.breadcrumb.system < systems.size()) {
      info.system = std::string(systems[fault.breadcrumb.system].name);
    }
    Fault(std::move(info));
    return ran;
  }
  FaultOnEnterError();
  if (Faulted()) return ran;
  if (stepping && ran > 0) {
    state_ = PlayState::kPaused;
    SetRunning(false);
  }
  return ran;
}

std::expected<void, Error> PlaySession::Edit(const EditorCommand& command,
                                             Tick apply_before) {
  if (Faulted()) {
    return Fail(ErrorCode::kInvalidArgument,
                "the play World is faulted: no edits");
  }
  const Tick next = driver_.NextTick();
  const Tick at = apply_before < next ? next : apply_before;
  auto entries = command.ToSession(instance_->Registry(), at);
  if (!entries) return std::unexpected(std::move(entries.error()));
  const InlineOwnerScope owner(OnMainThread());
  const std::uint32_t failures_before = log_.Failures();
  for (SessionEntry& entry : *entries) {
    log_.Enqueue(std::move(entry), next, *states_);
  }
  if (log_.Failures() != failures_before) {
    return Fail(
        ErrorCode::kInvalidArgument,
        std::format("'{}' did not apply to the play World", command.Label()));
  }
  return {};
}

bool PlaySession::BuildRenderList(const World& world, const Camera2D& camera,
                                  float alpha, RenderList& list) {
  AssertMainThread();
  auto build = [&] {
    if (std::exchange(inject_render_throw_, false)) {
      ThrowInjected("the play World's RenderList build");
    }
    se::BuildRenderList(world, camera, alpha, list);
  };
  const ContainedFault fault = RunContained(build);
  if (!fault.faulted) return true;
  if (!Faulted()) {
    PlayFault info;
    info.where = "render";
    info.what = fault.what;
    info.tick = LastTick();
    info.breadcrumb = fault.breadcrumb;
    Fault(std::move(info));
  } else {
    SE_LOG_ERROR("Play.Faulted: the faulted World's render build threw too: {}",
                 fault.what);
  }
  return false;
}

void PlaySession::LogAssetSwap(std::string_view key,
                               std::uint64_t content_hash) {
  Hasher name(0xA55E75A9);
  name.AddString(key);
  const Guid asset{name.Finish(), 1};
  const InlineOwnerScope owner(OnMainThread());
  log_.Enqueue(MakeAssetSwap(driver_.NextTick(), asset, content_hash),
               driver_.NextTick(), *states_);
  SE_LOG_INFO("play: asset '{}' swapped before tick {} (hash {:016x})", key,
              driver_.NextTick(), content_hash);
}

void PlaySession::Fault(PlayFault fault) {
  if (state_ == PlayState::kFaulted) return;
  state_ = PlayState::kFaulted;
  SetRunning(false);
  fault_ = std::move(fault);
  SE_LOG_ERROR(
      "Play.Faulted: {} fault at tick {}{}: {}; the play World is frozen "
      "(Stop returns to Edit)",
      fault_.where, fault_.tick,
      fault_.system.empty()
          ? std::string()
          : std::format(" (last system run: '{}')", fault_.system),
      fault_.what);
}

const World* PlaySession::TopWorld() const {
  if (!states_) return nullptr;
  const std::vector<World*> worlds = states_->TopWorlds();
  return worlds.empty() ? nullptr : worlds.front();
}

std::uint64_t PlaySession::Hash() const {
  return states_ ? states_->Hash().Folded() : 0;
}

ReplayHeader PlaySession::Header() const {
  return MakeReplayHeader(start_.bytes, start_.seed, start_.tick_rate);
}

std::vector<std::byte> PlaySession::SessionStartBytes() const {
  if (!recording_ || recording_->Bundle().Empty()) return start_.bytes;
  return recording_->Bundle().Write();
}

std::size_t PlaySession::LogEntries() const noexcept {
  const std::size_t texts =
      recording_ ? recording_->Bundle().Texts().size() : 0;
  return log_.Applied().size() + log_.PendingCount() + texts;
}

std::vector<std::byte> PlaySession::DumpBytes() const {
  const std::vector<std::byte> start = SessionStartBytes();
  ReplayRecorder file;
  file.Begin(MakeReplayHeader(start, start_.seed, start_.tick_rate), start);
  file.RecordSession(log_);
  return file.Finish();
}

std::vector<std::byte> PlaySession::FinishReplay() {
  if (!recorder_.Active()) return {};
  if (recording_ && !recording_->Bundle().Empty()) {
    recorder_.SetStart(recording_->Bundle().Write());
  }
  recorder_.RecordSession(log_);
  return recorder_.Finish();
}

}  // namespace se::editor
