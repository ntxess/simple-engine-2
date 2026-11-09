// Copyright 2026 The simple-engine-2 Authors.
//
// se-fault-boundary: spdlog reports file errors (opening, writing,
// rotating, flushing) with spdlog::spdlog_ex. The sinks are created and
// called only here; each call catches, counts the failure and carries on,
// so a full disk never takes the worker down (ADR-030).

#include "se_log/log_worker.h"

#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "se_core/dump_stream.h"
#include "se_core/error.h"
#include "se_core/log.h"
#include "se_core/log_ring.h"
#include "se_core/profile_clock.h"
#include "se_core/thread_registry.h"
#include "se_core/thread_role.h"
#include "se_os/thread.h"

namespace se {
namespace {

constexpr std::uint64_t kSecondNs = 1'000'000'000;
constexpr std::array<char, 6> kLevelLetters = {'T', 'D', 'I', 'W', 'E', 'F'};

spdlog::level::level_enum ToSpdlog(LogLevel level) {
  switch (level) {
    case LogLevel::kTrace:
      return spdlog::level::trace;
    case LogLevel::kDebug:
      return spdlog::level::debug;
    case LogLevel::kInfo:
      return spdlog::level::info;
    case LogLevel::kWarn:
      return spdlog::level::warn;
    case LogLevel::kError:
      return spdlog::level::err;
    case LogLevel::kFatal:
      return spdlog::level::critical;
  }
  return spdlog::level::info;
}

LogLevel FromSpdlog(spdlog::level::level_enum level) {
  switch (level) {
    case spdlog::level::trace:
      return LogLevel::kTrace;
    case spdlog::level::debug:
      return LogLevel::kDebug;
    case spdlog::level::info:
      return LogLevel::kInfo;
    case spdlog::level::warn:
      return LogLevel::kWarn;
    case spdlog::level::err:
      return LogLevel::kError;
    default:
      return LogLevel::kFatal;
  }
}

// stderr, through the front end's console writer: a sink a test installed
// with SetLogSink receives the worker's lines as it receives synchronous
// ones (the M4 integration's in-process App tests read them).
class ConsoleSink final : public spdlog::sinks::sink {
 public:
  void log(const spdlog::details::log_msg& msg) override {
    WriteConsoleLine(FromSpdlog(msg.level),
                     std::string_view(msg.payload.data(), msg.payload.size()));
  }
  void flush() override { std::fflush(stderr); }
  void set_pattern(const std::string& /*pattern*/) override {}
  void set_formatter(
      std::unique_ptr<spdlog::formatter> /*sink_formatter*/) override {}
};

// The last lines written, for M10's log panel. The worker appends under the
// mutex; the main thread copies with try_lock only.
class PanelSink final : public spdlog::sinks::sink {
 public:
  explicit PanelSink(std::size_t capacity) : lines_(capacity) {}

  void log(const spdlog::details::log_msg& msg) override {
    const std::scoped_lock lock(mutex_);
    lines_[next_].assign(msg.payload.data(), msg.payload.size());
    next_ = (next_ + 1) % lines_.size();
    count_ = std::min(count_ + 1, lines_.size());
  }
  void flush() override {}
  void set_pattern(const std::string& /*pattern*/) override {}
  void set_formatter(
      std::unique_ptr<spdlog::formatter> /*sink_formatter*/) override {}

  bool TryCopy(std::vector<std::string>& out) {
    const std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    out.clear();
    out.reserve(count_);
    const std::size_t first = (next_ + lines_.size() - count_) % lines_.size();
    for (std::size_t i = 0; i < count_; ++i) {
      out.push_back(lines_[(first + i) % lines_.size()]);
    }
    return true;
  }

 private:
  std::mutex mutex_;
  std::vector<std::string> lines_;
  std::size_t next_ = 0;
  std::size_t count_ = 0;
};

// One call site's rate-limit window.
struct SiteWindow {
  std::uint64_t start_ns = 0;
  std::uint32_t count = 0;
  std::uint64_t suppressed = 0;
  LogRecord last;
};

bool Earlier(const LogRecord& a, const LogRecord& b) {
  return a.timestamp_ns != b.timestamp_ns ? a.timestamp_ns < b.timestamp_ns
                                          : a.sequence < b.sequence;
}

void SetMessage(LogRecord& record, std::string_view text) {
  const std::size_t size = std::min(text.size(), kLogRecordMessageSize);
  text.copy(record.message, size);
  record.length = static_cast<std::uint16_t>(size);
}

void SetFile(LogRecord& record, std::string_view file) {
  std::fill(std::begin(record.file), std::end(record.file), '\0');
  file.copy(record.file, std::min(file.size(), kLogRecordFileSize));
}

}  // namespace

void FormatLogLine(const LogRecord& record, std::string& out) {
  const std::uint64_t epoch = LogEpochNs();
  const std::uint64_t since =
      record.timestamp_ns > epoch ? record.timestamp_ns - epoch : 0;
  const std::size_t level =
      std::min<std::size_t>(record.level, kLevelLetters.size() - 1);
  out.clear();
  std::format_to(std::back_inserter(out), "{}.{:06} {} {} t={} {}:{} {}",
                 since / kSecondNs, (since % kSecondNs) / 1000,
                 kLevelLetters[level],
                 RoleName(static_cast<ThreadRole>(record.role)), record.tick,
                 record.File(), record.line, record.Message());
}

struct LogWorker::State final : public LogConsumer {
  explicit State(const LogConfig& c) : config(c) {
    // Only drops from now on are reported.
    ThreadRegistry::Instance().ForEachLogRing(
        [&](const LogRing& ring) { reported_drops[&ring] = ring.Dropped(); });
  }

  State(const State&) = delete;
  State& operator=(const State&) = delete;
  ~State() override = default;

  void OnRecordPushed() noexcept override {
    if (config.wake_on_push) Wake();
  }

  bool Flush(std::uint64_t timeout_ns) noexcept override {
    if (std::this_thread::get_id() == worker_id.load()) return false;
    const std::uint64_t generation =
        flush_requested.fetch_add(1, std::memory_order_acq_rel) + 1;
    Wake();
    const std::uint64_t deadline = ProfileNowNs() + timeout_ns;
    while (flush_done.load(std::memory_order_acquire) < generation) {
      if (ProfileNowNs() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

  // Releases the semaphore if the worker is parked; only the caller that
  // clears `parked` releases it, so it never counts past one.
  void Wake() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (parked.load(std::memory_order_relaxed) &&
        parked.exchange(false, std::memory_order_acq_rel)) {
      wake.release();
    }
  }

  std::expected<void, Error> CreateSinks();
  void Run();
  void Park();
  [[nodiscard]] bool HasWork() const;
  void DrainOnce(bool everything);
  void WriteRecord(const LogRecord& record);
  void WriteSummary(const std::string& site_key, SiteWindow& site);
  void WriteLine(LogLevel level, std::string_view text);
  void FlushSinks();

  const LogConfig config;
  std::vector<std::shared_ptr<spdlog::sinks::sink>> sinks;
  std::shared_ptr<PanelSink> panel;

  std::binary_semaphore wake{0};
  std::atomic<bool> parked{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> stopped{false};
  std::atomic<std::uint64_t> flush_requested{0};
  std::atomic<std::uint64_t> flush_done{0};
  std::atomic<std::thread::id> worker_id{};
  Thread thread;

  // Worker only.
  std::vector<LogRecord> pending;
  std::unordered_map<const LogRing*, std::uint64_t> reported_drops;
  std::unordered_map<std::string, SiteWindow> sites;
  std::string line;
  std::string key;
  bool wrote = false;
  bool sink_error_reported = false;

  std::atomic<std::uint64_t> records{0};
  std::atomic<std::uint64_t> lines{0};
  std::atomic<std::uint64_t> suppressed{0};
  std::atomic<std::uint64_t> dropped{0};
  std::atomic<std::uint64_t> sink_errors{0};
  std::atomic<std::uint64_t> parks{0};
};

std::expected<void, Error> LogWorker::State::CreateSinks() {
  try {
    if (!config.file_path.empty()) {
      auto file = std::make_shared<spdlog::sinks::rotating_file_sink_st>(
          config.file_path, config.file_max_bytes, config.file_max_files);
      file->set_pattern("%v");
      file->set_level(spdlog::level::trace);
      sinks.push_back(std::move(file));
    }
    if (config.stderr_enabled) {
      auto err = std::make_shared<ConsoleSink>();
      err->set_level(ToSpdlog(config.stderr_level));
      sinks.push_back(std::move(err));
    }
    if (config.panel_lines > 0) {
      panel = std::make_shared<PanelSink>(config.panel_lines);
      panel->set_level(spdlog::level::trace);
      sinks.push_back(panel);
    }
  } catch (const std::exception& error) {
    Error failure{ErrorCode::kIo,
                  std::format("cannot open the log sinks: {}", error.what()),
                  config.file_path, 0, 0};
    return std::unexpected(std::move(failure));
  }
  return {};
}

void LogWorker::State::Run() {
  worker_id.store(std::this_thread::get_id());
  for (;;) {
    const bool stopping = stop.load(std::memory_order_acquire);
    const std::uint64_t flush_generation =
        flush_requested.load(std::memory_order_acquire);
    const bool flushing =
        flush_generation != flush_done.load(std::memory_order_relaxed);
    wrote = false;
    DrainOnce(stopping || flushing);
    if (stopping) {
      for (auto& [site_key, site] : sites) WriteSummary(site_key, site);
      FlushSinks();
      flush_done.store(flush_generation, std::memory_order_release);
      return;
    }
    if (flushing || wrote) FlushSinks();
    if (flushing) flush_done.store(flush_generation, std::memory_order_release);
    Park();
  }
}

bool LogWorker::State::HasWork() const {
  if (stop.load(std::memory_order_acquire)) return true;
  if (flush_requested.load(std::memory_order_acquire) !=
      flush_done.load(std::memory_order_relaxed)) {
    return true;
  }
  bool records_waiting = false;
  ThreadRegistry::Instance().ForEachLogRing([&](const LogRing& ring) {
    records_waiting = records_waiting || ring.HasRecords();
  });
  return records_waiting && config.wake_on_push;
}

void LogWorker::State::Park() {
  parks.fetch_add(1, std::memory_order_relaxed);
  parked.store(true, std::memory_order_seq_cst);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (HasWork()) {
    // Unpark; a waker that cleared the flag first owes a release.
    if (!parked.exchange(false, std::memory_order_acq_rel)) wake.acquire();
    return;
  }
  if (wake.try_acquire_for(std::chrono::milliseconds(config.wake_period_ms))) {
    return;  // the waker cleared the flag
  }
  if (!parked.exchange(false, std::memory_order_acq_rel)) wake.acquire();
}

void LogWorker::State::DrainOnce(bool everything) {
  ThreadRegistry& registry = ThreadRegistry::Instance();
  // Records older than every write in progress can be written; a write in
  // progress stamps its record at or after its start.
  std::uint64_t watermark = std::numeric_limits<std::uint64_t>::max();
  if (!everything) {
    watermark = ProfileNowNs();
    registry.ForEachLogRing([&](const LogRing& ring) {
      const std::uint64_t started = ring.WriteStartedNs();
      if (started != 0) watermark = std::min(watermark, started);
    });
  }
  struct DropNotice {
    std::uint64_t count;
    std::array<char, kDumpStreamNameSize + 1> name;
  };
  std::vector<DropNotice> notices;
  registry.ForEachLogRing([&](LogRing& ring) {
    records.fetch_add(
        ring.Drain([&](const LogRecord& record) { pending.push_back(record); }),
        std::memory_order_relaxed);
    std::uint64_t& reported = reported_drops[&ring];
    if (const std::uint64_t total = ring.Dropped(); total > reported) {
      notices.push_back({total - reported, ring.Name()});
      reported = total;
    }
  });
  std::sort(pending.begin(), pending.end(), Earlier);
  const auto ready = std::partition_point(
      pending.begin(), pending.end(),
      [&](const LogRecord& record) { return record.timestamp_ns < watermark; });
  for (auto it = pending.begin(); it != ready; ++it) WriteRecord(*it);
  pending.erase(pending.begin(), ready);

  const std::uint64_t now = ProfileNowNs();
  for (const DropNotice& notice : notices) {
    LogRecord record;
    record.timestamp_ns = now;
    record.level = static_cast<std::uint8_t>(LogLevel::kWarn);
    record.role = static_cast<std::uint8_t>(ThreadRole::kLogWorker);
    record.line = static_cast<std::uint32_t>(__LINE__);
    SetFile(record, "log_worker.cc");
    SetMessage(record, std::format("{} records dropped on {}", notice.count,
                                   notice.name.data()));
    FormatLogLine(record, line);
    WriteLine(LogLevel::kWarn, line);
    dropped.fetch_add(notice.count, std::memory_order_relaxed);
  }
  // Summaries of windows that have ended.
  for (auto& [site_key, site] : sites) {
    if (site.suppressed > 0 && now >= site.start_ns + kSecondNs) {
      WriteSummary(site_key, site);
    }
  }
}

void LogWorker::State::WriteRecord(const LogRecord& record) {
  const std::uint32_t limit = config.rate_limit_per_second;
  if (limit != 0) {
    key.assign(record.File());
    key += ':';
    key += std::to_string(record.line);
    SiteWindow& site = sites[key];
    if (site.count == 0 && site.suppressed == 0) {
      site.start_ns = record.timestamp_ns;
    }
    if (record.timestamp_ns >= site.start_ns + kSecondNs) {
      WriteSummary(key, site);
      site.start_ns = record.timestamp_ns;
      site.count = 0;
    }
    if (site.count >= limit) {
      ++site.suppressed;
      site.last = record;
      suppressed.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    ++site.count;
  }
  FormatLogLine(record, line);
  WriteLine(static_cast<LogLevel>(std::min<std::uint8_t>(record.level, 5)),
            line);
}

void LogWorker::State::WriteSummary(const std::string& site_key,
                                    SiteWindow& site) {
  if (site.suppressed == 0) return;
  LogRecord record = site.last;
  SetMessage(record,
             std::format("{} more records from {} in 1 s (rate limited)",
                         site.suppressed, site_key));
  site.suppressed = 0;
  FormatLogLine(record, line);
  WriteLine(static_cast<LogLevel>(std::min<std::uint8_t>(record.level, 5)),
            line);
}

void LogWorker::State::WriteLine(LogLevel level, std::string_view text) {
  const spdlog::details::log_msg msg(
      spdlog::source_loc{}, spdlog::string_view_t{}, ToSpdlog(level),
      spdlog::string_view_t(text.data(), text.size()));
  for (const auto& sink : sinks) {
    if (!sink->should_log(msg.level)) continue;
    try {
      sink->log(msg);
    } catch (const std::exception& error) {
      sink_errors.fetch_add(1, std::memory_order_relaxed);
      if (!sink_error_reported) {
        sink_error_reported = true;
        std::fprintf(stderr, "log sink failed: %s\n", error.what());
      }
    }
  }
  lines.fetch_add(1, std::memory_order_relaxed);
  wrote = true;
}

void LogWorker::State::FlushSinks() {
  for (const auto& sink : sinks) {
    try {
      sink->flush();
    } catch (const std::exception& error) {
      sink_errors.fetch_add(1, std::memory_order_relaxed);
      if (!sink_error_reported) {
        sink_error_reported = true;
        std::fprintf(stderr, "log sink flush failed: %s\n", error.what());
      }
    }
  }
}

LogWorker::LogWorker(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

LogWorker::LogWorker(LogWorker&& other) noexcept = default;

LogWorker& LogWorker::operator=(LogWorker&& other) noexcept {
  if (this != &other) {
    Stop();
    state_ = std::move(other.state_);
  }
  return *this;
}

LogWorker::~LogWorker() { Stop(); }

std::expected<LogWorker, Error> LogWorker::Start(const LogConfig& config) {
  // The time column counts from here unless something logged earlier.
  static_cast<void>(LogEpochNs());
  auto state = std::make_unique<State>(config);
  if (auto sinks = state->CreateSinks(); !sinks) {
    return std::unexpected(std::move(sinks.error()));
  }
  if (!AttachLogConsumer(state.get())) {
    return Fail(ErrorCode::kUnsupported, "a log worker is already running");
  }
  State* raw = state.get();
  auto thread = StartThread(
      ThreadSpec{.role = ThreadRole::kLogWorker, .name = "log-worker"},
      [raw](const std::stop_token& /*stop*/) { raw->Run(); });
  if (!thread) {
    DetachLogConsumer(raw);
    return std::unexpected(std::move(thread.error()));
  }
  state->thread = std::move(*thread);
  return LogWorker(std::move(state));
}

void LogWorker::Stop() noexcept {
  if (!state_ || state_->stopped.load()) return;
  // From here on the front end writes synchronously; records already in
  // the rings are written by the worker's last wake.
  DetachLogConsumer(state_.get());
  state_->stop.store(true, std::memory_order_release);
  state_->Wake();
  state_->thread.Join();
  state_->stopped.store(true);
  try {
    spdlog::shutdown();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "spdlog::shutdown failed: %s\n", error.what());
  }
}

bool LogWorker::Running() const noexcept {
  return state_ != nullptr && !state_->stopped.load();
}

bool LogWorker::TryCopyPanelLines(std::vector<std::string>& lines) const {
  return state_ != nullptr && state_->panel != nullptr &&
         state_->panel->TryCopy(lines);
}

LogWorkerStats LogWorker::Stats() const noexcept {
  LogWorkerStats stats;
  if (state_ == nullptr) return stats;
  stats.records = state_->records.load(std::memory_order_relaxed);
  stats.lines = state_->lines.load(std::memory_order_relaxed);
  stats.suppressed = state_->suppressed.load(std::memory_order_relaxed);
  stats.dropped = state_->dropped.load(std::memory_order_relaxed);
  stats.sink_errors = state_->sink_errors.load(std::memory_order_relaxed);
  stats.parks = state_->parks.load(std::memory_order_relaxed);
  return stats;
}

}  // namespace se
