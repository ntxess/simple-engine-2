// Copyright 2026 The simple-engine-2 Authors.
//
// The threaded ForkJoinPool (docs/design/mt-design.md "Job System", "Data
// Flow" and "Threads"; ADR-107 to ADR-109).
//
// A threaded fork, on the caller (the thread holding g_sim_owner):
//   1. writes the descriptor (invoke, body, count, grain, chunks, the claim
//      order under chaos, its breadcrumb) with plain stores, its full
//      generation and the batch hint, remaining = chunks, then
//      claim.store(pack(gen, chunks, 0), release);
//   2. bumps the wake word of participants - 1 workers, each its own padded
//      word: fetch_add(1, release), then notify_one;
//   3. claims batches itself with fetch_add(batch, acq_rel) on the claim
//      word, like the workers;
//   4. joins: waits until remaining.load(acquire) == 0 (atomic wait; the
//      claimer whose decrement reaches 0 notifies), then rethrows the
//      first exception a chunk threw.
// A worker loops: load its wake word, claim and run batches until the claim
// word has none left, then wait(old) on the wake word, so a bump between
// the load and the wait is never lost (the wait returns at once). Each
// successful claim copies the descriptor when its generation is new, runs
// the batch's chunks inside the fault boundary, then decrements remaining
// as its last access to the fork. After running chunks a worker spins for
// at most spin_us when the caller predicts another fork soon (never on
// battery), watching its wake word and the claim word, then parks.
//
// The claim word packs a 16-bit generation, the range of workers that may
// claim (two 4-bit indices: a PerformanceOnly site's workers, or one
// efficiency class during calibration), the chunk count (20 bits) and the
// next unclaimed index (20 bits). A worker outside the range does not add
// to the word (it checks with a load first); next passes the count by at
// most one batch per claimer, so the fields never overflow into each other
// (kMaxThreadedChunks, kMaxBatch). The generation's wrap is harmless: it
// only names the fork a claimer ran out of, and the descriptor is matched
// by its full 64-bit generation.

#include "se_core/jobs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
#include <immintrin.h>
#define SE_JOBS_X86 1
#endif

#include "se_core/breadcrumb.h"
#include "se_core/hash.h"
#include "se_core/log.h"
#include "se_core/profile.h"
#include "se_core/profile_clock.h"
#include "src/fork_fault_boundary.h"

namespace se::jobs {

ForkChunkRole g_fork_chunk;

// g_fork_chunk is a static token with nothing to lock, so acquiring and
// releasing it are excluded from the analysis (as for InlineOwnerScope).
ForkScope::ForkScope() noexcept SE_NO_THREAD_SAFETY_ANALYSIS {
  t_in_fork = true;
}

ForkScope::~ForkScope() SE_NO_THREAD_SAFETY_ANALYSIS { t_in_fork = previous_; }

namespace {

// The worker index of the calling thread (-1: not a pool worker).
thread_local int t_worker = -1;

constexpr int kChunksShift = 20;
constexpr int kEndShift = 40;
constexpr int kBeginShift = 44;
constexpr int kGenShift = 48;
constexpr std::uint64_t kField4 = 0xF;
constexpr std::uint64_t kField16 = 0xFFFF;
constexpr std::uint64_t kField20 = (std::uint64_t{1} << 20) - 1;
// Forks with more chunks than this run inline (the claim word's fields).
constexpr std::uint32_t kMaxThreadedChunks = std::uint32_t{1} << 19;
constexpr std::uint32_t kMaxBatch = std::uint32_t{1} << 13;
// A batch is sized for about this many claims per participant.
constexpr std::uint32_t kClaimsPerParticipant = 8;
// No 16-bit generation has this value.
constexpr std::uint32_t kNoGeneration = 0xFFFFFFFFU;
// Join percentiles refresh after this many threaded joins of a site, then
// every kRefreshEvery.
constexpr std::uint64_t kFirstRefresh = 32;
constexpr std::uint64_t kRefreshEvery = 64;
constexpr std::uint64_t kBatteryPollNs = 1'000'000'000;
// Chaos: the longest injected delays.
constexpr std::uint64_t kChaosClaimDelayNs = 20'000;
constexpr std::uint64_t kChaosWakeDelayNs = 100'000;
// Calibration: dummy chunks of about this long, this many per worker.
constexpr std::uint64_t kCalibrationChunkNs = 50'000;
constexpr std::uint32_t kCalibrationChunksPerWorker = 200;

constexpr std::uint64_t Pack(std::uint64_t generation, int begin, int end,
                             std::uint32_t chunks,
                             std::uint32_t next) noexcept {
  return ((generation & kField16) << kGenShift) |
         ((static_cast<std::uint64_t>(begin) & kField4) << kBeginShift) |
         ((static_cast<std::uint64_t>(end) & kField4) << kEndShift) |
         (static_cast<std::uint64_t>(chunks) << kChunksShift) | next;
}
constexpr std::uint32_t GenOf(std::uint64_t word) noexcept {
  return static_cast<std::uint32_t>((word >> kGenShift) & kField16);
}
// Whether worker |index| may claim from |word| (the caller always may).
constexpr bool MayClaim(std::uint64_t word, int index) noexcept {
  const auto begin = static_cast<int>((word >> kBeginShift) & kField4);
  const auto end = static_cast<int>((word >> kEndShift) & kField4);
  return index < 0 || (index >= begin && index < end);
}
constexpr std::uint32_t ChunksOf(std::uint64_t word) noexcept {
  return static_cast<std::uint32_t>((word >> kChunksShift) & kField20);
}
constexpr std::uint32_t NextOf(std::uint64_t word) noexcept {
  return static_cast<std::uint32_t>(word & kField20);
}

inline void SpinPause() noexcept {
#if defined(SE_JOBS_X86)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}

// A counter only its one writer changes: a load and a store.
inline void Bump(std::atomic<std::uint64_t>& counter,
                 std::uint64_t by = 1) noexcept {
  counter.store(counter.load(std::memory_order_relaxed) + by,
                std::memory_order_relaxed);
}

}  // namespace

namespace pool_internal {

// SplitMix64 steps for chaos decisions (never for results).
class ChaosRng {
 public:
  explicit ChaosRng(std::uint64_t seed) noexcept : state_(seed) {}
  std::uint64_t Next() noexcept {
    state_ += kHashGamma;
    return Mix64(state_);
  }
  bool OneIn(std::uint32_t n) noexcept { return Next() % n == 0; }
  std::uint64_t Below(std::uint64_t n) noexcept {
    return n == 0 ? 0 : Next() % n;
  }

 private:
  std::uint64_t state_;
};

}  // namespace pool_internal

namespace {

using pool_internal::ChaosRng;

// Waits about |ns| on the profiling clock, yielding (chaos only).
void ChaosDelay(ChaosRng& rng, std::uint64_t max_ns) noexcept {
  const std::uint64_t ns = rng.Below(max_ns);
  const std::uint64_t start = ProfileNowNs();
  while (ProfileNowNs() - start < ns) std::this_thread::yield();
}

// The calibration's dummy work: |iterations| xorshift steps.
std::uint64_t DummyWork(std::uint64_t seed, std::uint32_t iterations) noexcept {
  std::uint64_t x = seed | 1U;
  for (std::uint32_t i = 0; i < iterations; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
  }
  return x;
}

struct CalibrationSample {
  std::uint64_t ns = 0;
  int worker = -1;
  std::uint64_t sink = 0;
};

struct CalibrationBody {
  std::uint32_t iterations = 0;
  CalibrationSample* samples = nullptr;
};

void CalibrationChunk(const void* body, ChunkRange range) {
  const auto& calibration = *static_cast<const CalibrationBody*>(body);
  const std::uint64_t start = ProfileNowNs();
  const std::uint64_t sink = DummyWork(range.chunk, calibration.iterations);
  CalibrationSample& sample = calibration.samples[range.chunk];
  sample.ns = ProfileNowNs() - start;
  sample.worker = t_worker;
  sample.sink = sink;
}

std::uint64_t NthNs(const std::vector<std::uint32_t>& sorted, double p) {
  if (sorted.empty()) return 0;
  const auto n = static_cast<double>(sorted.size());
  const auto rank = static_cast<std::size_t>(std::clamp(p * n, 1.0, n));
  return sorted[rank - 1];
}

}  // namespace

// The types the pool's shared state holds: named, since ForkJoinPool::Shared
// (a member of a class with external linkage) holds them.
namespace pool_internal {

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4324)  // padded to cache lines on purpose
#endif

// The caller's descriptor of a threaded fork.
struct Descriptor {
  internal::ChunkInvoke invoke = nullptr;
  const void* body = nullptr;
  std::uint32_t count = 0;
  std::uint32_t grain = 0;
  std::uint32_t chunks = 0;
  // Claim index -> chunk under chaos, or null (claim k runs chunk k).
  const std::uint32_t* order = nullptr;
  // The caller's breadcrumb (tick, phase, system) for the workers'.
  std::uint64_t crumb = 0;
};

// A worker's wake word, alone on its cache line.
struct alignas(64) WakeLine {
  std::atomic<std::uint32_t> word{0};
};

// What the caller writes with each bump.
struct alignas(64) BumpLine {
  // The oldest bump the worker has not seen (0 once it took the stamp).
  std::atomic<std::uint64_t> stamp_ns{0};
  // The generation of the latest bump.
  std::atomic<std::uint64_t> generation{0};
};

// What the worker writes (its 64-byte stats slot and then some).
struct alignas(64) WorkerLine {
  // The last generation it claimed chunks in, and its last wake's
  // generation and latency.
  std::atomic<std::uint64_t> joined{0};
  std::atomic<std::uint64_t> woke{0};
  std::atomic<std::uint64_t> latency_ns{0};
  std::atomic<std::uint64_t> wakes{0};
  std::atomic<std::uint64_t> late{0};
  std::atomic<std::uint64_t> unstamped{0};
  std::atomic<std::uint64_t> latency_max_ns{0};
  std::atomic<std::uint64_t> chunks{0};
  std::atomic<std::uint64_t> spins{0};
  std::atomic<std::uint64_t> spin_hits{0};
  // The wake word value the worker last accounted for.
  std::atomic<std::uint32_t> seen{0};
};

struct Slot {
  WakeLine wake;
  BumpLine bump;
  WorkerLine worker;
  std::uint8_t efficiency_class = 0;
};

// A claimer's private state: the caller's or a worker's.
struct Claimer {
  Claimer(int worker_index, std::uint64_t chaos_seed) noexcept
      : index(worker_index),
        rng(Mix64(chaos_seed ^
                  (static_cast<std::uint64_t>(worker_index + 2) << 32))) {}

  int index;
  Descriptor desc{};
  // The full generation |desc| belongs to.
  std::uint64_t desc_generation = 0;
  // The 24-bit generation whose claims ran out for this claimer.
  std::uint32_t exhausted = kNoGeneration;
  // The wake word value last accounted for.
  std::uint32_t noted_wake = 0;
  ChaosRng rng;
};

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

}  // namespace pool_internal

using pool_internal::Claimer;
using pool_internal::Descriptor;
using pool_internal::Slot;
using pool_internal::WorkerLine;

int CurrentWorker() noexcept { return t_worker; }

void JoinTimes::Record(std::uint64_t ns) noexcept {
  ring_ns_[head_] =
      static_cast<std::uint32_t>(std::min<std::uint64_t>(ns, 0xFFFFFFFFU));
  head_ = static_cast<std::uint32_t>((head_ + 1) % kJoinRingSize);
  count_ = std::min<std::uint32_t>(count_ + 1, kJoinRingSize);
}

void JoinTimes::Sorted(std::vector<std::uint32_t>& out) const {
  out.assign(ring_ns_.begin(), ring_ns_.begin() + count_);
  std::sort(out.begin(), out.end());
}

std::uint64_t JoinTimes::PercentileNs(double p) const {
  std::vector<std::uint32_t> sorted;
  Sorted(sorted);
  return NthNs(sorted, p);
}

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4324)  // padded to cache lines on purpose
#endif

struct ForkJoinPool::Shared {
  Shared(int worker_count, const Config& config)
      : workers(worker_count),
        chaos_seed(config.chaos_seed),
        spin_ns(static_cast<std::uint64_t>(config.spin_us) * 1000),
        now(config.now_ns != nullptr ? config.now_ns : &ProfileNowNs),
        trace(config.wake_trace),
        slots(std::make_unique<Slot[]>(static_cast<std::size_t>(worker_count))),
        caller(-1, config.chaos_seed) {}

  void WorkerLoop(int index);
  // Claims and runs batches until the current fork has none left; returns
  // the chunks run.
  std::uint32_t ClaimAndRun(Claimer& self);
  void RunBatch(Claimer& self, std::uint32_t first, std::uint32_t last);
  // Spins while another fork is predicted; true when one (or a bump)
  // arrived.
  bool Spin(Claimer& self, std::uint32_t seen);
  // Accounts for a wake word that moved past what |self| saw.
  void NoteWake(Claimer& self, std::uint32_t seen) const;

  // Caller side.
  // Publishes a fork that the caller and workers [begin, end) may claim.
  void Publish(const Descriptor& d, std::uint32_t amount, bool hint, int begin,
               int end);
  void Wake(std::span<const int> woken, std::uint64_t now_ns);
  void NotifyDeferred();
  void Join();
  void Stop() noexcept;
  // A seeded permutation of [0, chunks) for the fork of |fork_generation|.
  void ShuffleOrder(std::uint32_t chunks, std::uint64_t fork_generation);

  const int workers;
  const std::uint64_t chaos_seed;
  const std::uint64_t spin_ns;
  std::uint64_t (*const now)();
  WakeTrace* const trace;
  std::unique_ptr<Slot[]> slots;

  // The caller's own state.
  Claimer caller;
  std::uint64_t generation = 0;
  std::uint64_t bumps = 0;
  std::array<int, kMaxWorkers> deferred{};
  int deferred_count = 0;
  std::vector<std::uint32_t> order;

  // The descriptor and the first exception: plain data, written by the
  // caller before the claim store (the exception by the claimer that won
  // error_claimed, before its decrement), read after acquiring them.
  alignas(64) Descriptor desc{};
  std::exception_ptr error;

  alignas(64) std::atomic<std::uint64_t> claim{0};
  std::atomic<std::uint64_t> desc_generation{0};
  std::atomic<std::uint32_t> batch{1};

  alignas(64) std::atomic<std::uint32_t> remaining{0};

  alignas(64) std::atomic<bool> error_claimed{false};
  std::atomic<bool> cancelled{false};
  std::atomic<bool> spin_hint{false};
  std::atomic<bool> stop{false};
  // Where the calibration's dummy work puts its results (see Calibrate).
  std::atomic<std::uint64_t> calibration_sink{0};
};

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

void ForkJoinPool::Shared::WorkerLoop(int index) {
  t_worker = index;
  Claimer self(index, chaos_seed);
  std::atomic<std::uint32_t>& wake = slots[index].wake.word;
  std::uint32_t seen = wake.load(std::memory_order_acquire);
  while (!stop.load(std::memory_order_acquire)) {
    NoteWake(self, seen);
    // An injected late wake: the worker dawdles before it claims.
    if (chaos_seed != 0 && self.rng.OneIn(8)) {
      ChaosDelay(self.rng, kChaosWakeDelayNs);
    }
    const std::uint32_t ran = ClaimAndRun(self);
    if (ran > 0 && Spin(self, seen)) {
      seen = wake.load(std::memory_order_acquire);
      continue;
    }
    wake.wait(seen, std::memory_order_acquire);
    seen = wake.load(std::memory_order_acquire);
  }
  t_worker = -1;
}

void ForkJoinPool::Shared::NoteWake(Claimer& self, std::uint32_t seen) const {
  if (seen == self.noted_wake) return;
  self.noted_wake = seen;
  Slot& slot = slots[self.index];
  WorkerLine& line = slot.worker;
  line.seen.store(seen, std::memory_order_release);
  const std::uint64_t woke = now();
  const std::uint64_t stamp =
      slot.bump.stamp_ns.exchange(0, std::memory_order_acq_rel);
  const std::uint64_t bumped =
      slot.bump.generation.load(std::memory_order_relaxed);
  Bump(line.wakes);
  if (stamp == 0) {
    Bump(line.unstamped);
    return;
  }
  const std::uint64_t latency = woke > stamp ? woke - stamp : 0;
  line.latency_ns.store(latency, std::memory_order_relaxed);
  line.woke.store(bumped, std::memory_order_relaxed);
  if (latency > kLateWakeNs) Bump(line.late);
  if (latency > line.latency_max_ns.load(std::memory_order_relaxed)) {
    line.latency_max_ns.store(latency, std::memory_order_relaxed);
  }
  if (trace != nullptr) trace->Record(self.index, stamp, woke);
}

std::uint32_t ForkJoinPool::Shared::ClaimAndRun(Claimer& self) {
  const bool chaos = chaos_seed != 0;
  const bool is_worker = self.index >= 0;
  std::uint32_t ran = 0;
  while (true) {
    if (chaos && self.rng.OneIn(4)) std::this_thread::yield();
    if (is_worker) {
      // A worker adds only to a word with chunks left in a generation it
      // has not run out of, so next stays within one batch per participant
      // of the count.
      const std::uint64_t word = claim.load(std::memory_order_acquire);
      if (GenOf(word) == self.exhausted || NextOf(word) >= ChunksOf(word) ||
          !MayClaim(word, self.index)) {
        return ran;
      }
    }
    const std::uint32_t amount = batch.load(std::memory_order_relaxed);
    const std::uint64_t word =
        claim.fetch_add(amount, std::memory_order_acq_rel);
    const std::uint32_t chunks = ChunksOf(word);
    const std::uint32_t first = NextOf(word);
    if (first >= chunks) {
      self.exhausted = GenOf(word);
      return ran;
    }
    const std::uint32_t last = std::min(first + amount, chunks);
    // Chaos: a delay between the claim and the descriptor read.
    if (chaos && self.rng.OneIn(4)) ChaosDelay(self.rng, kChaosClaimDelayNs);
    // The fork cannot end while this claim is outstanding, so the
    // descriptor and its generation are the claim's.
    const std::uint64_t claimed =
        desc_generation.load(std::memory_order_relaxed);
    if (claimed != self.desc_generation) {
      self.desc = desc;
      self.desc_generation = claimed;
      if (is_worker) {
        const BreadcrumbFields fields = UnpackBreadcrumb(self.desc.crumb);
        Breadcrumb& crumb = ThreadBreadcrumb();
        crumb.SetTick(fields.tick);
        crumb.SetPhase(fields.phase);
        crumb.SetSystem(fields.system);
      }
    }
    if (is_worker) {
      WorkerLine& line = slots[self.index].worker;
      if (line.joined.load(std::memory_order_relaxed) != claimed) {
        line.joined.store(claimed, std::memory_order_relaxed);
      }
      Bump(line.chunks, last - first);
    }
    RunBatch(self, first, last);
    ran += last - first;
    // Chaos: a delay between the last write and the decrement.
    if (chaos && self.rng.OneIn(4)) ChaosDelay(self.rng, kChaosClaimDelayNs);
    // The last access to the fork.
    const std::uint32_t done = last - first;
    if (remaining.fetch_sub(done, std::memory_order_acq_rel) == done) {
      remaining.notify_one();
    }
  }
}

void ForkJoinPool::Shared::RunBatch(Claimer& self, std::uint32_t first,
                                    std::uint32_t last) {
  SE_PROFILE_ZONE("ForkChunks");
  const Descriptor& d = self.desc;
  const ForkScope scope;
  const ChunkCrumbScope crumb;
  for (std::uint32_t k = first; k < last; ++k) {
    // After a throw the chunks not yet started are skipped.
    if (cancelled.load(std::memory_order_relaxed)) continue;
    const std::uint32_t chunk = d.order != nullptr ? d.order[k] : k;
    crumb.Set(chunk);
    std::exception_ptr thrown;
    if (!internal::RunChunkContained(
            d.invoke, d.body, ChunkAt(d.count, d.grain, chunk), thrown)) {
      if (!error_claimed.exchange(true, std::memory_order_acq_rel)) {
        error = thrown;
      }
      cancelled.store(true, std::memory_order_relaxed);
    }
  }
}

bool ForkJoinPool::Shared::Spin(Claimer& self, std::uint32_t seen) {
  if (spin_ns == 0 || !spin_hint.load(std::memory_order_relaxed)) {
    return false;
  }
  WorkerLine& line = slots[self.index].worker;
  Bump(line.spins);
  const std::atomic<std::uint32_t>& wake = slots[self.index].wake.word;
  const std::uint64_t start = ProfileNowNs();
  constexpr int kPausesPerCheck = 32;
  do {
    for (int i = 0; i < kPausesPerCheck; ++i) SpinPause();
    if (stop.load(std::memory_order_relaxed) ||
        wake.load(std::memory_order_relaxed) != seen) {
      Bump(line.spin_hits);
      return true;
    }
    const std::uint64_t word = claim.load(std::memory_order_relaxed);
    if (GenOf(word) != self.exhausted && NextOf(word) < ChunksOf(word) &&
        MayClaim(word, self.index)) {
      Bump(line.spin_hits);
      return true;
    }
  } while (ProfileNowNs() - start < spin_ns);
  return false;
}

void ForkJoinPool::Shared::Publish(const Descriptor& d, std::uint32_t amount,
                                   bool hint, int begin, int end) {
  const std::uint64_t generation_now = ++generation;
  desc = d;
  error = nullptr;
  error_claimed.store(false, std::memory_order_relaxed);
  cancelled.store(false, std::memory_order_relaxed);
  spin_hint.store(hint, std::memory_order_relaxed);
  desc_generation.store(generation_now, std::memory_order_relaxed);
  batch.store(amount, std::memory_order_relaxed);
  remaining.store(d.chunks, std::memory_order_relaxed);
  claim.store(Pack(generation_now, begin, end, d.chunks, 0),
              std::memory_order_release);
}

void ForkJoinPool::Shared::Wake(std::span<const int> woken,
                                std::uint64_t now_ns) {
  deferred_count = 0;
  const std::uint64_t stamp = now_ns == 0 ? 1 : now_ns;
  for (const int k : woken) {
    Slot& slot = slots[k];
    slot.bump.generation.store(generation, std::memory_order_relaxed);
    // Keeps the stamp of a bump the worker has not seen yet.
    std::uint64_t unseen = 0;
    slot.bump.stamp_ns.compare_exchange_strong(unseen, stamp,
                                               std::memory_order_relaxed);
    slot.wake.word.fetch_add(1, std::memory_order_release);
    ++bumps;
    // Chaos: an injected late wake, the notify sent after a delay.
    if (chaos_seed != 0 && caller.rng.OneIn(4)) {
      deferred[static_cast<std::size_t>(deferred_count++)] = k;
      continue;
    }
    slot.wake.word.notify_one();
  }
  if (deferred_count > 0) {
    ChaosDelay(caller.rng, kChaosWakeDelayNs);
    NotifyDeferred();
  }
}

void ForkJoinPool::Shared::NotifyDeferred() {
  for (int i = 0; i < deferred_count; ++i) {
    slots[deferred[static_cast<std::size_t>(i)]].wake.word.notify_one();
  }
  deferred_count = 0;
}

void ForkJoinPool::Shared::Join() {
  std::uint32_t left = remaining.load(std::memory_order_acquire);
  while (left != 0) {
    remaining.wait(left, std::memory_order_acquire);
    left = remaining.load(std::memory_order_acquire);
  }
}

void ForkJoinPool::Shared::Stop() noexcept {
  stop.store(true, std::memory_order_release);
  for (int k = 0; k < workers; ++k) {
    slots[k].wake.word.fetch_add(1, std::memory_order_release);
    slots[k].wake.word.notify_one();
  }
}

void ForkJoinPool::Shared::ShuffleOrder(std::uint32_t chunks,
                                        std::uint64_t fork_generation) {
  order.resize(chunks);
  for (std::uint32_t i = 0; i < chunks; ++i) order[i] = i;
  ChaosRng rng(Mix64(chaos_seed ^ (fork_generation * kHashGamma)));
  for (std::uint32_t i = chunks; i > 1; --i) {
    const auto j = static_cast<std::uint32_t>(rng.Below(i));
    std::swap(order[i - 1], order[j]);
  }
}

ForkJoinPool::ForkJoinPool() noexcept = default;

ForkJoinPool::ForkJoinPool(Config config) noexcept
    : config_(std::move(config)) {}

ForkJoinPool::~ForkJoinPool() { StopWorkers(); }

std::uint64_t ForkJoinPool::Now() const noexcept {
  return config_.now_ns != nullptr ? config_.now_ns() : ProfileNowNs();
}

int ForkJoinPool::ActiveWorkers() const noexcept {
  return shared_ != nullptr ? shared_->workers : 0;
}

bool ForkJoinPool::SiteEnabledByConfig(std::string_view name) const {
  for (const auto& [site, enabled] : site_overrides_) {
    if (site == name) return enabled;
  }
  return std::any_of(
      config_.enabled_sites.begin(), config_.enabled_sites.end(),
      [name](const std::string& site) { return site == "*" || site == name; });
}

ForkSite& ForkJoinPool::Site(const ForkSiteDecl& decl) {
  for (std::size_t i = 0; i < sites_.size(); ++i) {
    if (site_decls_[i] == &decl) return *sites_[i];
  }
  for (std::size_t i = 0; i < sites_.size(); ++i) {
    if (sites_[i]->name == decl.name) {
      site_decls_[i] = &decl;
      return *sites_[i];
    }
  }
  auto site = std::make_unique<ForkSite>();
  site->name = decl.name;
  site->grain = decl.grain;
  site->min_items = decl.min_items;
  site->cores = decl.cores;
  site->reads = decl.reads;
  site->writes = decl.writes;
  site->enabled = SiteEnabledByConfig(decl.name);
  site->owner = this;
  ForkSite& added = *site;
  sites_.push_back(std::move(site));
  site_decls_.push_back(&decl);
  if (added.enabled) StartWorkers();
  return added;
}

void ForkJoinPool::EnableSite(std::string_view name, bool enabled) {
  std::erase_if(site_overrides_,
                [name](const auto& entry) { return entry.first == name; });
  site_overrides_.emplace_back(std::string(name), enabled);
  for (const auto& site : sites_) {
    if (site->name == name) site->enabled = enabled;
  }
  if (enabled) StartWorkers();
}

std::vector<const ForkSite*> ForkJoinPool::Sites() const {
  std::vector<const ForkSite*> out;
  out.reserve(sites_.size());
  for (const auto& site : sites_) out.push_back(site.get());
  return out;
}

const ForkSite* ForkJoinPool::FindSite(std::string_view name) const {
  for (const auto& site : sites_) {
    if (site->name == name) return site.get();
  }
  return nullptr;
}

std::uint32_t ForkJoinPool::ChunkCountFor(const ForkSite& site,
                                          std::uint32_t count) const noexcept {
  const std::uint32_t grain =
      config_.grain_override != 0 ? config_.grain_override : site.grain;
  return ChunkCount(count, grain);
}

namespace {

void RefreshSite(ForkSite& site) {
  std::vector<std::uint32_t> sorted;
  site.joins.Sorted(sorted);
  const std::uint64_t p50 = NthNs(sorted, 0.5);
  site.stats.join_p50_us = static_cast<double>(p50) * 1e-3;
  site.stats.join_p99_us = static_cast<double>(NthNs(sorted, 0.99)) * 1e-3;
  site.stats.join_p999_us = static_cast<double>(NthNs(sorted, 0.999)) * 1e-3;
  site.median_ns = p50;
}

}  // namespace

void ForkJoinPool::RefreshStats() {
  for (const auto& site : sites_) {
    if (site->joins.Count() > 0) RefreshSite(*site);
  }
}

ForkJoinPool::Plan ForkJoinPool::PlanFork(ForkSite& site, std::uint32_t count) {
  Plan plan;
  plan.count = count;
  plan.grain =
      config_.grain_override != 0 ? config_.grain_override : site.grain;
  plan.chunks = ChunkCount(count, plan.grain);
  ++site.stats.forks;
  site.stats.chunks += plan.chunks;
  plan.participants = Participants(site, plan);
  if (plan.participants < 2) {
    ++site.stats.inline_forks;
  } else {
    ++site.stats.threaded_forks;
  }
  return plan;
}

std::uint32_t ForkJoinPool::Participants(ForkSite& site, const Plan& plan) {
  if (!site.enabled || config_.workers == 0 || plan.count < site.min_items ||
      plan.chunks < 2 || plan.chunks > kMaxThreadedChunks) {
    return 1;
  }
  if (!StartWorkers()) return 1;
  int workers = shared_->workers;
  if (site.cores == CoreClass::kPerformanceOnly) {
    workers = std::min(workers, performance_only_);
  }
  const std::uint32_t per =
      std::max<std::uint32_t>(1, config_.chunks_per_participant);
  const std::uint32_t participants = std::min<std::uint32_t>(
      static_cast<std::uint32_t>(workers) + 1, plan.chunks / per);
  if (participants < 2) return 1;
  if (Now() < site.inline_until_ns) {
    ++site.stats.fallback_forks;
    return 1;
  }
  return participants;
}

void ForkJoinPool::RunThreaded(ForkSite& site, const Plan& plan,
                               InvokeFn invoke, const void* body) {
  Shared& s = *shared_;
  std::uint64_t start = Now();
  // The spin prediction: did this fork follow the last one quickly?
  if (last_site_ != nullptr) {
    last_site_->followed_quickly =
        start - last_join_end_ns_ <=
        static_cast<std::uint64_t>(config_.spin_us) * 1000;
  }
  if (battery_checked_ns_ == 0 ||
      start - battery_checked_ns_ >= kBatteryPollNs) {
    on_battery_ = threads_->OnBattery();
    battery_checked_ns_ = start == 0 ? 1 : start;
    // The poll (a system call, or reads of sysfs) is not the fork's time:
    // counted in the join it could pass 4x a small site's median and send
    // the site inline once a second, and it would show as wake latency.
    start = Now();
  }

  Descriptor d;
  d.invoke = invoke;
  d.body = body;
  d.count = plan.count;
  d.grain = plan.grain;
  d.chunks = plan.chunks;
  d.crumb = ThreadBreadcrumb().Packed();
  if (s.chaos_seed != 0) {
    // Seeded by the generation Publish is about to take.
    s.ShuffleOrder(plan.chunks, s.generation + 1);
    d.order = s.order.data();
  }
  const std::uint32_t amount = std::clamp<std::uint32_t>(
      plan.chunks / (plan.participants * kClaimsPerParticipant), 1, kMaxBatch);
  const bool hint =
      site.followed_quickly && !on_battery_ && config_.spin_us > 0;
  // A PerformanceOnly site's workers are the first performance_only_.
  const int eligible =
      site.cores == CoreClass::kPerformanceOnly ? performance_only_ : s.workers;
  s.Publish(d, amount, hint, 0, eligible);

  const auto woken_count = static_cast<std::size_t>(plan.participants - 1);
  std::array<int, kMaxWorkers> woken{};
  for (std::size_t k = 0; k < woken_count; ++k) {
    woken[k] = static_cast<int>(k);
  }
  const std::span<const int> woken_span(woken.data(), woken_count);
  s.Wake(woken_span, start);
  s.ClaimAndRun(s.caller);
  s.Join();

  const std::uint64_t end = Now();
  ++threaded_forks_;
  RecordJoin(site, end > start ? end - start : 0, end, woken_span,
             s.generation);
  last_site_ = site.owner == this ? &site : nullptr;
  last_join_end_ns_ = end;
  if (s.error) {
    ++site.stats.faults;
    std::exception_ptr error = std::exchange(s.error, nullptr);
    std::rethrow_exception(error);
  }
}

void ForkJoinPool::RecordJoin(ForkSite& site, std::uint64_t join_ns,
                              std::uint64_t end_ns, std::span<const int> woken,
                              std::uint64_t generation) {
  ForkStats& stats = site.stats;
  site.joins.Record(join_ns);
  const auto participants = static_cast<std::uint32_t>(woken.size() + 1);
  stats.participants = participants;
  stats.max_participants = std::max(stats.max_participants, participants);
  stats.join_max_us =
      std::max(stats.join_max_us, static_cast<double>(join_ns) * 1e-3);
  for (const int k : woken) {
    const auto& line = shared_->slots[k].worker;
    if (line.joined.load(std::memory_order_acquire) != generation) {
      ++stats.missed_wakes;
      ++missed_wakes_;
      continue;
    }
    if (line.woke.load(std::memory_order_relaxed) == generation &&
        line.latency_ns.load(std::memory_order_relaxed) > kLateWakeNs) {
      ++stats.late_wakes;
      ++late_wakes_;
    }
  }
  if (stats.threaded_forks == kFirstRefresh ||
      stats.threaded_forks % kRefreshEvery == 0) {
    RefreshSite(site);
  }
  if (config_.fallback_factor > 0 && site.median_ns > 0 &&
      join_ns > site.median_ns * config_.fallback_factor) {
    site.inline_until_ns = end_ns + config_.fallback_ns;
    ++stats.fallbacks;
    SE_LOG_DEBUG(
        "fork site {}: a join of {:.1f} us is over {}x its median {:.1f} us; "
        "inline for {:.1f} s",
        site.name, static_cast<double>(join_ns) * 1e-3, config_.fallback_factor,
        static_cast<double>(site.median_ns) * 1e-3,
        static_cast<double>(config_.fallback_ns) * 1e-9);
  }
}

bool ForkJoinPool::StartWorkers() {
  if (shared_ != nullptr) return true;
  if (start_failed_ || config_.workers == 0) return false;
  if (!config_.threads) {
    start_failed_ = true;
    SE_LOG_WARN("fork pool: no worker threads to start; every fork inline");
    return false;
  }
  std::unique_ptr<WorkerThreads> threads = config_.threads();
  if (threads == nullptr) {
    start_failed_ = true;
    SE_LOG_WARN("fork pool: no worker threads to start; every fork inline");
    return false;
  }
  int workers = config_.workers < 0 ? threads->MaxWorkers() : config_.workers;
  if (config_.max_workers >= 0) {
    workers = std::min(workers, config_.max_workers);
  }
  workers = std::clamp(workers, 0, kMaxWorkers);
  if (workers == 0) {
    start_failed_ = true;
    SE_LOG_INFO("fork pool: W = 0 on this machine; every fork inline");
    return false;
  }
  int performance_only = config_.performance_only_workers >= 0
                             ? config_.performance_only_workers
                             : threads->PerformanceOnlyWorkers();
  performance_only = std::clamp(performance_only, 0, workers);

  auto shared = std::make_unique<Shared>(workers, config_);
  std::vector<WorkerSpec> specs;
  specs.reserve(static_cast<std::size_t>(workers));
  for (int i = 0; i < workers; ++i) {
    specs.push_back(threads->Placement(i, workers));
    shared->slots[i].efficiency_class = specs.back().efficiency_class;
  }
  for (int i = 0; i < workers; ++i) {
    Shared* state = shared.get();
    auto started = threads->Start(specs[static_cast<std::size_t>(i)],
                                  [state, i] { state->WorkerLoop(i); });
    if (!started) {
      shared->Stop();
      threads->JoinAll();
      start_failed_ = true;
      SE_LOG_ERROR("fork pool: worker {} did not start ({}); every fork inline",
                   i, started.error().message);
      return false;
    }
  }
  shared_ = std::move(shared);
  threads_ = std::move(threads);
  performance_only_ = performance_only;
  SE_LOG_INFO(
      "fork pool: {} worker(s) started ({} for PerformanceOnly sites){}",
      workers, performance_only, config_.chaos_seed != 0 ? ", chaos mode" : "");
  if (config_.calibrate) Calibrate();
  return true;
}

void ForkJoinPool::Calibrate() {
  Shared& s = *shared_;
  // The caller's time for the dummy chunk: grow the iterations until a run
  // is measurable, then scale to kCalibrationChunkNs.
  // Every result goes to s.calibration_sink, which the workers can see, so
  // an optimised build cannot drop the work it times (a Release build did,
  // and sized the chunks from a loop that took no time).
  std::uint32_t iterations = 1024;
  std::uint64_t ns = 0;
  std::uint64_t sink = 0;
  constexpr std::uint64_t kMeasurableNs = 5'000;
  // 2^24 xorshift steps take milliseconds on any core: a probe that long
  // which still reads as no time was not run.
  constexpr std::uint32_t kMaxIterations = std::uint32_t{1} << 24;
  while (true) {
    const std::uint64_t start = ProfileNowNs();
    sink ^= DummyWork(iterations, iterations);
    s.calibration_sink.store(sink, std::memory_order_relaxed);
    ns = ProfileNowNs() - start;
    if (ns >= kMeasurableNs || iterations >= kMaxIterations) break;
    iterations *= 2;
  }
  calibrations_.clear();
  if (ns < kMeasurableNs) {
    SE_LOG_WARN(
        "fork pool: the calibration's dummy work could not be timed ({} "
        "steps in {} ns); not calibrated",
        iterations, ns);
    return;
  }
  const double per_iteration = static_cast<double>(ns) / iterations;
  iterations = static_cast<std::uint32_t>(
      std::clamp(static_cast<double>(kCalibrationChunkNs) / per_iteration, 1.0,
                 static_cast<double>(kMaxIterations)));
  std::uint64_t caller_ns = ~std::uint64_t{0};
  for (int i = 0; i < 3; ++i) {
    const std::uint64_t start = ProfileNowNs();
    sink ^= DummyWork(sink + static_cast<std::uint64_t>(i), iterations);
    s.calibration_sink.store(sink, std::memory_order_relaxed);
    caller_ns = std::min(caller_ns, ProfileNowNs() - start);
  }

  std::vector<std::uint8_t> classes;
  for (int k = 0; k < s.workers; ++k) {
    if (std::find(classes.begin(), classes.end(),
                  s.slots[k].efficiency_class) == classes.end()) {
      classes.push_back(s.slots[k].efficiency_class);
    }
  }
  std::sort(classes.rbegin(), classes.rend());
  for (const std::uint8_t efficiency_class : classes) {
    std::vector<int> members;
    for (int k = 0; k < s.workers; ++k) {
      if (s.slots[k].efficiency_class == efficiency_class) members.push_back(k);
    }
    const auto chunks = static_cast<std::uint32_t>(kCalibrationChunksPerWorker *
                                                   members.size());
    std::vector<CalibrationSample> samples(chunks);
    const CalibrationBody body{iterations, samples.data()};
    Descriptor d;
    d.invoke = &CalibrationChunk;
    d.body = &body;
    d.count = chunks;
    d.grain = 1;
    d.chunks = chunks;
    d.crumb = ThreadBreadcrumb().Packed();
    const std::uint64_t start = ProfileNowNs();
    s.Publish(d, 1, /*hint=*/false, members.front(), members.back() + 1);
    s.Wake(members, Now());
    // The caller does not claim: only this class's workers run the chunks.
    s.Join();
    const std::uint64_t wall = ProfileNowNs() - start;
    s.error = nullptr;

    std::vector<std::uint32_t> times;
    for (const CalibrationSample& sample : samples) {
      sink ^= sample.sink;
      if (sample.worker < 0 ||
          s.slots[sample.worker].efficiency_class != efficiency_class) {
        continue;
      }
      times.push_back(static_cast<std::uint32_t>(
          std::min<std::uint64_t>(sample.ns, 0xFFFFFFFFU)));
    }
    std::sort(times.begin(), times.end());
    Calibration result;
    result.efficiency_class = efficiency_class;
    result.workers = static_cast<int>(members.size());
    result.chunks = static_cast<std::uint32_t>(times.size());
    const std::uint64_t p50 = NthNs(times, 0.5);
    result.chunk_p50_us = static_cast<double>(p50) * 1e-3;
    result.chunk_max_us =
        times.empty() ? 0.0 : static_cast<double>(times.back()) * 1e-3;
    result.wall_ms = static_cast<double>(wall) * 1e-6;
    result.relative_speed =
        p50 > 0 ? static_cast<double>(caller_ns) / static_cast<double>(p50)
                : 0.0;
    calibrations_.push_back(result);
    SE_LOG_INFO(
        "fork pool: efficiency class {} calibrated: {} worker(s), {} chunks, "
        "p50 {:.1f} us, max {:.1f} us, {:.1f} ms, speed {:.2f} of the caller's",
        efficiency_class, result.workers, result.chunks, result.chunk_p50_us,
        result.chunk_max_us, result.wall_ms, result.relative_speed);
  }
  s.calibration_sink.store(sink, std::memory_order_relaxed);
}

void ForkJoinPool::StopWorkers() noexcept {
  if (shared_ == nullptr) return;
  shared_->Stop();
  if (threads_ != nullptr) threads_->JoinAll();
  shared_.reset();
  threads_.reset();
}

std::vector<WorkerWake> ForkJoinPool::WorkerWakes() const {
  std::vector<WorkerWake> out;
  if (shared_ == nullptr) return out;
  for (int k = 0; k < shared_->workers; ++k) {
    const Slot& slot = shared_->slots[k];
    out.push_back({slot.wake.word.load(std::memory_order_acquire),
                   slot.worker.seen.load(std::memory_order_acquire),
                   slot.efficiency_class});
  }
  return out;
}

PoolStats ForkJoinPool::Stats() const {
  PoolStats out;
  out.workers = ActiveWorkers();
  out.performance_only_workers = performance_only_;
  out.start_failed = start_failed_;
  out.threaded_forks = threaded_forks_;
  out.missed_wakes = missed_wakes_;
  out.late_wakes = late_wakes_;
  if (shared_ == nullptr) return out;
  out.bumps = shared_->bumps;
  for (int k = 0; k < shared_->workers; ++k) {
    const auto& line = shared_->slots[k].worker;
    out.wakes += line.wakes.load(std::memory_order_relaxed);
    out.unstamped_wakes += line.unstamped.load(std::memory_order_relaxed);
    out.wake_latency_max_ns =
        std::max(out.wake_latency_max_ns,
                 line.latency_max_ns.load(std::memory_order_relaxed));
    out.worker_chunks += line.chunks.load(std::memory_order_relaxed);
    out.spins += line.spins.load(std::memory_order_relaxed);
    out.spin_hits += line.spin_hits.load(std::memory_order_relaxed);
  }
  return out;
}

}  // namespace se::jobs
