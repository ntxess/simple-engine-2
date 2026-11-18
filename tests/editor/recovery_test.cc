// Copyright 2026 The simple-engine-2 Authors.
//
// Recovery and autosave (M10a): the recovery file's format, its path per
// document, the autosave timer (2 s without input, once per revision), an
// autosave written through the background lane (a unique temp file, then
// the rename) and found again at the next start, and the synchronous
// write of the session-end path.

#include "se_editor/recovery.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "se_assets/background_lane.h"
#include "se_assets/lane_jobs.h"
#include "se_assets/payloads.h"
#include "se_platform/fs.h"
#include "support/test_roles.h"

namespace se::editor {
namespace {

std::string Pref(const char* name) {
  const std::string pref =
      std::string(SE_TEST_OUT_DIR) + "/recovery_" + name + "/";
  for (const RecoveryFile& file : FindRecoveryFiles(pref)) {
    REQUIRE(fs::Remove(file.path).has_value());
  }
  REQUIRE(fs::CreateDirectories(RecoveryDirectory(pref)).has_value());
  return pref;
}

TEST_CASE("Recovery files round-trip and are keyed by document", "[recovery]") {
  RecoveryFile file;
  file.document = "C:/scenes/a.scene.json";
  file.name = "a";
  file.revision = 42;
  file.scene = "{\n  \"format\": \"se-scene-v1\",\n  \"name\": \"a\\b\"\n}";
  file.path = "x";
  auto decoded = DecodeRecovery(EncodeRecovery(file), "x");
  REQUIRE(decoded.has_value());
  REQUIRE(decoded->document == file.document);
  REQUIRE(decoded->name == "a");
  REQUIRE(decoded->revision == 42);
  REQUIRE(decoded->scene == file.scene);
  REQUIRE_FALSE(DecodeRecovery("{}", "y").has_value());
  REQUIRE_FALSE(DecodeRecovery("not json", "z").has_value());

  const std::string a = RecoveryPathFor("P/", "C:/scenes/a.scene.json", "a");
  REQUIRE(a == RecoveryPathFor("P/", "C:/scenes/a.scene.json", "a"));
  REQUIRE(a != RecoveryPathFor("P/", "C:/scenes/b.scene.json", "a"));
  REQUIRE(a.ends_with(".serecovery"));
  REQUIRE(a.starts_with("P/recovery/a-"));
}

TEST_CASE("Autosave waits 2 s without input, once per revision", "[recovery]") {
  AutosaveTimer timer;
  constexpr std::uint64_t kS = 1'000'000'000;
  REQUIRE_FALSE(timer.Due(10 * kS, 9 * kS, 1, true));
  REQUIRE(timer.Due(11 * kS, 9 * kS, 1, true));
  REQUIRE_FALSE(timer.Due(11 * kS, 9 * kS, 1, false));
  timer.Submitted(1);
  REQUIRE_FALSE(timer.Due(20 * kS, 9 * kS, 1, true));
  REQUIRE(timer.Due(20 * kS, 9 * kS, 2, true));
  timer.Reset(5);
  REQUIRE_FALSE(timer.Due(20 * kS, 9 * kS, 5, true));
}

TEST_CASE("An autosave goes through the lane and is found at the next start",
          "[recovery]") {
  const testing::MainThreadScope main_thread;
  const std::string pref = Pref("lane");
  auto lane = BackgroundLane::Start(LaneConfig{});
  REQUIRE(lane.has_value());
  RecoveryFile file;
  file.document = "C:/scenes/level.scene.json";
  file.name = "level";
  file.revision = 7;
  file.scene = "{\"format\": \"se-scene-v1\"}";
  file.path = RecoveryPathFor(pref, file.document, file.name);
  const std::string text = EncodeRecovery(file);
  SaveRequest request;
  request.path = file.path;
  const auto bytes = std::as_bytes(std::span<const char>(text));
  request.bytes.assign(bytes.begin(), bytes.end());
  request.revision = 1;
  const Ticket ticket =
      SubmitSave(**lane, std::move(request), JobClass::kBackground);
  bool done = false;
  bool ok = false;
  for (int i = 0; i < 500 && !done; ++i) {
    (*lane)->DrainCompletions([&](LaneCompletion&& completion) {
      const LaneCompletion taken = std::move(completion);
      if (taken.ticket != ticket) return;
      done = true;
      ok = taken.result.has_value();
    });
    if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  (*lane)->Stop();
  REQUIRE(done);
  REQUIRE(ok);
  const std::vector<RecoveryFile> found = FindRecoveryFiles(pref);
  REQUIRE(found.size() == 1);
  REQUIRE(found[0].document == file.document);
  REQUIRE(found[0].revision == 7);
  REQUIRE(found[0].scene == file.scene);
  // No temp file is left beside it.
  auto names = fs::ListDirectory(RecoveryDirectory(pref));
  REQUIRE(names.has_value());
  REQUIRE(names->size() == 1);
}

TEST_CASE("The session-end path writes recovery files synchronously",
          "[recovery]") {
  const std::string pref = Pref("sync");
  RecoveryFile file;
  file.document = "C:/scenes/sync.scene.json";
  file.name = "sync";
  file.revision = 3;
  file.scene = "{}";
  file.path = RecoveryPathFor(pref, file.document, file.name);
  REQUIRE(WriteRecoveryNow(file).has_value());
  const std::vector<RecoveryFile> found = FindRecoveryFiles(pref);
  REQUIRE(found.size() == 1);
  REQUIRE(found[0].revision == 3);
}

}  // namespace
}  // namespace se::editor
