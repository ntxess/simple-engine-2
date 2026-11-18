// Copyright 2026 The simple-engine-2 Authors.
//
// se_crash_probe: registers dump streams with known contents, starts crash
// capture, then crashes or asks for dumps (docs/milestone-specs.md, M4b).
// tests/crash/crash_test.py runs it and checks the dumps with
// tools/minidump/read_dump.py; the contents below are mirrored there.
//
//   se_crash_probe --database DIR [--handler PATH] --crash=KIND
//       KIND: access-violation, abort, terminate, terminate-noexcept,
//       stack-overflow
//   se_crash_probe --database DIR [--handler PATH] --dump-without-crash
//       two dumps from a second thread, with more log records in the second;
//       a Play-start bytes stream (kind 7) registered for the first and
//       withdrawn (UnregisterStream, M12) before the second
//   se_crash_probe --database DIR --handler PATH --expect-start-failure
//       Start must fail (a missing or broken handler, or a build without
//       crashpad) and leave the fault handlers as they were; the probe runs
//       on and exits 0
//
// It prints "probe: thread <name> <id>" for its threads before anything
// can crash, and "probe: ..." progress lines.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <crtdbg.h>
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "se_core/breadcrumb.h"
#include "se_core/dump_stream.h"
#include "se_core/input_frame.h"
#include "se_core/thread_role.h"
#include "se_crash/crash.h"
#include "se_os/process.h"

namespace se::crash_probe {
namespace {

constexpr std::uint32_t kBreadcrumbSlots = 64;
constexpr std::uint32_t kMainLogCapacity = 64;
constexpr std::uint32_t kMainLogFirstDump = 80;
constexpr std::uint32_t kMainLogSecondDump = 90;
constexpr std::uint32_t kWorkerLogCapacity = 16;
constexpr std::uint32_t kWorkerLogRecords = 3;
constexpr std::uint32_t kInputCapacity = 16;
constexpr std::uint32_t kInputFrames = 20;
constexpr std::uint32_t kFlightRecords = 4;
constexpr std::uint32_t kFlightRecordSize = 32;

// Registered regions, never freed (the handler reads them at dump time).
struct BreadcrumbStream {
  DumpStreamHeader header;
  std::array<BreadcrumbRecord, kBreadcrumbSlots> records;
};
template <std::uint32_t Capacity>
struct LogStream {
  DumpStreamHeader header;
  std::array<LogRecord, Capacity> records;
};
struct InputStream {
  DumpStreamHeader header;
  std::array<InputFrameSlot, kInputCapacity> slots;
};
struct FlightStream {
  DumpStreamHeader header;
  std::array<std::uint8_t, std::size_t{kFlightRecords} * kFlightRecordSize>
      records;
};
// The Play-start bytes of the first dump (crash_test.py knows the text).
constexpr std::string_view kPlayStartText = "probe play-start bytes";
struct PlayStartStream {
  DumpStreamHeader header;
  std::array<char, kPlayStartText.size()> bytes;
};

alignas(64) BreadcrumbStream g_breadcrumbs{};
alignas(64) LogStream<kMainLogCapacity> g_main_log{};
alignas(64) LogStream<kWorkerLogCapacity> g_worker_log{};
alignas(64) InputStream g_input{};
alignas(64) FlightStream g_flight{};
alignas(64) PlayStartStream g_play_start{};

std::uint64_t OsThreadId() {
#if defined(_WIN32)
  return GetCurrentThreadId();
#else
  return static_cast<std::uint64_t>(gettid());
#endif
}

void NameThisThread(const char* name) {
#if defined(_WIN32)
  std::array<wchar_t, 32> wide{};
  for (std::size_t i = 0; name[i] != '\0' && i + 1 < wide.size(); ++i) {
    wide[i] = static_cast<wchar_t>(name[i]);
  }
  SetThreadDescription(GetCurrentThread(), wide.data());
#else
  pthread_setname_np(pthread_self(), name);
#endif
}

void Report(std::string_view line) {
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

template <std::size_t Size>
void CopyName(char (&out)[Size], std::string_view name) {
  std::fill_n(out, Size, '\0');
  std::copy_n(name.begin(), std::min(name.size(), Size - 1), out);
}

void InitHeader(DumpStreamHeader& header, DumpStreamKind kind,
                std::uint32_t record_size, std::uint32_t capacity,
                std::uint64_t os_thread_id, ThreadRole role,
                std::string_view name) {
  header.magic = kDumpStreamMagic;
  header.version = kDumpStreamVersion;
  header.kind = static_cast<std::uint16_t>(kind);
  header.record_size = record_size;
  header.capacity = capacity;
  header.head = 0;
  header.dropped = 0;
  header.os_thread_id = os_thread_id;
  header.role = static_cast<std::uint8_t>(role);
  CopyName(header.name, name);
}

void SetBreadcrumb(std::uint32_t slot, std::string_view name, ThreadRole role,
                   ThreadSlotState state, std::uint64_t os_thread_id,
                   const BreadcrumbFields& fields) {
  BreadcrumbRecord& record = g_breadcrumbs.records[slot];
  record.packed = PackBreadcrumb(fields);
  record.os_thread_id = os_thread_id;
  record.role = static_cast<std::uint8_t>(role);
  record.state = static_cast<std::uint8_t>(state);
  CopyName(record.name, name);
}

// Main's slot 0 at tick |tick|, inside system 5 of the Movement phase.
void SetMainBreadcrumb(std::uint64_t main_tid, std::uint32_t tick) {
  SetBreadcrumb(0, "main", ThreadRole::kMain, ThreadSlotState::kLive, main_tid,
                {.role = static_cast<std::uint8_t>(ThreadRole::kMain),
                 .phase = 2,
                 .system = 5,
                 .chunk = 0,
                 .tick = tick});
}

template <std::uint32_t Capacity>
void AppendLog(LogStream<Capacity>& ring, std::uint64_t sequence,
               std::uint32_t tick, std::uint32_t line, std::uint8_t level,
               ThreadRole role, std::string_view message) {
  LogRecord& record = ring.records[ring.header.head % Capacity];
  record.timestamp_ns = 1'000'000'000ULL + sequence * 1'000'000ULL;
  record.sequence = sequence;
  record.tick = tick;
  record.line = line;
  record.level = level;
  record.role = static_cast<std::uint8_t>(role);
  CopyName(record.file, "crash_probe.cc");
  const std::size_t length = std::min(message.size(), kLogRecordMessageSize);
  std::fill_n(record.message, kLogRecordMessageSize, '\0');
  std::copy_n(message.begin(), length, record.message);
  record.length = static_cast<std::uint16_t>(length);
  ++ring.header.head;
}

// Main's log record i: "probe log i", sequence 1000 + i, tick 200 + i / 2,
// line 100 + i, level i % 5.
void AppendMainLog(std::uint32_t i) {
  AppendLog(g_main_log, 1000 + i, 200 + (i / 2), 100 + i,
            static_cast<std::uint8_t>(i % 5), ThreadRole::kMain,
            std::format("probe log {}", i));
}

// Frame of tick t: held bits 0 and t % 64, one press of action t % 64,
// axis 0 = t / 4, t % 3 edges.
InputFrame FrameOfTick(std::uint32_t t) {
  InputFrame frame;
  frame.tick = t;
  const std::uint32_t action = t % kMaxActions;
  frame.held = 1ULL | (1ULL << action);
  frame.pressed[action] = 1;
  frame.axes[0] = static_cast<float>(t) * 0.25F;
  frame.edge_count = static_cast<std::uint8_t>(t % 3);
  for (std::uint8_t k = 0; k < frame.edge_count; ++k) {
    frame.edges[k] = {.action = static_cast<ActionId>(action),
                      .down = true,
                      .stamp_ns = (t * 1000ULL) + k,
                      .order = k,
                      .source = EdgeSource::kReplay};
  }
  frame.platform_live = true;
  return frame;
}

// Writes frame |t| as the World owner's ring does (InputFrameRing): frame n
// has stamp 2n + 1 while it is written and 2n + 2 once it is complete, and
// the head is published after it.
void PushFrame(std::uint32_t t) {
  const std::uint64_t index = g_input.header.head;
  InputFrameSlot& slot = g_input.slots[index % kInputCapacity];
  slot.stamp = (2 * index) + 1;
  slot.frame = FrameOfTick(t);
  slot.stamp = (2 * index) + 2;
  ++g_input.header.head;
}

void FillStreams(std::uint64_t main_tid) {
  InitHeader(g_breadcrumbs.header, DumpStreamKind::kBreadcrumbs,
             sizeof(BreadcrumbRecord), kBreadcrumbSlots, 0,
             ThreadRole::kLibrary, "threads");
  SetMainBreadcrumb(main_tid, 240);
  SetBreadcrumb(2, "fork-worker-3", ThreadRole::kForkWorker,
                ThreadSlotState::kExited, 0x1234,
                {.role = static_cast<std::uint8_t>(ThreadRole::kForkWorker),
                 .phase = 4,
                 .system = 7,
                 .chunk = 3,
                 .tick = 239});
  g_breadcrumbs.header.head = 3;

  InitHeader(g_main_log.header, DumpStreamKind::kLogRing, sizeof(LogRecord),
             kMainLogCapacity, main_tid, ThreadRole::kMain, "main");
  for (std::uint32_t i = 0; i < kMainLogFirstDump; ++i) {
    AppendMainLog(i);
  }
  g_main_log.header.dropped = 2;

  InitHeader(g_input.header, DumpStreamKind::kInputFrameRing,
             sizeof(InputFrameSlot), kInputCapacity, main_tid,
             ThreadRole::kMain, "input");
  for (std::uint32_t t = 1; t <= kInputFrames; ++t) {
    PushFrame(t);
  }
  // Tick 21 (frame 20) is being written when the dump is taken: its slot
  // (the oldest frame, tick 5) has stamp 2 * 20 + 1 and the head still
  // says 20.
  InputFrameSlot& torn = g_input.slots[g_input.header.head % kInputCapacity];
  torn.stamp = (2 * g_input.header.head) + 1;
  torn.frame.tick = kInputFrames + 1;

  InitHeader(g_flight.header, DumpStreamKind::kFlightRecorder,
             kFlightRecordSize, kFlightRecords, main_tid, ThreadRole::kMain,
             "flight");
  for (std::size_t i = 0; i < g_flight.records.size(); ++i) {
    g_flight.records[i] = static_cast<std::uint8_t>(i);
  }
  g_flight.header.head = kFlightRecords;
}

// The worker thread's slot and log ring: "worker log i", sequence 2000 + i,
// tick 240, line 300 + i, level info.
void FillWorker(std::uint64_t worker_tid) {
  SetBreadcrumb(1, "worker", ThreadRole::kWatchdog, ThreadSlotState::kLive,
                worker_tid,
                {.role = static_cast<std::uint8_t>(ThreadRole::kWatchdog),
                 .phase = 0,
                 .system = 0,
                 .chunk = 0,
                 .tick = 240});
  InitHeader(g_worker_log.header, DumpStreamKind::kLogRing, sizeof(LogRecord),
             kWorkerLogCapacity, worker_tid, ThreadRole::kWatchdog, "worker");
  for (std::uint32_t i = 0; i < kWorkerLogRecords; ++i) {
    AppendLog(g_worker_log, 2000 + i, 240, 300 + i, 2, ThreadRole::kWatchdog,
              std::format("worker log {}", i));
  }
}

// The process-wide handlers crash capture installs.
struct FaultHandlers {
#if defined(_WIN32)
  LPTOP_LEVEL_EXCEPTION_FILTER exception_filter = nullptr;
#endif
  decltype(SIG_DFL) abort_handler = SIG_DFL;
  std::terminate_handler terminate = nullptr;

  friend bool operator==(const FaultHandlers&, const FaultHandlers&) = default;
};

FaultHandlers CurrentFaultHandlers() {
  FaultHandlers handlers;
#if defined(_WIN32)
  handlers.exception_filter = SetUnhandledExceptionFilter(nullptr);
  SetUnhandledExceptionFilter(handlers.exception_filter);
#endif
  handlers.abort_handler = std::signal(SIGABRT, SIG_DFL);
  std::signal(SIGABRT, handlers.abort_handler);
  handlers.terminate = std::get_terminate();
  return handlers;
}

bool Register(crash::StreamType type, const void* data, std::size_t size) {
  if (auto registered = crash::RegisterStream(type, data, size); !registered) {
    Report(std::format("probe: RegisterStream failed: {}",
                       registered.error().message));
    return false;
  }
  return true;
}

bool RegisterStreams() {
  return Register(crash::LogRingStream(0), &g_main_log, sizeof(g_main_log)) &&
         Register(crash::LogRingStream(1), &g_worker_log,
                  sizeof(g_worker_log)) &&
         Register(crash::StreamType::kInputFrameRing, &g_input,
                  sizeof(g_input)) &&
         Register(crash::StreamType::kFlightRecorder, &g_flight,
                  sizeof(g_flight));
}

// The probe's second thread, "se-probe-worker" in breadcrumb slot 1 with
// log ring 1. It requests the dumps of --dump-without-crash, as the
// watchdog will; otherwise it waits until the probe exits, so every dump
// holds both threads.
class Worker {
 public:
  explicit Worker(std::uint64_t main_tid, bool dump)
      : thread_([this, main_tid, dump] { Body(main_tid, dump); }) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return ready_; });
  }
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;
  ~Worker() {
    {
      const std::scoped_lock lock(mutex_);
      stop_ = true;
    }
    changed_.notify_all();
    thread_.join();
  }

  // Waits for the dumps and returns whether both were written.
  [[nodiscard]] bool WaitForDumps() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return dumps_done_; });
    return dumps_ok_;
  }

 private:
  void Body(std::uint64_t main_tid, bool dump) {
    NameThisThread("se-probe-worker");
    const std::uint64_t worker_tid = OsThreadId();
    FillWorker(worker_tid);
    Report(std::format("probe: thread worker {}", worker_tid));
    {
      const std::scoped_lock lock(mutex_);
      ready_ = true;
    }
    changed_.notify_all();
    const bool ok = !dump || DumpTwice(main_tid);
    {
      std::unique_lock lock(mutex_);
      dumps_ok_ = ok;
      dumps_done_ = true;
      changed_.notify_all();
      changed_.wait(lock, [this] { return stop_; });
    }
  }

  static bool DumpTwice(std::uint64_t main_tid) {
    // The first dump holds the Play-start bytes, the second does not: they
    // are withdrawn between the two (M12).
    InitHeader(g_play_start.header, DumpStreamKind::kBytes, 1,
               static_cast<std::uint32_t>(kPlayStartText.size()), main_tid,
               ThreadRole::kMain, "play-start");
    std::copy(kPlayStartText.begin(), kPlayStartText.end(),
              g_play_start.bytes.begin());
    g_play_start.header.head = kPlayStartText.size();
    if (!Register(crash::StreamType::kPlayStartBytes, &g_play_start,
                  sizeof(g_play_start))) {
      return false;
    }
    if (auto dump = crash::DumpWithoutCrash("probe dump 1"); !dump) {
      Report(std::format("probe: dump 1 failed: {}", dump.error().message));
      return false;
    }
    Report("probe: dump 1 written");
    if (auto withdrawn =
            crash::UnregisterStream(crash::StreamType::kPlayStartBytes);
        !withdrawn) {
      Report(std::format("probe: UnregisterStream failed: {}",
                         withdrawn.error().message));
      return false;
    }
    // The withdrawn region's contents change: the second dump must not
    // read it.
    g_play_start.bytes.fill('x');
    // The second dump must show what changed since: the handler reads the
    // registered memory at dump time.
    for (std::uint32_t i = kMainLogFirstDump; i < kMainLogSecondDump; ++i) {
      AppendMainLog(i);
    }
    SetMainBreadcrumb(main_tid, 241);
    if (auto dump = crash::DumpWithoutCrash("probe dump 2"); !dump) {
      Report(std::format("probe: dump 2 failed: {}", dump.error().message));
      return false;
    }
    Report("probe: dump 2 written");
    return true;
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  bool ready_ = false;
  bool dumps_done_ = false;
  bool dumps_ok_ = false;
  bool stop_ = false;
  std::thread thread_;
};

[[noreturn]] void CrashAccessViolation() {
  volatile int* volatile target = nullptr;
  *target = 42;
  std::abort();  // not reached
}

// std::terminate while an exception is being handled: the reason carries
// its message on every compiler.
[[noreturn]] void TerminateInHandler() {
  try {
    throw std::runtime_error("probe terminate");
  } catch (const std::exception&) {
    std::terminate();
  }
}

// Throws through noexcept. Whether the exception is still active when
// std::terminate runs is the compiler's choice: MSVC keeps it, clang-cl
// calls std::terminate before the exception is handled.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4297)  // throws in a noexcept function, on purpose
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexceptions"
#endif
// NOLINTNEXTLINE(bugprone-exception-escape): the point of the scenario.
void ThrowThroughNoexcept() noexcept {
  throw std::runtime_error("probe noexcept");
}
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif

int Recurse(int depth) {
  std::array<volatile char, 4096> frame{};
  frame[0] = static_cast<char>(depth);
  if (depth >= 0) {
    return Recurse(depth + 1) + frame[0];
  }
  return frame[0];
}

[[noreturn]] void Crash(std::string_view kind) {
  if (kind == "access-violation") {
    CrashAccessViolation();
  }
  if (kind == "abort") {
    std::abort();
  }
  if (kind == "terminate") {
    TerminateInHandler();
  }
  if (kind == "terminate-noexcept") {
    ThrowThroughNoexcept();
  }
  if (kind == "stack-overflow") {
    Report(std::format("probe: recursion returned {}", Recurse(0)));
  }
  Report(std::format("probe: unknown crash kind '{}'", kind));
  std::exit(2);
}

int Run(int argc, char** argv) {
  std::string database;
  std::string handler;
  std::string crash_kind;
  bool dump_without_crash = false;
  bool expect_start_failure = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--database" && i + 1 < argc) {
      database = argv[++i];
    } else if (arg == "--handler" && i + 1 < argc) {
      handler = argv[++i];
    } else if (arg.starts_with("--crash=")) {
      crash_kind = arg.substr(8);
    } else if (arg == "--dump-without-crash") {
      dump_without_crash = true;
    } else if (arg == "--expect-start-failure") {
      expect_start_failure = true;
    } else {
      Report(std::format("probe: unknown argument '{}'", arg));
      return 2;
    }
  }
  if (database.empty()) {
    Report("probe: --database is required");
    return 2;
  }
  if (handler.empty()) {
    auto dir = ExecutableDir();
    if (!dir) {
      Report(std::format("probe: {}", dir.error().message));
      return 2;
    }
    handler = crash::HandlerPathIn(*dir);
  }

  NameThisThread("se-probe-main");
  const std::uint64_t main_tid = OsThreadId();
  Report(std::format("probe: thread main {}", main_tid));
  FillStreams(main_tid);
  // The breadcrumb table is registered before Start, the rest after it:
  // both reach the dump.
  if (!Register(crash::StreamType::kBreadcrumbs, &g_breadcrumbs,
                sizeof(g_breadcrumbs))) {
    return 1;
  }

  crash::CrashConfig config;
  config.database_dir = database;
  config.handler_path = handler;
  config.annotations = {
      {"se.probe", "crash_probe"},
      {"se.scenario",
       dump_without_crash ? std::string("dump-without-crash") : crash_kind}};
  const FaultHandlers before = CurrentFaultHandlers();
  auto guard = crash::Start(config);
  if (!guard) {
    Report(std::format("probe: crash capture unavailable: {}",
                       guard.error().message));
    if (!expect_start_failure) {
      return 1;
    }
    // A failed Start leaves the process's fault handlers as they were.
    if (CurrentFaultHandlers() != before) {
      Report("probe: a failed Start changed the fault handlers");
      return 1;
    }
    Report("probe: fault handlers unchanged");
    // The probe runs on without crash capture.
    if (!RegisterStreams()) {
      return 1;
    }
    if (auto dump = crash::DumpWithoutCrash("no handler"); dump) {
      Report("probe: DumpWithoutCrash succeeded without a handler");
      return 1;
    }
    Report("probe: running without crash capture");
    return 0;
  }
  if (expect_start_failure) {
    Report("probe: Start succeeded but was expected to fail");
    return 1;
  }
  Report("probe: crash capture running");
  if (!RegisterStreams()) {
    return 1;
  }

  if (!dump_without_crash && crash_kind.empty()) {
    Report("probe: give --crash=KIND or --dump-without-crash");
    return 2;
  }
  Worker worker(main_tid, dump_without_crash);
  if (dump_without_crash) {
    if (!worker.WaitForDumps()) {
      return 1;
    }
    // Releasing the guard ends DumpWithoutCrash and puts the terminate
    // handler back.
    {
      const crash::CrashGuard released = std::move(*guard);
    }
    if (crash::Running() || std::get_terminate() != before.terminate ||
        crash::DumpWithoutCrash("after the guard").has_value()) {
      Report("probe: crash capture still active after the guard");
      return 1;
    }
    Report("probe: guard released");
    return 0;
  }
  Report(std::format("probe: crashing ({})", crash_kind));
  Crash(crash_kind);
}

}  // namespace
}  // namespace se::crash_probe

int main(int argc, char** argv) {
#if defined(_WIN32)
  // No error dialogs: crash dialogs, "abort() has been called" or the debug
  // CRT's report windows (SE_NO_MESSAGE_BOX is implied for this probe).
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
               SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
  try {
    return se::crash_probe::Run(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "probe: exception: %s\n", e.what());
    return 3;
  }
}
