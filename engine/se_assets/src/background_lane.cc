// Copyright 2026 The simple-engine-2 Authors.

#include "se_assets/background_lane.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "se_assets/payloads.h"
#include "se_core/assert.h"
#include "se_core/error.h"
#include "se_core/hash.h"
#include "se_core/log.h"
#include "se_core/profile.h"
#include "se_core/spsc_ring.h"
#include "se_core/thread_role.h"
#include "se_os/thread.h"
#include "se_platform/file_stream.h"
#include "se_platform/fs.h"
#include "src/lane_fault_boundary.h"

namespace se {
namespace {

// Files the scan looks at per step.
constexpr std::size_t kScanFilesPerStep = 256;
// How soon the lane tries again to push completions held back by a full
// completion ring.
constexpr std::uint64_t kHeldRetryNs = 1'000'000;

std::uint64_t SteadyNowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct AtomicStats {
  std::atomic<std::uint64_t> completed{0};
  std::atomic<std::uint64_t> failed{0};
  std::atomic<std::uint64_t> cancelled{0};
  std::atomic<std::uint64_t> steps{0};
  std::atomic<std::uint64_t> urgent_steps{0};
  std::atomic<std::uint64_t> background_steps{0};
  std::atomic<std::uint64_t> max_step_ns{0};
  std::atomic<std::uint64_t> retries{0};
  std::atomic<std::uint64_t> scans{0};
  std::atomic<std::uint64_t> changes{0};
  std::atomic<std::uint64_t> qos_switches{0};
  std::atomic<std::uint64_t> completions_held{0};
  std::atomic<std::uint64_t> completions_dropped{0};
  std::atomic<std::uint64_t> wakes{0};
  std::atomic<std::uint64_t> steps_after_stop{0};
  std::atomic<std::uint64_t> stop_requested_ns{0};
  std::atomic<std::uint64_t> last_step_end_ns{0};
  std::atomic<std::uint64_t> exit_ns{0};
};

void Bump(std::atomic<std::uint64_t>& counter) noexcept {
  counter.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

std::string_view JobClassName(JobClass job_class) noexcept {
  return job_class == JobClass::kUrgent ? "urgent" : "background";
}

std::uint64_t StrandOf(std::string_view path) {
  const std::string normal = fs::Normalize(path);
  Hasher hasher;
  hasher.AddString(normal);
  const std::uint64_t strand = hasher.Finish();
  return strand == 0 ? 1 : strand;
}

// The lane thread's state. Lane thread only while it runs, except the
// atomics.
struct LaneState {
  struct Entry {
    Ticket ticket = 0;
    std::uint64_t seq = 0;
    JobDesc desc;
    std::unique_ptr<LaneJob> job;
    std::uint64_t not_before_ns = 0;
    std::uint32_t steps = 0;
    std::uint64_t step_ns = 0;
  };

  struct Watched {
    std::string path;
    std::vector<std::uint64_t> tags;
    // What the last report (or the watch) saw.
    fs::PathInfo info;
    // A change seen by one scan, reported once the next scan sees it
    // unchanged (ADR-094): a file being written is never reported half way.
    fs::PathInfo pending;
    bool has_pending = false;
  };

  explicit LaneState(BackgroundLane* owner) : lane(owner) {}

  BackgroundLane* lane;
  AtomicStats stats;
  std::uint64_t start_ns = 0;
  std::stop_token stop;

  // Queued jobs in submission order.
  std::vector<std::unique_ptr<Entry>> queue;
  std::uint64_t next_seq = 1;
  std::uint32_t durable_queued = 0;
  bool cancelled_at_stop = false;
  // Reused by Pick (no allocation per step once grown).
  std::vector<std::uint64_t> urgent_strands;
  std::vector<std::uint64_t> seen_strands;

  // The newest revision renamed into place, per strand.
  std::unordered_map<std::uint64_t, std::uint64_t> committed;

  // The hot-reload scan.
  std::vector<Watched> watched;
  std::size_t scan_cursor = 0;
  std::uint64_t next_scan_ns = 0;

  // Completions waiting for room in the completion ring.
  std::deque<LaneCompletion*> held;
  bool pushed_since_wake = false;

  // The running entry (LaneContext reads it).
  Entry* current = nullptr;
  Qos qos = Qos::kDefault;

  std::vector<LaneStepRecord> trace;
  std::uint32_t trace_capacity = 0;

  [[nodiscard]] bool Stopping() const noexcept { return stop.stop_requested(); }

  void PullRequests() {
    while (std::optional<BackgroundLane::Request*> request =
               lane->requests_.TryPop()) {
      std::unique_ptr<BackgroundLane::Request> owned(*request);
      if (Stopping() && !owned->desc.durable) {
        Bump(stats.cancelled);
        continue;
      }
      auto entry = std::make_unique<Entry>();
      entry->ticket = owned->ticket;
      entry->seq = next_seq++;
      entry->desc = owned->desc;
      entry->job = std::move(owned->job);
      if (entry->desc.durable) ++durable_queued;
      queue.push_back(std::move(entry));
    }
  }

  // At stop: every queued job that is not Durable ends here.
  void CancelNonDurable() {
    const auto removed =
        std::ranges::remove_if(queue, [](const std::unique_ptr<Entry>& entry) {
          return !entry->desc.durable;
        });
    stats.cancelled.fetch_add(
        static_cast<std::uint64_t>(std::distance(removed.begin(), queue.end())),
        std::memory_order_relaxed);
    queue.erase(removed.begin(), removed.end());
  }

  [[nodiscard]] static bool Contains(const std::vector<std::uint64_t>& list,
                                     std::uint64_t value) noexcept {
    return std::ranges::find(list, value) != list.end();
  }

  // The next step to run: the oldest ready Urgent step, else the oldest
  // ready Background step. Only a strand's oldest job is eligible; it runs
  // Urgent when an Urgent job is queued behind it. |earliest_retry| gets
  // the soonest not-before time of a job that is waiting.
  Entry* Pick(std::uint64_t now, JobClass& step_class,
              std::uint32_t& urgent_ready, std::uint64_t& earliest_retry) {
    urgent_strands.clear();
    seen_strands.clear();
    for (const std::unique_ptr<Entry>& entry : queue) {
      if (entry->desc.strand != 0 &&
          entry->desc.job_class == JobClass::kUrgent &&
          !Contains(urgent_strands, entry->desc.strand)) {
        urgent_strands.push_back(entry->desc.strand);
      }
    }
    Entry* urgent = nullptr;
    Entry* background = nullptr;
    urgent_ready = 0;
    earliest_retry = UINT64_MAX;
    for (const std::unique_ptr<Entry>& entry : queue) {
      const std::uint64_t strand = entry->desc.strand;
      if (strand != 0) {
        if (Contains(seen_strands, strand)) continue;
        seen_strands.push_back(strand);
      }
      if (entry->not_before_ns > now) {
        earliest_retry = std::min(earliest_retry, entry->not_before_ns);
        continue;
      }
      const bool is_urgent = entry->desc.job_class == JobClass::kUrgent ||
                             (strand != 0 && Contains(urgent_strands, strand));
      if (is_urgent) {
        ++urgent_ready;
        if (urgent == nullptr) urgent = entry.get();
      } else if (background == nullptr) {
        background = entry.get();
      }
    }
    if (urgent != nullptr) {
      --urgent_ready;
      step_class = JobClass::kUrgent;
      return urgent;
    }
    step_class = JobClass::kBackground;
    return background;
  }

  void ApplyQos(JobClass step_class) {
    const Qos wanted = step_class == JobClass::kUrgent ? Qos::kHigh : Qos::kEco;
    if (wanted == qos) return;
    qos = wanted;
    SetCurrentThreadQos(wanted);
    Bump(stats.qos_switches);
  }

  void Record(const Entry& entry, JobClass step_class, std::uint32_t ready,
              std::uint64_t start, std::uint64_t duration) {
    if (trace.size() >= trace_capacity) return;
    LaneStepRecord record;
    record.ticket = entry.ticket;
    record.strand = entry.desc.strand;
    record.start_ns = start - start_ns;
    record.duration_ns = duration;
    record.step_class = step_class;
    record.job_class = entry.desc.job_class;
    record.durable = entry.desc.durable;
    record.urgent_ready = ready;
    const std::string_view kind = entry.job->Kind();
    const std::size_t size = std::min(kind.size(), record.kind.size() - 1);
    std::memcpy(record.kind.data(), kind.data(), size);
    trace.push_back(record);
  }

  void Queue(LaneCompletion* completion) {
    if (held.empty() && lane->completions_.TryPush(completion)) {
      pushed_since_wake = true;
      return;
    }
    held.push_back(completion);
    Bump(stats.completions_held);
  }

  void FlushHeld() {
    while (!held.empty() && lane->completions_.TryPush(held.front())) {
      held.pop_front();
      pushed_since_wake = true;
    }
  }

  void WakeMainIfPushed() {
    if (!pushed_since_wake) return;
    pushed_since_wake = false;
    Bump(stats.wakes);
    if (lane->config_.wake_main != nullptr) {
      lane->config_.wake_main();
    } else {
      WakeMain();
    }
  }

  // The entry finished (done or failed): its completion goes to main.
  void Finish(std::size_t index, std::expected<LanePayload, Error> result) {
    std::unique_ptr<Entry> entry = std::move(queue[index]);
    queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(index));
    const bool ok = result.has_value();
    auto completion = std::make_unique<LaneCompletion>();
    completion->ticket = entry->ticket;
    completion->desc = entry->desc;
    completion->kind = std::string(entry->job->Kind());
    completion->result = std::move(result);
    completion->steps = entry->steps;
    completion->step_ns = entry->step_ns;
    entry->job.reset();
    Queue(completion.release());
    Bump(ok ? stats.completed : stats.failed);
    if (entry->desc.durable) {
      --durable_queued;
      if (!ok) {
        lane->durable_failures_.fetch_add(1, std::memory_order_acq_rel);
      }
      lane->durable_pending_.fetch_sub(1, std::memory_order_acq_rel);
    }
  }

  void RunStep(Entry& entry, JobClass step_class, std::uint32_t ready) {
    ApplyQos(step_class);
    current = &entry;
    LaneContext context(this);
    const std::uint64_t start = SteadyNowNs();
    const std::uint64_t stop_at =
        stats.stop_requested_ns.load(std::memory_order_acquire);
    if (stop_at != 0 && start >= stop_at && !entry.desc.durable) {
      Bump(stats.steps_after_stop);
    }
    std::expected<StepResult, Error> step =
        lane_internal::ContainedStep(*entry.job, context);
    const std::uint64_t end = SteadyNowNs();
    stats.last_step_end_ns.store(end, std::memory_order_relaxed);
    current = nullptr;
    const std::uint64_t duration = end - start;
    ++entry.steps;
    entry.step_ns += duration;
    Bump(stats.steps);
    Bump(step_class == JobClass::kUrgent ? stats.urgent_steps
                                         : stats.background_steps);
    std::uint64_t max = stats.max_step_ns.load(std::memory_order_relaxed);
    while (duration > max && !stats.max_step_ns.compare_exchange_weak(
                                 max, duration, std::memory_order_relaxed)) {
    }
    Record(entry, step_class, ready, start, duration);

    const auto found =
        std::ranges::find_if(queue, [&entry](const std::unique_ptr<Entry>& e) {
          return e.get() == &entry;
        });
    const auto index = static_cast<std::size_t>(found - queue.begin());
    if (!step) {
      Finish(index, std::unexpected(std::move(step.error())));
      return;
    }
    switch (step->status) {
      case StepStatus::kContinue:
        break;
      case StepStatus::kRetryLater:
        entry.not_before_ns = end + step->retry_after_ns;
        Bump(stats.retries);
        break;
      case StepStatus::kDone:
        Finish(index, lane_internal::ContainedResult(*entry.job));
        break;
    }
  }

  void AddWatch(const std::string& path, std::uint64_t tag) {
    const std::string normal = fs::Normalize(path);
    for (Watched& watch : watched) {
      if (watch.path != normal) continue;
      if (!Contains(watch.tags, tag)) watch.tags.push_back(tag);
      return;
    }
    Watched watch;
    watch.path = normal;
    watch.tags.push_back(tag);
    if (auto info = fs::Stat(normal)) watch.info = *info;
    watched.push_back(std::move(watch));
  }

  void RemoveWatch(const std::string& path, std::uint64_t tag) {
    const std::string normal = fs::Normalize(path);
    for (std::size_t i = 0; i < watched.size(); ++i) {
      Watched& watch = watched[i];
      if (watch.path != normal) continue;
      std::erase(watch.tags, tag);
      if (watch.tags.empty()) {
        watched.erase(watched.begin() + static_cast<std::ptrdiff_t>(i));
        scan_cursor = 0;
      }
      return;
    }
  }

  static bool SameInfo(const fs::PathInfo& a, const fs::PathInfo& b) {
    return a.type == b.type && a.size == b.size &&
           a.modify_time_ns == b.modify_time_ns;
  }

  // One step of the hot-reload scan: compares the size and modification
  // time of up to kScanFilesPerStep watched files. A change is reported
  // when two scans in a row see the same new size and time (ADR-094), so a
  // file caught between its truncation and its last write waits for the
  // write to finish.
  void ScanStep() {
    ApplyQos(JobClass::kBackground);
    Bump(stats.scans);
    const std::size_t count = std::min(kScanFilesPerStep, watched.size());
    for (std::size_t n = 0; n < count; ++n) {
      if (scan_cursor >= watched.size()) scan_cursor = 0;
      Watched& watch = watched[scan_cursor++];
      fs::PathInfo info;
      if (auto stat = fs::Stat(watch.path)) info = *stat;
      if (SameInfo(info, watch.info)) {
        watch.has_pending = false;
        continue;
      }
      if (!watch.has_pending || !SameInfo(info, watch.pending)) {
        // New, or still changing: wait for one more scan.
        watch.pending = info;
        watch.has_pending = true;
        continue;
      }
      watch.has_pending = false;
      watch.info = info;
      Bump(stats.changes);
      for (const std::uint64_t tag : watch.tags) {
        auto completion = std::make_unique<LaneCompletion>();
        completion->kind = "scan";
        FileChange change;
        change.path = watch.path;
        change.tag = tag;
        change.exists = info.type == fs::PathType::kFile;
        change.size = info.size;
        change.modify_time_ns = info.modify_time_ns;
        completion->result = LanePayload(std::move(change));
        Queue(completion.release());
      }
    }
  }
};

// --- LaneContext ---

bool LaneContext::StopRequested() const noexcept { return state_->Stopping(); }

std::uint64_t LaneContext::NowNs() const noexcept { return SteadyNowNs(); }

const JobDesc& LaneContext::Desc() const noexcept {
  return state_->current->desc;
}

Ticket LaneContext::CurrentTicket() const noexcept {
  return state_->current->ticket;
}

std::uint64_t LaneContext::CommittedRevision(std::uint64_t strand) const {
  const auto found = state_->committed.find(strand);
  return found == state_->committed.end() ? 0 : found->second;
}

void LaneContext::CommitRevision(std::uint64_t strand, std::uint64_t revision) {
  std::uint64_t& committed = state_->committed[strand];
  committed = std::max(committed, revision);
}

void LaneContext::Watch(const std::string& path, std::uint64_t tag) {
  state_->AddWatch(path, tag);
}

void LaneContext::Unwatch(const std::string& path, std::uint64_t tag) {
  state_->RemoveWatch(path, tag);
}

// --- BackgroundLane ---

BackgroundLane::BackgroundLane(const LaneConfig& config)
    : config_(config),
      requests_(std::bit_ceil(std::max(config.request_capacity, 2U))),
      completions_(std::bit_ceil(std::max(config.completion_capacity, 2U))),
      state_(std::make_unique<LaneState>(this)) {
  state_->trace_capacity = config.trace_capacity;
  state_->trace.reserve(config.trace_capacity);
}

std::expected<std::unique_ptr<BackgroundLane>, Error> BackgroundLane::Start(
    const LaneConfig& config) {
  std::unique_ptr<BackgroundLane> lane(new BackgroundLane(config));
  BackgroundLane* raw = lane.get();
  auto thread =
      StartThread(ThreadSpec{.role = ThreadRole::kBackground,
                             .name = config.thread_name,
                             .qos = Qos::kDefault},
                  [raw](const std::stop_token& stop) { raw->Run(stop); });
  if (!thread) return std::unexpected(std::move(thread.error()));
  lane->thread_ = std::move(*thread);
  return lane;
}

BackgroundLane::~BackgroundLane() {
  Stop();
  // Whatever main never took.
  while (std::optional<LaneCompletion*> completion = completions_.TryPop()) {
    delete *completion;
  }
  for (Request* request : queued_submits_) delete request;
  queued_submits_.clear();
}

void BackgroundLane::Wake() noexcept {
  // At most one release is outstanding: the lane clears the flag only
  // after an acquire, so the semaphore's count never exceeds 1.
  if (!wake_pending_.exchange(true, std::memory_order_acq_rel)) {
    semaphore_.release();
  }
}

Ticket BackgroundLane::Submit(const JobDesc& desc,
                              std::unique_ptr<LaneJob> job) {
  SE_ASSERT_MAIN_THREAD();
  SE_ASSERT_MSG(job != nullptr, "BackgroundLane::Submit needs a job");
  const Ticket ticket = next_ticket_++;
  ++submitted_;
  if (desc.durable) durable_pending_.fetch_add(1, std::memory_order_acq_rel);
  auto* request = new Request{ticket, desc, std::move(job)};
  if (!thread_.Joinable()) {
    // Stopped: nothing will run it. A Durable job counts as failed.
    if (desc.durable) {
      durable_failures_.fetch_add(1, std::memory_order_acq_rel);
      durable_pending_.fetch_sub(1, std::memory_order_acq_rel);
    }
    delete request;
    return ticket;
  }
  FlushQueuedSubmits();
  if (!queued_submits_.empty() || !requests_.TryPush(request)) {
    queued_submits_.push_back(request);
    ++submits_queued_;
  }
  Wake();
  return ticket;
}

void BackgroundLane::FlushQueuedSubmits() {
  bool pushed = false;
  while (!queued_submits_.empty() &&
         requests_.TryPush(queued_submits_.front())) {
    queued_submits_.pop_front();
    pushed = true;
  }
  if (pushed) Wake();
}

void BackgroundLane::Stop() noexcept {
  if (!thread_.Joinable()) return;
  // Submits still queued on main (the request ring was full): the ones
  // that are not Durable are cancelled here, as the lane cancels its own
  // at the stop. Durable jobs are never cancelled, so those go in, in
  // order, waiting for room while the lane, which pulls the ring after
  // every step until it is asked to stop, makes some. This is a shutdown
  // join, where main may wait.
  std::uint64_t cancelled = 0;
  std::erase_if(queued_submits_, [&cancelled](Request* request) {
    if (request->desc.durable) return false;
    delete request;
    ++cancelled;
    return true;
  });
  state_->stats.cancelled.fetch_add(cancelled, std::memory_order_relaxed);
  while (!queued_submits_.empty()) {
    if (requests_.TryPush(queued_submits_.front())) {
      queued_submits_.pop_front();
      continue;
    }
    Wake();
    std::this_thread::yield();
  }
  thread_.RequestStop();
  thread_.Join();
}

LaneStats BackgroundLane::Stats() const noexcept {
  const AtomicStats& s = state_->stats;
  LaneStats stats;
  stats.submitted = submitted_;
  stats.submits_queued = submits_queued_;
  stats.completed = s.completed.load(std::memory_order_relaxed);
  stats.failed = s.failed.load(std::memory_order_relaxed);
  stats.cancelled = s.cancelled.load(std::memory_order_relaxed);
  stats.steps = s.steps.load(std::memory_order_relaxed);
  stats.urgent_steps = s.urgent_steps.load(std::memory_order_relaxed);
  stats.background_steps = s.background_steps.load(std::memory_order_relaxed);
  stats.max_step_ns = s.max_step_ns.load(std::memory_order_relaxed);
  stats.retries = s.retries.load(std::memory_order_relaxed);
  stats.scans = s.scans.load(std::memory_order_relaxed);
  stats.changes = s.changes.load(std::memory_order_relaxed);
  stats.qos_switches = s.qos_switches.load(std::memory_order_relaxed);
  stats.completions_held = s.completions_held.load(std::memory_order_relaxed);
  stats.completions_dropped =
      s.completions_dropped.load(std::memory_order_relaxed);
  stats.wakes = s.wakes.load(std::memory_order_relaxed);
  stats.steps_after_stop = s.steps_after_stop.load(std::memory_order_relaxed);
  const std::uint64_t stop_at =
      s.stop_requested_ns.load(std::memory_order_acquire);
  const std::uint64_t exit_at = s.exit_ns.load(std::memory_order_relaxed);
  const std::uint64_t last = s.last_step_end_ns.load(std::memory_order_relaxed);
  if (stop_at != 0 && exit_at >= stop_at) {
    stats.stop_to_exit_ns = exit_at - stop_at;
    stats.stop_to_last_step_ns = last > stop_at ? last - stop_at : 0;
  }
  return stats;
}

std::span<const LaneStepRecord> BackgroundLane::StepTrace() const noexcept {
  return state_->trace;
}

std::vector<LaneStepRecord> BackgroundLane::TakeStepTrace() noexcept {
  return std::exchange(state_->trace, {});
}

std::expected<void, Error> BackgroundLane::WriteStepTrace(
    std::string_view path) const {
  return WriteLaneStepTrace(state_->trace, path);
}

std::expected<void, Error> WriteLaneStepTrace(
    std::span<const LaneStepRecord> trace, std::string_view path) {
  std::string text =
      "# lane step trace: start_us duration_us ticket kind job_class "
      "step_class durable strand urgent_ready\n";
  for (const LaneStepRecord& record : trace) {
    text += std::format("{:.1f} {:.1f} {} {} {} {} {} {:016x} {}\n",
                        static_cast<double>(record.start_ns) * 1e-3,
                        static_cast<double>(record.duration_ns) * 1e-3,
                        record.ticket, std::string_view(record.kind.data()),
                        JobClassName(record.job_class),
                        JobClassName(record.step_class), record.durable ? 1 : 0,
                        record.strand, record.urgent_ready);
  }
  const auto* data = reinterpret_cast<const std::byte*>(text.data());
  return fs::WriteFile(path, std::span<const std::byte>(data, text.size()));
}

void BackgroundLane::Run(const std::stop_token& stop) {
  LaneState& state = *state_;
  state.stop = stop;
  state.start_ns = SteadyNowNs();
  state.next_scan_ns = state.start_ns + config_.scan_period_ns;
  const std::stop_callback wake_on_stop(stop, [this, &state] {
    state.stats.stop_requested_ns.store(SteadyNowNs(),
                                        std::memory_order_release);
    Wake();
  });
  while (true) {
    state.PullRequests();
    const bool stopping = state.Stopping();
    if (stopping && !state.cancelled_at_stop) {
      state.CancelNonDurable();
      state.cancelled_at_stop = true;
    }
    state.FlushHeld();
    state.WakeMainIfPushed();

    std::uint64_t now = SteadyNowNs();
    JobClass step_class = JobClass::kBackground;
    std::uint32_t urgent_ready = 0;
    std::uint64_t earliest_retry = UINT64_MAX;
    LaneState::Entry* next =
        state.Pick(now, step_class, urgent_ready, earliest_retry);
    const bool scan_due =
        !stopping && config_.scan_period_ns != 0 && now >= state.next_scan_ns;
    // The scan is Background work: Urgent steps go first.
    if (scan_due && (next == nullptr || step_class != JobClass::kUrgent)) {
      if (!state.watched.empty()) state.ScanStep();
      state.next_scan_ns = SteadyNowNs() + config_.scan_period_ns;
      continue;
    }
    if (next != nullptr) {
      SE_PROFILE_ZONE("lane step");
      state.RunStep(*next, step_class, urgent_ready);
      continue;
    }
    if (stopping && state.durable_queued == 0) {
      // A Durable request pushed before the stop may not have been seen
      // yet: the stop request synchronizes with this check, so one more
      // pull finds it.
      state.PullRequests();
      if (state.durable_queued == 0) break;
      continue;
    }

    std::uint64_t deadline = earliest_retry;
    if (!stopping && config_.scan_period_ns != 0) {
      deadline = std::min(deadline, state.next_scan_ns);
    }
    if (!state.held.empty()) deadline = std::min(deadline, now + kHeldRetryNs);
    now = SteadyNowNs();
    const std::uint64_t wait_ns = deadline == UINT64_MAX
                                      ? 3'600'000'000'000ULL
                                      : (deadline > now ? deadline - now : 0);
    if (wait_ns == 0) continue;
    if (semaphore_.try_acquire_for(std::chrono::nanoseconds(wait_ns))) {
      // Synchronizes with the Wake() that released (an RMW reads the
      // newest flag value), so the pushes before it are visible.
      wake_pending_.exchange(false, std::memory_order_acq_rel);
    }
  }
  // Main is shutting down: what is left in the ring and on the lane is
  // dropped (a Durable job's completion too; its file is in place).
  state.PullRequests();
  state.FlushHeld();
  for (LaneCompletion* completion : state.held) {
    delete completion;
    Bump(state.stats.completions_dropped);
  }
  state.held.clear();
  state.queue.clear();
  state.stats.exit_ns.store(SteadyNowNs(), std::memory_order_relaxed);
  // Last: SDL's thread-local data from se::fs calls (its error buffer).
  fs::ReleaseThreadState();
}

}  // namespace se
