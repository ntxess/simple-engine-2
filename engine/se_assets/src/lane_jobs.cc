// Copyright 2026 The simple-engine-2 Authors.

#include "se_assets/lane_jobs.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "se_assets/background_lane.h"
#include "se_assets/payloads.h"
#include "se_core/assert.h"
#include "se_core/error.h"
#include "se_core/log.h"
#include "se_platform/file_stream.h"
#include "se_platform/fs.h"

namespace se {
namespace {

// A temporary file that is no longer needed; a failure only leaves it
// behind.
void RemoveTemporary(const std::string& path) {
  if (auto removed = fs::Remove(path); !removed) {
    SE_LOG_WARN("could not remove the temporary file {}: {}", path,
                removed.error().message);
  }
}

std::unexpected<Error> WithPath(Error error, std::string_view path) {
  if (error.path.empty()) error.path = std::string(path);
  return std::unexpected(std::move(error));
}

// --- Read ---

class ReadJob final : public LaneJob {
 public:
  explicit ReadJob(std::string path) : read_(std::move(path)) {}

  [[nodiscard]] std::string_view Kind() const noexcept override {
    return "read";
  }

  StepResult Step(LaneContext& /*lane*/) override {
    auto done = read_.Step();
    if (!done) {
      error_ = std::move(done.error());
      return StepResult::Done();
    }
    return *done ? StepResult::Done() : StepResult::Continue();
  }

  std::expected<LanePayload, Error> TakeResult() override {
    if (error_) return std::unexpected(std::move(*error_));
    return LanePayload(std::move(read_.Bytes()));
  }

 private:
  SteppedRead read_;
  std::optional<Error> error_;
};

// --- Save ---

class SaveJob final : public LaneJob {
 public:
  explicit SaveJob(SaveRequest request) : request_(std::move(request)) {}

  [[nodiscard]] std::string_view Kind() const noexcept override {
    return "save";
  }

  StepResult Step(LaneContext& lane) override {
    switch (phase_) {
      case Phase::kOpen:
        return Open();
      case Phase::kWrite:
        return Write();
      case Phase::kFlush:
        return Flush();
      case Phase::kRename:
        return Rename(lane);
    }
    return StepResult::Done();
  }

  std::expected<LanePayload, Error> TakeResult() override {
    if (error_) return std::unexpected(std::move(*error_));
    return LanePayload(std::move(outcome_));
  }

 private:
  enum class Phase : std::uint8_t { kOpen, kWrite, kFlush, kRename };

  StepResult Fail(Error error) {
    writer_.reset();
    if (!temp_.empty()) RemoveTemporary(temp_);
    if (error.path.empty()) error.path = request_.path;
    error_ = std::move(error);
    return StepResult::Done();
  }

  StepResult Open() {
    temp_ = fs::UniqueTempPath(request_.path);
    auto writer = fs::FileWriter::Create(temp_);
    if (!writer) return Fail(std::move(writer.error()));
    writer_.emplace(std::move(*writer));
    phase_ = request_.bytes.empty() ? Phase::kFlush : Phase::kWrite;
    return StepResult::Continue();
  }

  StepResult Write() {
    if (!writer_) return Fail(Error{ErrorCode::kIo, "no file open", {}, 0, 0});
    const std::span<const std::byte> all(request_.bytes);
    const std::size_t count = std::min(kWriteStepBytes, all.size() - written_);
    if (auto wrote = writer_->Write(all.subspan(written_, count)); !wrote) {
      return Fail(std::move(wrote.error()));
    }
    written_ += count;
    if (written_ == all.size()) phase_ = Phase::kFlush;
    return StepResult::Continue();
  }

  StepResult Flush() {
    if (!writer_) return Fail(Error{ErrorCode::kIo, "no file open", {}, 0, 0});
    if (auto flushed = writer_->FlushAndClose(); !flushed) {
      return Fail(std::move(flushed.error()));
    }
    writer_.reset();
    // The buffer is on the device: free it before the rename's retries.
    outcome_.bytes = request_.bytes.size();
    request_.bytes = {};
    phase_ = Phase::kRename;
    return StepResult::Continue();
  }

  StepResult Rename(LaneContext& lane) {
    outcome_.path = request_.path;
    outcome_.revision = request_.revision;
    const std::uint64_t strand = lane.Desc().strand;
    if (strand != 0 && lane.CommittedRevision(strand) > request_.revision) {
      // A newer revision is in place: never rename over it.
      RemoveTemporary(temp_);
      outcome_.superseded = true;
      SE_LOG_DEBUG("save of {} revision {} superseded by revision {}",
                   request_.path, request_.revision,
                   lane.CommittedRevision(strand));
      return StepResult::Done();
    }
    auto replaced = fs::TryReplace(temp_, request_.path);
    if (!replaced) return Fail(std::move(replaced.error()));
    if (*replaced == fs::ReplaceOutcome::kBusy) {
      const std::uint64_t now = lane.NowNs();
      if (first_busy_ns_ == 0) first_busy_ns_ = now;
      if (now - first_busy_ns_ >= kReplaceRetryLimitNs) {
        return Fail(Error{
            ErrorCode::kIo,
            std::format("{} is still in use by another process after {} "
                        "renames over {:.1f} s (sharing violation); the save "
                        "failed",
                        request_.path, outcome_.busy_retries + 1,
                        static_cast<double>(now - first_busy_ns_) * 1e-9),
            request_.path, 0, 0});
      }
      ++outcome_.busy_retries;
      return StepResult::RetryAfter(kReplaceRetryNs);
    }
    if (strand != 0) lane.CommitRevision(strand, request_.revision);
    return StepResult::Done();
  }

  SaveRequest request_;
  Phase phase_ = Phase::kOpen;
  std::string temp_;
  std::optional<fs::FileWriter> writer_;
  std::size_t written_ = 0;
  std::uint64_t first_busy_ns_ = 0;
  SaveOutcome outcome_;
  std::optional<Error> error_;
};

// --- Watch ---

class WatchJob final : public LaneJob {
 public:
  WatchJob(std::string path, std::uint64_t tag, bool watch)
      : path_(std::move(path)), tag_(tag), watch_(watch) {}

  [[nodiscard]] std::string_view Kind() const noexcept override {
    return watch_ ? "watch" : "unwatch";
  }

  StepResult Step(LaneContext& lane) override {
    if (watch_) {
      lane.Watch(path_, tag_);
    } else {
      lane.Unwatch(path_, tag_);
    }
    return StepResult::Done();
  }

  std::expected<LanePayload, Error> TakeResult() override {
    return LanePayload(std::monostate{});
  }

 private:
  std::string path_;
  std::uint64_t tag_;
  bool watch_;
};

// --- JSON ---

class JsonJob final : public LaneJob {
 public:
  JsonJob(std::string path, JsonKind kind)
      : read_(std::move(path)), kind_(kind) {}

  [[nodiscard]] std::string_view Kind() const noexcept override {
    return kind_ == JsonKind::kPrefab ? "prefab" : "scene_chunk";
  }

  StepResult Step(LaneContext& /*lane*/) override {
    if (!read_.Done()) {
      auto done = read_.Step();
      if (!done) {
        result_ = std::unexpected(std::move(done.error()));
        return StepResult::Done();
      }
      return StepResult::Continue();
    }
    // One step parses the file: a prefab, or one chunk of a scene.
    const std::vector<std::byte>& bytes = read_.Bytes();
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()),
                                bytes.size());
    if (kind_ == JsonKind::kPrefab) {
      auto prefab = ParsePrefab(text, read_.Path());
      result_ = prefab ? std::expected<LanePayload, Error>(
                             LanePayload(std::move(*prefab)))
                       : std::unexpected(std::move(prefab.error()));
    } else {
      auto chunk = ParseSceneChunk(text, read_.Path());
      result_ = chunk ? std::expected<LanePayload, Error>(
                            LanePayload(std::move(*chunk)))
                      : std::unexpected(std::move(chunk.error()));
    }
    return StepResult::Done();
  }

  std::expected<LanePayload, Error> TakeResult() override {
    return std::move(result_);
  }

 private:
  SteppedRead read_;
  JsonKind kind_;
  std::expected<LanePayload, Error> result_;
};

}  // namespace

// --- SteppedRead ---

std::expected<bool, Error> SteppedRead::Step() {
  if (done_) return true;
  if (!reader_) {
    auto reader = fs::FileReader::Open(path_);
    if (!reader) return WithPath(std::move(reader.error()), path_);
    if (reader->Size() > std::numeric_limits<std::size_t>::max()) {
      return std::unexpected(
          Error{ErrorCode::kIo, "file too large to read", path_, 0, 0});
    }
    // Reserved, not filled: each step touches only what it reads.
    bytes_.reserve(static_cast<std::size_t>(reader->Size()));
    reader_.emplace(std::move(*reader));
    if (reader_->AtEnd()) {
      reader_.reset();
      done_ = true;
      return true;
    }
    // Opening is a step of its own.
    return false;
  }
  const auto size = static_cast<std::size_t>(reader_->Size());
  const std::size_t count = std::min(kReadStepBytes, size - filled_);
  bytes_.resize(filled_ + count);
  auto got =
      reader_->Read(std::span<std::byte>(bytes_.data() + filled_, count));
  if (!got) return WithPath(std::move(got.error()), path_);
  filled_ += *got;
  if (*got == 0 || filled_ >= size) {
    // A file that shrank while it was read ends where it ended.
    bytes_.resize(filled_);
    reader_.reset();
    done_ = true;
  }
  return done_;
}

std::unique_ptr<LaneJob> MakeReadJob(std::string path) {
  return std::make_unique<ReadJob>(std::move(path));
}

std::unique_ptr<LaneJob> MakeSaveJob(SaveRequest request) {
  return std::make_unique<SaveJob>(std::move(request));
}

Ticket SubmitSave(BackgroundLane& lane, SaveRequest request,
                  JobClass job_class) {
  JobDesc desc;
  desc.job_class = job_class;
  desc.durable = request.durable;
  desc.strand = StrandOf(request.path);
  desc.revision = request.revision;
  return lane.Submit(desc, MakeSaveJob(std::move(request)));
}

Ticket SubmitFlush(BackgroundLane& lane, std::string path,
                   std::vector<std::byte> bytes) {
  // Main only, so a plain counter is enough.
  static std::uint64_t flush_revision = 0;
  SaveRequest request;
  request.path = std::move(path);
  request.bytes = std::move(bytes);
  request.revision = ++flush_revision;
  request.durable = false;
  return SubmitSave(lane, std::move(request), JobClass::kBackground);
}

std::unique_ptr<LaneJob> MakeWatchJob(std::string path, std::uint64_t tag,
                                      bool watch) {
  return std::make_unique<WatchJob>(std::move(path), tag, watch);
}

std::unique_ptr<LaneJob> MakeJsonJob(std::string path, JsonKind kind) {
  return std::make_unique<JsonJob>(std::move(path), kind);
}

}  // namespace se
