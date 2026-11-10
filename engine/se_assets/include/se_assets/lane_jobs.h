// Copyright 2026 The simple-engine-2 Authors.
//
// The background lane's standard jobs (docs/milestone-specs.md M9). Each is
// a LaneJob made on main and run in bounded steps on the lane:
//
//   MakeReadJob        a whole file, at most kReadStepBytes (1 MB) per
//                      step (fs::FileReader: FILE_SHARE_DELETE on Windows)
//   MakeSaveJob        a SaveRequest: a unique temporary file in the
//                      destination's directory, written at most
//                      kWriteStepBytes per step, flushed to the device,
//                      then renamed into place unless the strand already
//                      holds a newer revision; a sharing violation retries
//                      every kReplaceRetryNs for up to kReplaceRetryLimitNs
//                      (2 s), then fails and removes the temporary file
//                      (the document stays dirty)
//   MakeWatchJob       adds or removes a path in the hot-reload scan
//   MakeJsonJob        reads a prefab or a scene chunk and parses it
//                      (one chunk per step) with glaze
//   MakeSoundJob       reads a sound file and decodes it with the
//                      SoundDecoder the integration provides (M8's
//                      miniaudio decoder, data only on the lane)
//   (atlas_page.h)     MakeAtlasJob: a cooked atlas page, one LZ4 block
//                      per step
//
// SubmitSave fills the JobDesc of a SaveRequest: the strand of its path,
// its revision and its durable flag.

#ifndef SE_ASSETS_LANE_JOBS_H_
#define SE_ASSETS_LANE_JOBS_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "se_assets/background_lane.h"
#include "se_assets/payloads.h"
#include "se_core/error.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_platform/file_stream.h"

namespace se {

inline constexpr std::size_t kReadStepBytes = std::size_t{1} << 20;
inline constexpr std::size_t kWriteStepBytes = std::size_t{1} << 20;
inline constexpr std::uint64_t kReplaceRetryNs = 20'000'000;
inline constexpr std::uint64_t kReplaceRetryLimitNs = 2'000'000'000;

// A file read in steps, for jobs that read before they decode.
class SteppedRead {
 public:
  explicit SteppedRead(std::string path) : path_(std::move(path)) {}

  // One step: opens the file on the first, then reads at most
  // kReadStepBytes. Returns true once the whole file is in Bytes().
  [[nodiscard]] std::expected<bool, Error> Step();
  [[nodiscard]] const std::string& Path() const noexcept { return path_; }
  [[nodiscard]] std::vector<std::byte>& Bytes() noexcept { return bytes_; }
  [[nodiscard]] bool Done() const noexcept { return done_; }

 private:
  std::string path_;
  std::optional<fs::FileReader> reader_;
  std::vector<std::byte> bytes_;
  std::size_t filled_ = 0;
  bool done_ = false;
};

// Payload: std::vector<std::byte>.
[[nodiscard]] std::unique_ptr<LaneJob> MakeReadJob(std::string path);

struct SaveRequest {
  std::string path;
  std::vector<std::byte> bytes;
  // The document's revision; a newer one of the same path is never
  // overwritten.
  std::uint64_t revision = 0;
  // Never cancelled; quit waits for it.
  bool durable = false;
};

// Payload: SaveOutcome.
[[nodiscard]] std::unique_ptr<LaneJob> MakeSaveJob(SaveRequest request);

// Submits |request| as a save job on its path's strand.
Ticket SubmitSave(BackgroundLane& lane, SaveRequest request,
                  JobClass job_class = JobClass::kBackground)
    SE_REQUIRES(g_main);

// Replay, SessionLog and flight-recorder flushes: a Background save of
// |bytes| over |path| on the path's strand, not Durable, with a revision
// newer than every earlier flush (a process-wide counter).
Ticket SubmitFlush(BackgroundLane& lane, std::string path,
                   std::vector<std::byte> bytes) SE_REQUIRES(g_main);

// Payload: std::monostate.
[[nodiscard]] std::unique_ptr<LaneJob> MakeWatchJob(std::string path,
                                                    std::uint64_t tag,
                                                    bool watch);

enum class JsonKind : std::uint8_t { kPrefab, kSceneChunk };

// Payload: PrefabPayload or SceneChunkPayload.
[[nodiscard]] std::unique_ptr<LaneJob> MakeJsonJob(std::string path,
                                                   JsonKind kind);

}  // namespace se

#endif  // SE_ASSETS_LANE_JOBS_H_
