// Copyright 2026 The simple-engine-2 Authors.
//
// BackgroundLane: the one background I/O thread (docs/milestone-specs.md
// M9; docs/design/mt-design.md "Threads", background lane). Role
// kBackground, started through se::StartThread.
//
// Main submits jobs; the lane runs them in bounded steps and hands the
// results back:
//   - requests go through an SPSC ring (main to lane), results through an
//     SPSC completion ring (lane to main) that main drains once per frame
//     at iterate start. A request ring that is full keeps the job in a
//     main-side queue pushed on the next Submit or drain: main never waits.
//   - after pushing, main releases a binary_semaphore (at most one release
//     is outstanding: an atomic flag guards it). The lane waits on it with
//     the hot-reload scan's 500 ms period as the timeout; a stop_callback
//     releases it.
//   - a job is a sequence of steps of about 2 ms or less: file reads of at
//     most 1 MB, LZ4 blocks of about 256 KB, one scene chunk. Between two
//     steps the lane checks its stop_token and picks the next step again,
//     so an Urgent job that arrives during a long Background job runs at
//     the next step.
//   - classes: Urgent (loads someone waits on, user imports, hot reload)
//     steps run at HighQoS and always first; Background steps (autosave,
//     recovery, flushes, thumbnails, scans) at EcoQoS. The QoS switches
//     per step (se::SetCurrentThreadQos).
//   - strands: jobs with the same strand (StrandOf(destination path)) run
//     first in, first out; only a strand's oldest job is eligible, and it
//     runs Urgent when any job queued behind it is Urgent.
//   - revisions: each strand remembers the newest revision renamed into
//     place; a write never renames over a newer one (SaveOutcome::
//     superseded).
//   - Durable jobs are never cancelled: Stop() runs them to the end, and
//     the two-phase quit waits for DurablePending() to reach 0 (se_runtime's
//     LaneQuitProvider). Stop() cancels every other queued job at the next
//     step boundary, so it joins within one step.
//   - the hot-reload scan: every scan period the lane compares each watched
//     file's size and modification time and reports a change as a
//     FileChange completion (a Background step).
//   - after pushing completions the lane calls WakeMain (ADR-047: WakeMain
//     pushes only inside the window, so the lane may call it at any time).
//
// The lane never touches a World, SDL video, events or the GPU, ImGui or
// the AssetRegistry's tables: jobs receive and return values. A job that
// throws (a standard-library throw such as bad_alloc) ends with an Error
// (lane_fault_boundary.cc, ADR-079).

#ifndef SE_ASSETS_BACKGROUND_LANE_H_
#define SE_ASSETS_BACKGROUND_LANE_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <semaphore>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "se_assets/payloads.h"
#include "se_core/error.h"
#include "se_core/spsc_ring.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_os/thread.h"

namespace se {

enum class JobClass : std::uint8_t {
  kUrgent,
  kBackground,
};

[[nodiscard]] std::string_view JobClassName(JobClass job_class) noexcept;

// A job's id, unique for the lane's lifetime (never 0).
using Ticket = std::uint64_t;

// The strand of the writes to |path| (its normalised text); never 0.
[[nodiscard]] std::uint64_t StrandOf(std::string_view path);

struct JobDesc {
  JobClass job_class = JobClass::kBackground;
  // Never cancelled; quit waits for it.
  bool durable = false;
  // Jobs with one strand run first in, first out; 0: none.
  std::uint64_t strand = 0;
  // The revision a write carries.
  std::uint64_t revision = 0;
};

enum class StepStatus : std::uint8_t {
  kContinue,
  kDone,
  // Run again after StepResult::retry_after_ns (a sharing violation).
  kRetryLater,
};

struct StepResult {
  StepStatus status = StepStatus::kContinue;
  std::uint64_t retry_after_ns = 0;

  [[nodiscard]] static constexpr StepResult Continue() noexcept { return {}; }
  [[nodiscard]] static constexpr StepResult Done() noexcept {
    return {StepStatus::kDone, 0};
  }
  [[nodiscard]] static constexpr StepResult RetryAfter(
      std::uint64_t ns) noexcept {
    return {StepStatus::kRetryLater, ns};
  }
};

using LanePayload =
    std::variant<std::monostate, std::vector<std::byte>, AtlasPage, PcmPayload,
                 PrefabPayload, SceneChunkPayload, SaveOutcome, FileChange>;

struct LaneState;

// What a step may ask of the lane. Lane thread only.
class LaneContext {
 public:
  // A stop was requested: the lane cancels non-durable jobs at the next
  // step boundary; a long step may end early.
  [[nodiscard]] bool StopRequested() const noexcept;
  // Monotonic nanoseconds (steady_clock).
  [[nodiscard]] std::uint64_t NowNs() const noexcept;
  // The running job.
  [[nodiscard]] const JobDesc& Desc() const noexcept;
  [[nodiscard]] Ticket CurrentTicket() const noexcept;
  // The newest revision of |strand| renamed into place (0: none yet).
  [[nodiscard]] std::uint64_t CommittedRevision(std::uint64_t strand) const;
  void CommitRevision(std::uint64_t strand, std::uint64_t revision);
  // The hot-reload scan's watch list: a change of |path| is reported as a
  // FileChange carrying |tag|. Watching a path twice with one tag counts
  // once; Unwatch removes one tag.
  void Watch(const std::string& path, std::uint64_t tag);
  void Unwatch(const std::string& path, std::uint64_t tag);

 private:
  friend struct LaneState;
  explicit LaneContext(LaneState* state) noexcept : state_(state) {}
  LaneState* state_;
};

// One job: steps until kDone, then its result. Lane thread only (it is
// created on main and handed over through the request ring).
class LaneJob {
 public:
  virtual ~LaneJob() = default;
  // A short name for traces and stats ("read", "save", "atlas", ...).
  [[nodiscard]] virtual std::string_view Kind() const noexcept = 0;
  // One step of about 2 ms or less.
  virtual StepResult Step(LaneContext& lane) = 0;
  // After kDone: the payload, or the Error.
  [[nodiscard]] virtual std::expected<LanePayload, Error> TakeResult() = 0;
};

struct LaneCompletion {
  Ticket ticket = 0;
  JobDesc desc;
  std::string kind;
  std::expected<LanePayload, Error> result;
  std::uint32_t steps = 0;
  // The job's steps, summed.
  std::uint64_t step_ns = 0;
};

// One record of the step trace (--lane-step-trace).
struct LaneStepRecord {
  Ticket ticket = 0;
  std::uint64_t strand = 0;
  // Nanoseconds since the lane started.
  std::uint64_t start_ns = 0;
  std::uint64_t duration_ns = 0;
  // The class the step ran at (a strand's head can run Urgent).
  JobClass step_class = JobClass::kBackground;
  JobClass job_class = JobClass::kBackground;
  bool durable = false;
  // Urgent steps that were ready besides the one picked.
  std::uint32_t urgent_ready = 0;
  std::array<char, 16> kind{};
};

struct LaneStats {
  std::uint64_t submitted = 0;
  std::uint64_t completed = 0;
  std::uint64_t failed = 0;
  std::uint64_t cancelled = 0;
  std::uint64_t steps = 0;
  std::uint64_t urgent_steps = 0;
  std::uint64_t background_steps = 0;
  std::uint64_t max_step_ns = 0;
  std::uint64_t retries = 0;
  std::uint64_t scans = 0;
  std::uint64_t changes = 0;
  std::uint64_t qos_switches = 0;
  // Completions kept on the lane because the completion ring was full,
  // and those dropped at stop.
  std::uint64_t completions_held = 0;
  std::uint64_t completions_dropped = 0;
  // Submits that waited in main's queue for room in the request ring.
  std::uint64_t submits_queued = 0;
  std::uint64_t wakes = 0;
  // Steps of jobs that are not Durable that began after Stop() was
  // called (at most one: the step the lane had already picked).
  std::uint64_t steps_after_stop = 0;
  // From the stop request to the end of the last step before the exit, and
  // to the exit (0 before a stop).
  std::uint64_t stop_to_last_step_ns = 0;
  std::uint64_t stop_to_exit_ns = 0;
};

struct LaneConfig {
  std::uint32_t request_capacity = 1024;
  std::uint32_t completion_capacity = 1024;
  // The hot-reload scan period (0: no scan).
  std::uint64_t scan_period_ns = 500'000'000;
  // Wakes main after completions are pushed (null: se::WakeMain).
  void (*wake_main)() noexcept = nullptr;
  // Step trace records kept (0: none; --lane-step-trace).
  std::uint32_t trace_capacity = 0;
  std::string_view thread_name = "bg-lane";
};

// Writes |trace| as text, one step per line.
[[nodiscard]] std::expected<void, Error> WriteLaneStepTrace(
    std::span<const LaneStepRecord> trace, std::string_view path);

class BackgroundLane {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<BackgroundLane>, Error>
  Start(const LaneConfig& config);

  BackgroundLane(const BackgroundLane&) = delete;
  BackgroundLane& operator=(const BackgroundLane&) = delete;
  // Stop().
  ~BackgroundLane();

  // Main: queues |job|. Never waits.
  Ticket Submit(const JobDesc& desc, std::unique_ptr<LaneJob> job)
      SE_REQUIRES(g_main);

  // Main: calls on_completion(LaneCompletion&&) for every completion the
  // lane has pushed, oldest first. It may Submit; it must not drain again.
  template <class F>
  std::size_t DrainCompletions(F&& on_completion) SE_REQUIRES(g_main) {
    FlushQueuedSubmits();
    auto&& visit = std::forward<F>(on_completion);
    return completions_.PopAll([&visit](LaneCompletion* const& completion) {
      const std::unique_ptr<LaneCompletion> owned(completion);
      visit(std::move(*owned));
    });
  }

  // Requests a stop and joins: queued and running jobs that are not
  // Durable end at the next step boundary, Durable ones run to the end.
  // Durable submits still queued on main (a full request ring) are pushed
  // first, waiting for room; the others are cancelled. Idempotent; main
  // (shutdown step 8).
  void Stop() noexcept;
  [[nodiscard]] bool Running() const noexcept { return thread_.Joinable(); }

  // Durable jobs submitted and not yet finished.
  [[nodiscard]] std::uint32_t DurablePending() const noexcept {
    return durable_pending_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool DurableIdle() const noexcept {
    return DurablePending() == 0;
  }
  // Durable jobs that ended with an Error.
  [[nodiscard]] std::uint64_t DurableFailures() const noexcept {
    return durable_failures_.load(std::memory_order_acquire);
  }
  [[nodiscard]] LaneStats Stats() const noexcept;
  // The step trace; complete once Stop() has returned.
  [[nodiscard]] std::span<const LaneStepRecord> StepTrace() const noexcept;
  // Moves the step trace out (after Stop()).
  [[nodiscard]] std::vector<LaneStepRecord> TakeStepTrace() noexcept;
  // Writes the step trace as text, one step per line (after Stop()).
  [[nodiscard]] std::expected<void, Error> WriteStepTrace(
      std::string_view path) const;

 private:
  friend struct LaneState;
  explicit BackgroundLane(const LaneConfig& config);

  void Wake() noexcept;
  void FlushQueuedSubmits() SE_REQUIRES(g_main);
  void Run(const std::stop_token& stop);

  struct Request {
    Ticket ticket = 0;
    JobDesc desc;
    std::unique_ptr<LaneJob> job;
  };

  LaneConfig config_;
  SpscRing<Request*> requests_;
  SpscRing<LaneCompletion*> completions_;
  std::binary_semaphore semaphore_{0};
  std::atomic<bool> wake_pending_{false};
  std::atomic<std::uint32_t> durable_pending_{0};
  std::atomic<std::uint64_t> durable_failures_{0};
  // Main's side.
  Ticket next_ticket_ = 1;
  std::deque<Request*> queued_submits_;
  std::uint64_t submitted_ = 0;
  std::uint64_t submits_queued_ = 0;
  // The lane's side (lane thread only while it runs).
  std::unique_ptr<LaneState> state_;
  Thread thread_;
};

}  // namespace se

#endif  // SE_ASSETS_BACKGROUND_LANE_H_
