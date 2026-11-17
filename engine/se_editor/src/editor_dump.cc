// Copyright 2026 The simple-engine-2 Authors.
//
// The Editor's part in dumps and in the pool's statistics (docs/milestone-
// specs.md "Integration (M10a + M10b + M12 + M14)"; ADR-111):
//
//   - While Play runs, the process's session header (M12) describes the
//     Play session (flag kSessionFlagPlayStart), the session's InputFrames
//     go into the dump ring, and the Play-start stream holds the session's
//     replay file without frames: the header, the Play-start bytes and the
//     SessionLog so far, attached again whenever the log gains an entry.
//     A dump taken during Play (a hang, a crash, Play.Faulted) therefore
//     replays headless with se_replay --from-dump, edits included. In Edit
//     mode the header says that no session runs.
//   - The fork sites of the App's pool and each phase's parallel time, for
//     the Profiler and Schedule panels.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "se_core/dump_stream.h"
#include "se_core/input_frame_ring.h"
#include "se_core/jobs.h"
#include "se_core/log.h"
#include "se_core/profile_clock.h"
#include "se_core/schedule.h"
#include "se_core/session_header.h"
#include "se_core/thread_role.h"
#include "se_editor/editor.h"
#include "se_editor/editor_stats.h"
#include "se_editor/play_session.h"
#include "se_runtime/app.h"
#include "se_runtime/integration.h"
#include "se_sim/game_module.h"
#include "se_sim/replay.h"
#include "se_sim/sim_driver.h"

namespace se::editor {

void Editor::BeginIdleHeader() {
  const ReplayHeader build = MakeReplayHeader({}, 0, kDefaultTickRate);
  SessionHeaderInfo info;
  info.module = game_.module != nullptr ? game_.module->name : "";
  info.scene =
      Active() != nullptr ? std::string_view(Active()->Desc().name) : "";
  info.build = build.build;
  info.platform = build.platform;
  info.compiler = build.compiler;
  info.math_hash = build.math_hash;
  info.cpu_features = build.cpu_features;
  info.start_tick = 0;
  info.first_frame = integration::DumpInputRing().Head();
  info.flags = kSessionFlagNoSession;
  integration::DumpSessionHeader().Begin(info);
}

void Editor::AttachPlayDump() {
  if (!play_) return;
  InputFrameRing& ring = integration::DumpInputRing();
  play_->AttachInputRing(&ring);
  const ReplayHeader header = play_->Header();
  SessionHeaderInfo info;
  info.module = game_.module != nullptr ? game_.module->name : "";
  info.scene =
      Active() != nullptr ? std::string_view(Active()->Desc().name) : "";
  info.scene_hash = header.scene_hash;
  info.seed = header.seed;
  info.tick_rate = header.tick_rate;
  info.build = header.build;
  info.platform = header.platform;
  info.compiler = header.compiler;
  info.math_hash = header.math_hash;
  info.cpu_features = header.cpu_features;
  info.start_tick = play_->NextTick();
  info.first_frame = ring.Head();
  info.flags = kSessionFlagPlayStart;
  integration::DumpSessionHeader().Begin(info);
  // Attached below on the first sync.
  play_stream_entries_ = kNoPlayStream;
  SyncPlayDump();
}

void Editor::SyncPlayDump() {
  if (!play_) return;
  const std::size_t entries = play_->LogEntries();
  if (entries == play_stream_entries_) return;
  const std::vector<std::byte> bytes = play_->DumpBytes();
  if (auto attached = play_stream_.Attach(bytes); !attached) {
    SE_LOG_WARN("play: the Play-start dump stream: {}",
                attached.error().message);
  }
  play_stream_entries_ = entries;
}

void Editor::PublishPlayTicks() {
  if (!play_) return;
  const std::span<const TickResult> results = play_->FrameResults();
  if (results.empty()) return;
  integration::DumpSessionHeader().PublishTick(results.back().tick,
                                               results.back().hash);
}

void Editor::DetachPlayDump() {
  play_stream_.Detach();
  play_stream_entries_ = kNoPlayStream;
  BeginIdleHeader();
}

void Editor::FillForkStats(App& app) {
  fork_sites_.clear();
  phase_parallel_ns_.assign(kPhaseCount, 0);
  for (const SystemStats& system : stats_.systems) {
    phase_parallel_ns_[static_cast<std::size_t>(system.phase)] +=
        system.last_fork_ns;
  }
  stats_.phase_parallel_ns = phase_parallel_ns_;
  const jobs::ForkJoinPool* pool = app.GamePool();
  if (pool == nullptr) return;
  // The sites' records are the tick owner's data; the editor ticks inline,
  // so between ticks they are main's to read.
  const std::uint64_t now = ProfileNowNs();
  for (const jobs::ForkSite* site : pool->Sites()) {
    ForkSiteStats shown;
    shown.name = site->name;
    shown.enabled = site->enabled;
    // A fallback window that has ended is not shown as one.
    shown.inline_until_ns =
        site->inline_until_ns > now ? site->inline_until_ns : 0;
    shown.stats = site->stats;
    fork_sites_.push_back(shown);
  }
  stats_.fork_sites = fork_sites_;
  stats_.fork_workers = pool->ActiveWorkers();
}

}  // namespace se::editor
