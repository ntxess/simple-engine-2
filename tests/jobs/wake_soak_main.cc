// Copyright 2026 The simple-engine-2 Authors.
//
// se_wake_soak: the fork pool's wake-stall soak (docs/milestone-specs.md
// M14; docs/design/mt-design.md "Measurements", unexplained wake stalls;
// ADR-110).
//
//   se_wake_soak [--strict] [--seconds N] [--workers W] [--max-wake-ms MS]
//                [--loaded-busy F] [--json FILE]
//
// A pool of W workers (default: the machine's W_max) on se_os's
// ForkWorkers runs for N seconds (600 by default): a frame every 8.33 ms on
// a precise timer (120 Hz ticks), each with four back-to-back forks of
// 4 (W + 1) chunks of 25 us of busy work, so the workers park between
// frames and the first fork of each frame wakes them from parked. Every
// wake's latency (the caller's bump stamp to the worker seeing it; both
// steady_clock, which is QueryPerformanceCounter on Windows) is recorded
// with its stamps, every join's duration, and the machine's busy share in
// each 100 ms window (GetSystemTimes; /proc/stat).
//
// Beside the pool runs a canary: a thread of the workers' kind (Qos::kHigh,
// the cores the workers prefer) that shares nothing with the pool, sleeps
// 1 ms at a time on its own precise timer and records how late each of its
// wakes was. Its usual lateness is the timer's granularity (about 0.5 ms
// on Windows' high-resolution timers), so it counts as late 1 ms above its
// median of the run. A worker's wake that stalls while the canary is late
// too was not delayed by the wake protocol: ready threads of that kind
// were not being run at that instant (other load on their cores, or the
// system stalled).
//
// It prints (and writes as JSON) the wakes, their p50, p99, p99.9 and max,
// the same for the windows in which the machine had idle cores (busy below
// --loaded-busy, 0.9 by default), the wakes over 1 ms and over the bound,
// the missed and late wakes, the join p50, p99.9 and max, the canary's
// wakes and their p99.9 and max lateness, and every wake over the bound
// with its time from the start, worker, latency, the window's busy share,
// the canary's lateness at that instant and how many other stalls came
// within 1 ms of it (the design's stalls hit several threads "at the same
// instant").
//
// Exit status: 0 when every wake over the bound (2 ms by default) is
// explained (with --strict: when there is none at all), 1 otherwise, 2 for
// bad arguments. A stall is explained when the machine was saturated in its
// window or the canary was late with it (ADR-110); a worker that woke late
// beside idle cores while the canary ran on time is not.

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "se_core/jobs.h"
#include "se_core/profile_clock.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_os/cpu_topology.h"
#include "se_os/fork_workers.h"
#include "se_os/precise_wait.h"
#include "se_os/process.h"
#include "se_os/thread.h"
#include "support/test_roles.h"

namespace se {
namespace {

struct Args {
  double seconds = 600.0;
  int workers = -1;
  double max_wake_ms = 2.0;
  // A window this busy or busier counts as saturated.
  double loaded_busy = 0.9;
  // Fail on any wake over the bound, saturated or not.
  bool strict = false;
  std::string json;
};

std::optional<Args> Parse(int argc, char** argv) {
  Args args;
  const std::vector<std::string_view> tokens(argv + 1, argv + argc);
  std::size_t i = 0;
  for (; i < tokens.size() && tokens[i] == "--strict"; ++i) args.strict = true;
  for (; i + 1 < tokens.size(); i += 2) {
    const std::string_view flag = tokens[i];
    const std::string_view value = tokens[i + 1];
    const char* first = value.data();
    const char* last = value.data() + value.size();
    std::errc error{};
    if (flag == "--seconds") {
      error = std::from_chars(first, last, args.seconds).ec;
    } else if (flag == "--workers") {
      error = std::from_chars(first, last, args.workers).ec;
    } else if (flag == "--max-wake-ms") {
      error = std::from_chars(first, last, args.max_wake_ms).ec;
    } else if (flag == "--loaded-busy") {
      error = std::from_chars(first, last, args.loaded_busy).ec;
    } else if (flag == "--json") {
      args.json = value;
    } else {
      return std::nullopt;
    }
    if (error != std::errc()) return std::nullopt;
  }
  if (i != tokens.size() || args.seconds <= 0.0) return std::nullopt;
  return args;
}

struct Wake {
  std::uint64_t bump_ns = 0;
  std::uint64_t woke_ns = 0;
};

// One preallocated array per worker, each written only by its worker (no
// allocation on the worker), read after the pool joined them.
class SoakTrace final : public jobs::WakeTrace {
 public:
  SoakTrace(int workers, std::size_t capacity)
      : wakes_(static_cast<std::size_t>(workers), std::vector<Wake>(capacity)),
        counts_(static_cast<std::size_t>(workers), 0),
        dropped_(static_cast<std::size_t>(workers), 0) {}

  void Record(int worker, std::uint64_t bump_ns,
              std::uint64_t woke_ns) noexcept override {
    const auto w = static_cast<std::size_t>(worker);
    if (counts_[w] == wakes_[w].size()) {
      ++dropped_[w];
      return;
    }
    wakes_[w][counts_[w]++] = Wake{bump_ns, woke_ns};
  }

  [[nodiscard]] std::size_t Workers() const { return wakes_.size(); }
  [[nodiscard]] std::span<const Wake> WakesOf(std::size_t worker) const {
    return {wakes_[worker].data(), counts_[worker]};
  }
  [[nodiscard]] std::uint64_t Dropped() const {
    std::uint64_t total = 0;
    for (const std::uint64_t d : dropped_) total += d;
    return total;
  }

 private:
  std::vector<std::vector<Wake>> wakes_;
  std::vector<std::size_t> counts_;
  std::vector<std::uint64_t> dropped_;
};

// The machine's busy share in each 100 ms window of the run.
class LoadWindows {
 public:
  static constexpr std::uint64_t kWindowNs = 100'000'000;

  explicit LoadWindows(std::uint64_t start_ns)
      : start_ns_(start_ns), last_(ReadSystemCpuTimes()) {}

  // Closes every window that ended by |now_ns|.
  void Sample(std::uint64_t now_ns) {
    if (now_ns - start_ns_ < (busy_.size() + 1) * kWindowNs) return;
    const std::optional<SystemCpuTimes> times = ReadSystemCpuTimes();
    double busy = -1.0;
    if (times && last_ && times->total_ns > last_->total_ns) {
      const auto total = static_cast<double>(times->total_ns - last_->total_ns);
      const auto idle = static_cast<double>(times->idle_ns - last_->idle_ns);
      busy = std::clamp(1.0 - idle / total, 0.0, 1.0);
    }
    last_ = times;
    // A late sample closes the windows it skipped with the same share.
    while (now_ns - start_ns_ >= (busy_.size() + 1) * kWindowNs) {
      busy_.push_back(busy);
    }
  }
  // The busy share of the window holding |at_ns| after the start (-1:
  // unknown).
  [[nodiscard]] double BusyAt(std::uint64_t at_ns) const {
    const auto i = static_cast<std::size_t>(at_ns / kWindowNs);
    return i < busy_.size() ? busy_[i] : -1.0;
  }
  [[nodiscard]] double Mean() const {
    double sum = 0.0;
    int n = 0;
    for (const double b : busy_) {
      if (b < 0.0) continue;
      sum += b;
      ++n;
    }
    return n > 0 ? sum / n : -1.0;
  }

 private:
  std::uint64_t start_ns_;
  std::optional<SystemCpuTimes> last_;
  std::vector<double> busy_;
};

// The canary thread (see the file comment). Its samples are written by the
// canary alone and read after Stop() joined it.
class Canary {
 public:
  static constexpr std::uint64_t kPeriodNs = 1'000'000;
  // A canary wake this much later than the run's median lateness, or more,
  // explains a worker's stall with it.
  static constexpr std::uint64_t kLateOverMedianNs = 1'000'000;

  struct Sample {
    std::uint64_t due_ns = 0;
    std::uint64_t woke_ns = 0;
  };

  explicit Canary(std::size_t capacity) { samples_.reserve(capacity); }

  // False when the thread could not be started (the soak then runs without
  // it and explains stalls by the load alone).
  bool Start() {
    ThreadSpec spec;
    spec.role = ThreadRole::kForkWorker;
    spec.name = "wake-canary";
    spec.qos = Qos::kHigh;
    if (auto topology = ReadCpuTopology()) {
      spec.cpus = WorkerCpuSet(*topology, jobs::CoreClass::kAnyPhysical);
    }
    auto thread =
        StartThread(spec, [this](const std::stop_token& stop) { Run(stop); });
    if (!thread) return false;
    thread_ = std::move(*thread);
    return true;
  }

  void Stop() {
    if (!thread_.Joinable()) return;
    thread_.RequestStop();
    thread_.Join();
  }

  [[nodiscard]] std::span<const Sample> Samples() const { return samples_; }

  // The canary's largest lateness among its wakes that overlap
  // [from_ns, to_ns]; 0 when none does.
  [[nodiscard]] std::uint64_t LatenessDuring(std::uint64_t from_ns,
                                             std::uint64_t to_ns) const {
    // Samples are in time order: the first that woke at or after |from_ns|.
    auto it = std::lower_bound(samples_.begin(), samples_.end(), from_ns,
                               [](const Sample& sample, std::uint64_t at) {
                                 return sample.woke_ns < at;
                               });
    std::uint64_t worst = 0;
    for (; it != samples_.end() && it->due_ns <= to_ns; ++it) {
      worst = std::max(worst, it->woke_ns - it->due_ns);
    }
    return worst;
  }

 private:
  void Run(const std::stop_token& stop) {
    auto timer = PreciseTimer::Create();
    if (!timer) return;
    std::uint64_t due = MonotonicNowNs() + kPeriodNs;
    while (!stop.stop_requested() && samples_.size() < samples_.capacity()) {
      timer->SleepUntil(due);
      const std::uint64_t woke = MonotonicNowNs();
      // An early return (a signal on Linux) is not a sample.
      if (woke < due) continue;
      samples_.push_back({due, woke});
      // The next deadline is a period after this wake, so one late wake is
      // one late sample.
      due = woke + kPeriodNs;
    }
  }

  std::vector<Sample> samples_;
  Thread thread_;
};

struct Stall {
  std::uint64_t at_ns = 0;
  int worker = 0;
  std::uint64_t latency_ns = 0;
  double busy = -1.0;
  // The canary's lateness while this wake waited.
  std::uint64_t canary_ns = 0;
  int together = 0;
};

void BusyFor(std::uint64_t ns) {
  const std::uint64_t start = ProfileNowNs();
  while (ProfileNowNs() - start < ns) {
  }
}

std::uint64_t Nth(const std::vector<std::uint64_t>& sorted, double p) {
  if (sorted.empty()) return 0;
  const auto n = static_cast<double>(sorted.size());
  const auto rank = static_cast<std::size_t>(std::clamp(p * n, 1.0, n));
  return sorted[rank - 1];
}

std::uint64_t Max(const std::vector<std::uint64_t>& sorted) {
  return sorted.empty() ? 0 : sorted.back();
}

double Ms(std::uint64_t ns) { return static_cast<double>(ns) * 1e-6; }

const jobs::ForkSiteDecl& SoakSite(int k) {
  static const jobs::ForkSiteDecl kSites[4] = {{.name = "soak.a", .grain = 1},
                                               {.name = "soak.b", .grain = 1},
                                               {.name = "soak.c", .grain = 1},
                                               {.name = "soak.d", .grain = 1}};
  return kSites[k];
}

struct Measured {
  int workers = 0;
  std::uint32_t chunks = 0;
  std::uint64_t start_ns = 0;
  std::uint64_t frames = 0;
  std::uint64_t threaded = 0;
  std::uint64_t missed = 0;
  std::uint64_t late = 0;
  std::uint64_t unstamped = 0;
  bool canary = false;
  std::vector<std::uint64_t> joins;
  std::optional<LoadWindows> load;
};

// Runs the frames; the trace and the load windows fill as it goes.
std::optional<Measured> Soak(const Args& args, SoakTrace& trace, Canary& canary)
    SE_REQUIRES(g_main) {
  constexpr std::uint64_t kFrameNs = 8'333'333;
  constexpr std::uint64_t kChunkNs = 25'000;
  constexpr int kForksPerFrame = 4;
  jobs::ForkJoinPool::Config config;
  config.workers = args.workers;
  config.threads = ForkWorkersFactory();
  config.enabled_sites = {"*"};
  config.fallback_factor = 0;
  config.wake_trace = &trace;
  std::optional<jobs::ForkJoinPool> pool;
  pool.emplace(config);
  Measured out;
  {
    const InlineOwnerScope owner(OnMainThread());
    if (!pool->StartWorkers()) {
      std::fputs("se_wake_soak: no fork workers on this machine\n", stderr);
      return std::nullopt;
    }
    out.workers = pool->ActiveWorkers();
  }
  out.chunks = static_cast<std::uint32_t>(4 * (out.workers + 1));
  const auto frames =
      static_cast<std::uint64_t>(args.seconds * 1e9 / kFrameNs) + 1;
  out.joins.reserve(static_cast<std::size_t>(frames * kForksPerFrame));
  auto timer = PreciseTimer::Create();
  if (!timer) {
    std::fputs("se_wake_soak: no precise timer\n", stderr);
    return std::nullopt;
  }
  out.canary = canary.Start();
  if (!out.canary) {
    std::fputs(
        "se_wake_soak: no canary thread; stalls are judged by the machine's "
        "load alone\n",
        stderr);
  }
  // Wakes before this (the startup calibration's) are not the soak's.
  out.start_ns = MonotonicNowNs();
  out.load.emplace(out.start_ns);
  const std::uint64_t end =
      out.start_ns + static_cast<std::uint64_t>(args.seconds * 1e9);
  std::uint64_t next = out.start_ns;
  while (MonotonicNowNs() < end) {
    {
      const InlineOwnerScope owner(OnMainThread());
      for (int k = 0; k < kForksPerFrame; ++k) {
        jobs::ForkSite& site = pool->Site(SoakSite(k));
        const std::uint64_t fork_start = ProfileNowNs();
        pool->ParallelFor(site, out.chunks,
                          [](jobs::ChunkRange) { BusyFor(kChunkNs); });
        out.joins.push_back(ProfileNowNs() - fork_start);
      }
    }
    ++out.frames;
    next += kFrameNs;
    timer->SleepUntil(next);
    out.load->Sample(MonotonicNowNs());
  }
  for (const jobs::ForkSite* site : pool->Sites()) {
    out.missed += site->stats.missed_wakes;
    out.late += site->stats.late_wakes;
    out.threaded += site->stats.threaded_forks;
  }
  out.unstamped = pool->Stats().unstamped_wakes;
  // Joins the workers and the canary: the traces are complete after this.
  pool.reset();
  canary.Stop();
  return out;
}

int Run(const Args& args) SE_REQUIRES(g_main) {
  constexpr std::size_t kStallsPrinted = 40;
  constexpr std::uint64_t kSameInstantNs = 1'000'000;
  constexpr std::uint64_t kOneMs = 1'000'000;
  const auto frames = static_cast<std::uint64_t>(args.seconds * 120.0) + 1;
  SoakTrace trace(jobs::kMaxWorkers,
                  static_cast<std::size_t>(frames * 4 + 1024));
  Canary canary(static_cast<std::size_t>(args.seconds * 1000.0) + 4096);
  std::optional<Measured> measured = Soak(args, trace, canary);
  if (!measured) {
    canary.Stop();
    return 1;
  }
  Measured& m = *measured;
  const LoadWindows* load = &*m.load;
  std::vector<std::uint64_t> canary_late;
  for (const Canary::Sample& sample : canary.Samples()) {
    if (sample.due_ns < m.start_ns) continue;
    canary_late.push_back(sample.woke_ns - sample.due_ns);
  }
  std::sort(canary_late.begin(), canary_late.end());
  // Late for the canary: 1 ms over its median (the timer's granularity).
  const std::uint64_t canary_late_ns =
      Nth(canary_late, 0.5) + Canary::kLateOverMedianNs;
  const auto canary_over = static_cast<std::uint64_t>(
      canary_late.end() -
      std::lower_bound(canary_late.begin(), canary_late.end(), canary_late_ns));

  const auto bound_ns = static_cast<std::uint64_t>(args.max_wake_ms * 1e6);
  std::vector<std::uint64_t> latencies;
  // Wakes in windows where the machine had idle cores: a stall there is not
  // explained by the load.
  std::vector<std::uint64_t> spare;
  std::vector<Stall> stalls;
  std::uint64_t over_1ms = 0;
  for (std::size_t w = 0; w < trace.Workers(); ++w) {
    for (const Wake& wake : trace.WakesOf(w)) {
      if (wake.bump_ns < m.start_ns) continue;
      const std::uint64_t latency =
          wake.woke_ns > wake.bump_ns ? wake.woke_ns - wake.bump_ns : 0;
      const double busy = load->BusyAt(wake.bump_ns - m.start_ns);
      latencies.push_back(latency);
      if (busy >= 0.0 && busy < args.loaded_busy) spare.push_back(latency);
      if (latency > kOneMs) ++over_1ms;
      if (latency > bound_ns) {
        stalls.push_back(
            {wake.bump_ns - m.start_ns, static_cast<int>(w), latency, busy,
             canary.LatenessDuring(wake.bump_ns, wake.woke_ns), 0});
      }
    }
  }
  std::sort(latencies.begin(), latencies.end());
  std::sort(spare.begin(), spare.end());
  std::sort(m.joins.begin(), m.joins.end());
  std::sort(stalls.begin(), stalls.end(),
            [](const Stall& a, const Stall& b) { return a.at_ns < b.at_ns; });
  // The other stalls within 1 ms of each.
  for (std::size_t i = 0, lo = 0, hi = 0; i < stalls.size(); ++i) {
    while (stalls[i].at_ns - stalls[lo].at_ns > kSameInstantNs) ++lo;
    while (hi < stalls.size() &&
           stalls[hi].at_ns - stalls[i].at_ns <= kSameInstantNs) {
      ++hi;
    }
    stalls[i].together = static_cast<int>(hi - lo) - 1;
  }
  // A stall is explained by a saturated machine (or a load that could not
  // be read), or by the canary waking late with it.
  std::size_t loaded_stalls = 0;
  std::size_t canary_stalls = 0;
  std::size_t unexplained = 0;
  for (const Stall& stall : stalls) {
    if (stall.busy < 0.0 || stall.busy >= args.loaded_busy) {
      ++loaded_stalls;
    } else if (m.canary && stall.canary_ns >= canary_late_ns) {
      ++canary_stalls;
    } else {
      ++unexplained;
    }
  }

  std::string report = std::format(
      "se_wake_soak: {:.0f} s, W = {}, {} frames, {} threaded forks of {} "
      "chunks of 25 us; the machine {:.0f}% busy on average\n",
      args.seconds, m.workers, m.frames, m.threaded, m.chunks,
      load->Mean() * 100.0);
  report += std::format(
      "  wakes {} (dropped {}, unstamped {}): p50 {:.4f} ms, p99 {:.4f} ms, "
      "p99.9 {:.4f} ms, max {:.4f} ms; over 1 ms {}, over {:.1f} ms {}\n",
      latencies.size(), trace.Dropped(), m.unstamped, Ms(Nth(latencies, 0.5)),
      Ms(Nth(latencies, 0.99)), Ms(Nth(latencies, 0.999)), Ms(Max(latencies)),
      over_1ms, args.max_wake_ms, stalls.size());
  report += std::format(
      "  with idle cores (busy below {:.0f}%): {} wakes, p99.9 {:.4f} ms, "
      "max {:.4f} ms\n",
      args.loaded_busy * 100.0, spare.size(), Ms(Nth(spare, 0.999)),
      Ms(Max(spare)));
  report += std::format(
      "  canary{}: {} wakes, lateness p50 {:.4f} ms, p99.9 {:.4f} ms, max "
      "{:.4f} ms, {} late by {:.4f} ms or more (1 ms over its median)\n",
      m.canary ? "" : " (not running)", canary_late.size(),
      Ms(Nth(canary_late, 0.5)), Ms(Nth(canary_late, 0.999)),
      Ms(Max(canary_late)), canary_over, Ms(canary_late_ns));
  report += std::format(
      "  wakes over the bound: {} while the machine was saturated, {} with "
      "the canary late too, {} unexplained\n",
      loaded_stalls, canary_stalls, unexplained);
  report += std::format(
      "  missed wakes {}, late wakes {}; joins p50 {:.4f} ms, p99.9 {:.4f} "
      "ms, max {:.4f} ms\n",
      m.missed, m.late, Ms(Nth(m.joins, 0.5)), Ms(Nth(m.joins, 0.999)),
      Ms(Max(m.joins)));
  std::string stall_json;
  for (std::size_t i = 0; i < stalls.size(); ++i) {
    const Stall& stall = stalls[i];
    if (i < kStallsPrinted) {
      report += std::format(
          "  stall at {:.6f} s: worker {} woke {:.3f} ms late, the machine "
          "{:.0f}% busy, the canary {:.3f} ms late ({} other stall(s) within "
          "1 ms)\n",
          static_cast<double>(stall.at_ns) * 1e-9, stall.worker,
          Ms(stall.latency_ns), stall.busy * 100.0, Ms(stall.canary_ns),
          stall.together);
    }
    stall_json += std::format(
        "{}{{\"at_s\": {:.6f}, \"worker\": {}, \"latency_ms\": {:.4f}, "
        "\"busy\": {:.3f}, \"canary_late_ms\": {:.4f}, \"together\": {}}}",
        i == 0 ? "" : ",\n    ", static_cast<double>(stall.at_ns) * 1e-9,
        stall.worker, Ms(stall.latency_ns), stall.busy, Ms(stall.canary_ns),
        stall.together);
  }
  if (stalls.size() > kStallsPrinted) {
    report += std::format("  ... {} more stalls in the JSON\n",
                          stalls.size() - kStallsPrinted);
  }
  std::fputs(report.c_str(), stdout);
  if (!args.json.empty()) {
    std::ofstream out(args.json, std::ios::binary);
    out << std::format(
        "{{\n  \"seconds\": {:.1f},\n  \"workers\": {},\n  \"frames\": {},\n"
        "  \"threaded_forks\": {},\n  \"busy_mean\": {:.3f},\n"
        "  \"wakes\": {},\n  \"dropped\": {},\n"
        "  \"wake_p50_ms\": {:.6f},\n  \"wake_p99_ms\": {:.6f},\n"
        "  \"wake_p999_ms\": {:.6f},\n  \"wake_max_ms\": {:.6f},\n"
        "  \"over_1ms\": {},\n  \"bound_ms\": {:.3f},\n  \"over_bound\": {},\n",
        args.seconds, m.workers, m.frames, m.threaded, load->Mean(),
        latencies.size(), trace.Dropped(), Ms(Nth(latencies, 0.5)),
        Ms(Nth(latencies, 0.99)), Ms(Nth(latencies, 0.999)), Ms(Max(latencies)),
        over_1ms, args.max_wake_ms, stalls.size());
    out << std::format(
        "  \"loaded_busy\": {:.2f},\n  \"spare_wakes\": {},\n"
        "  \"spare_p999_ms\": {:.6f},\n  \"spare_max_ms\": {:.6f},\n"
        "  \"canary_wakes\": {},\n  \"canary_p50_ms\": {:.6f},\n"
        "  \"canary_p999_ms\": {:.6f},\n  \"canary_max_ms\": {:.6f},\n"
        "  \"canary_late_ms\": {:.6f},\n  \"canary_late\": {},\n",
        args.loaded_busy, spare.size(), Ms(Nth(spare, 0.999)), Ms(Max(spare)),
        canary_late.size(), Ms(Nth(canary_late, 0.5)),
        Ms(Nth(canary_late, 0.999)), Ms(Max(canary_late)), Ms(canary_late_ns),
        canary_over);
    out << std::format(
        "  \"unexplained_stalls\": {},\n  \"loaded_stalls\": {},\n"
        "  \"canary_stalls\": {},\n"
        "  \"missed_wakes\": {},\n  \"late_wakes\": {},\n"
        "  \"join_p50_ms\": {:.6f},\n  \"join_p999_ms\": {:.6f},\n"
        "  \"join_max_ms\": {:.6f},\n  \"stalls\": [\n    {}\n  ]\n}}\n",
        unexplained, loaded_stalls, canary_stalls, m.missed, m.late,
        Ms(Nth(m.joins, 0.5)), Ms(Nth(m.joins, 0.999)), Ms(Max(m.joins)),
        stall_json);
  }
  return (args.strict ? stalls.empty() : unexplained == 0) ? 0 : 1;
}

}  // namespace
}  // namespace se

int main(int argc, char** argv) {
  // Tool code: a standard-library throw (bad_alloc) ends the run with a
  // message instead of terminate.
  try {
    const std::optional<se::Args> args = se::Parse(argc, argv);
    if (!args) {
      std::fputs(
          "usage: se_wake_soak [--strict] [--seconds N] [--workers W]\n"
          "                    [--max-wake-ms MS] [--loaded-busy F] "
          "[--json FILE]\n",
          stderr);
      return 2;
    }
    const se::testing::MainThreadScope main_thread;
    return se::Run(*args);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "se_wake_soak: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("se_wake_soak: unknown exception\n", stderr);
    return 1;
  }
}
