// Copyright 2026 The simple-engine-2 Authors.
//
// se_replay --from-dump (docs/milestone-specs.md M12): a C++ reader of the
// minidump layout tools/minidump/read_dump.py decodes, for the streams a
// replay needs: the session header (kind 6: module, scene, seed, tick rate,
// build, start tick, the last published tick hash), the InputFrame ring
// (kind 3), main's breadcrumb (kind 1, ThreadRegistry slot 0) and, for an
// editor Play session, the Play-start stream (kind 7: the session's replay
// file without frames, ADR-111); and the replay of a dumped rampage session
// headless up to main's breadcrumb tick.
//
//   auto session = ReadDumpedSession(dump_bytes);
//   FromDumpResult result = ReplayDumpedRampage(*session, repo_root, scene);
//
// A game's session starts from its scene file under the root. An editor
// Play session (the header's kSessionFlagPlayStart) starts from the
// Play-start bytes of the dump, written to |start_scene| and loaded from
// there, and applies the dump's SessionLog (the edits made during Play).
// M13: a game's dump carries its session stream too (the same stream
// type: the replay file without frames); its SessionLog is applied, and
// start bytes that are a content bundle run the module's states from the
// bundle's start state (rampage, or the content test module).
//
// The minidump is parsed from its header and stream directory only (the
// MINIDUMP_HEADER signature "MDMP", little-endian); first-party streams are
// read by their own headers (se_core/dump_stream.h), so a dump from another
// build of the same layout reads the same.

#ifndef TOOLS_SE_REPLAY_DUMP_SESSION_H_
#define TOOLS_SE_REPLAY_DUMP_SESSION_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "se_core/breadcrumb.h"
#include "se_core/dump_stream.h"
#include "se_core/error.h"
#include "se_core/input_frame.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_core/types.h"

namespace se::replay {

// What a dump says about its session.
struct DumpedSession {
  SessionHeaderRecord header{};
  // The session's InputFrames in tick order from header.start_tick: the
  // ring's complete slots from header.first_frame up to its head (a slot
  // being written when the dump was taken ends the list).
  std::vector<InputFrame> frames;
  std::uint64_t ring_head = 0;
  // Main's breadcrumb (ThreadRegistry slot 0) when that slot was live.
  bool main_live = false;
  BreadcrumbFields main;
  // The Play-start stream's bytes (an editor Play session, or since M13 a
  // game's session: a replay file without frames); empty when the dump
  // has none.
  std::vector<std::byte> play_start;
};

// Decodes |minidump|. Errors (kCorrupt): not a minidump, a missing or
// malformed session header or InputFrame ring, a session that has not
// begun, frames the ring no longer holds or that are out of order.
[[nodiscard]] std::expected<DumpedSession, Error> ReadDumpedSession(
    std::span<const std::byte> minidump);

// A minimal minidump of |streams| (a header, the directory, the streams'
// bytes): what the tests feed ReadDumpedSession.
struct MinidumpStream {
  std::uint32_t type = 0;
  std::span<const std::byte> bytes;
};
[[nodiscard]] std::vector<std::byte> WriteMinidump(
    std::span<const MinidumpStream> streams);

struct FromDumpResult {
  // main's breadcrumb tick (with its high bits from the frames).
  Tick breadcrumb_tick = 0;
  // Main was inside the breadcrumb's tick (no hash published for it): the
  // replay ran the ticks before it. Otherwise that tick had finished and
  // the replay ran it too.
  bool inside_tick = false;
  // The last tick replayed; hashes[i] is the hash after tick
  // header.start_tick + i.
  Tick last_tick = 0;
  std::vector<std::uint64_t> hashes;
  // The dump's published hash was compared at this tick (0: none was
  // published).
  Tick checked_tick = 0;
  // M13: the top state after each tick, for a session replayed from a
  // content bundle (empty otherwise).
  std::vector<std::string> top_states;
  // Empty when the replay ran and matched the dump; else why not.
  std::string error;
};

// Replays a dumped rampage session headless (the module, the scene under
// |root|, whose hash must match, the seed and the frames from the dump) up
// to main's breadcrumb tick, and checks the dump's last published hash. An
// editor Play session's Play-start bytes are written to |start_scene|
// (required for one) and its SessionLog is applied. A game session whose
// stream holds a content bundle (M13) runs from |start_state| (empty: the
// bundle's, else the module's game_start_state) on the bundle; such a dump
// may also come from the content test module.
[[nodiscard]] FromDumpResult ReplayDumpedRampage(
    const DumpedSession& session, const std::string& root,
    const std::string& start_scene = {}, const std::string& start_state = {})
    SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner);

}  // namespace se::replay

#endif  // TOOLS_SE_REPLAY_DUMP_SESSION_H_
