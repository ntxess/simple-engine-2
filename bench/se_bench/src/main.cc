// Copyright 2026 The simple-engine-2 Authors.
//
// se_bench: runs one benchmark suite and reports the median, p95, p99 and
// max of its iterations (docs/milestone-specs.md M3c).
//
//   se_bench --suite NAME [--n N] [--iterations K] [--warmup W] [--seed S]
//            [--json FILE] [--max-p95-ms MS]
//   se_bench --list
//
// --max-p95-ms makes the run a gate: CTest's bench.<suite> tests pass the
// suite's threshold from bench/thresholds.json. Thresholds are for Release
// builds on the 7700X. Exit status: 0 ok, 1 over the threshold or the suite
// failed, 2 bad arguments.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "se_bench/suite.h"
#include "se_core/fp.h"
#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"

#ifndef SE_BENCH_CONFIG
#define SE_BENCH_CONFIG "unknown"
#endif
#ifndef SE_BENCH_COMPILER
#define SE_BENCH_COMPILER "unknown"
#endif

namespace se::bench {
namespace {

struct Args {
  bool list = false;
  std::string suite;
  std::optional<std::uint32_t> n;
  std::optional<std::uint32_t> iterations;
  std::optional<std::uint32_t> warmup;
  std::uint64_t seed = 1;
  std::string json;
  std::optional<double> max_p95_ms;
};

void Usage() {
  std::fputs(
      "usage: se_bench --suite NAME [--n N] [--iterations K] [--warmup W]\n"
      "                [--seed S] [--json FILE] [--max-p95-ms MS]\n"
      "       se_bench --list\n",
      stderr);
}

template <class T>
bool ParseNumber(std::string_view text, T& out) {
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, out);
  return ec == std::errc() && ptr == end;
}

std::optional<Args> Parse(int argc, char** argv) {
  Args args;
  const std::vector<std::string_view> tokens(argv + 1, argv + argc);
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const std::string_view flag = tokens[i];
    if (flag == "--list") {
      args.list = true;
      continue;
    }
    if (i + 1 >= tokens.size()) {
      std::fputs(std::format("se_bench: {} needs a value\n", flag).c_str(),
                 stderr);
      return std::nullopt;
    }
    const std::string_view value = tokens[++i];
    bool ok = true;
    std::uint32_t number = 0;
    if (flag == "--suite") {
      args.suite = value;
    } else if (flag == "--json") {
      args.json = value;
    } else if (flag == "--n") {
      ok = ParseNumber(value, number) && number > 0;
      args.n = number;
    } else if (flag == "--iterations") {
      ok = ParseNumber(value, number) && number > 0;
      args.iterations = number;
    } else if (flag == "--warmup") {
      ok = ParseNumber(value, number);
      args.warmup = number;
    } else if (flag == "--seed") {
      ok = ParseNumber(value, args.seed);
    } else if (flag == "--max-p95-ms") {
      double ms = 0.0;
      ok = ParseNumber(value, ms) && ms > 0.0;
      args.max_p95_ms = ms;
    } else {
      std::fputs(std::format("se_bench: unknown option {}\n", flag).c_str(),
                 stderr);
      return std::nullopt;
    }
    if (!ok) {
      std::fputs(
          std::format("se_bench: bad value '{}' for {}\n", value, flag).c_str(),
          stderr);
      return std::nullopt;
    }
  }
  if (!args.list && args.suite.empty()) return std::nullopt;
  return args;
}

// Registers the calling thread as the main thread for the run: suites build
// Worlds under an InlineOwnerScope, which needs g_main.
class SE_SCOPED_CAPABILITY MainThreadScope {
 public:
  MainThreadScope() noexcept SE_ACQUIRE(g_main) { RegisterMainThread(); }
  ~MainThreadScope() SE_RELEASE() { UnregisterMainThread(); }

  MainThreadScope(const MainThreadScope&) = delete;
  MainThreadScope& operator=(const MainThreadScope&) = delete;
};

double Ms(std::uint64_t ns) { return static_cast<double>(ns) * 1e-6; }

std::string JsonString(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  out += '"';
  return out;
}

std::string Report(const SuiteDef& def, const SuiteOptions& options,
                   const Summary& summary, const Sampler& sampler,
                   const std::optional<double>& max_p95_ms, bool passed) {
  std::string json = "{\n";
  json += std::format("  \"suite\": {},\n", JsonString(def.name));
  json += std::format("  \"description\": {},\n", JsonString(def.description));
  json += std::format("  \"config\": {},\n", JsonString(SE_BENCH_CONFIG));
  json += std::format("  \"compiler\": {},\n", JsonString(SE_BENCH_COMPILER));
  json += std::format("  \"n\": {},\n", options.n);
  json += std::format("  \"iterations\": {},\n", options.iterations);
  json += std::format("  \"warmup\": {},\n", options.warmup);
  json += std::format("  \"seed\": {},\n", options.seed);
  json += std::format("  \"min_ms\": {:.6f},\n", Ms(summary.min_ns));
  json += std::format("  \"median_ms\": {:.6f},\n", Ms(summary.median_ns));
  json += std::format("  \"mean_ms\": {:.6f},\n", summary.mean_ns * 1e-6);
  json += std::format("  \"p95_ms\": {:.6f},\n", Ms(summary.p95_ns));
  json += std::format("  \"p99_ms\": {:.6f},\n", Ms(summary.p99_ns));
  json += std::format("  \"max_ms\": {:.6f},\n", Ms(summary.max_ns));
  if (max_p95_ms) {
    json += std::format("  \"max_p95_ms\": {:.6f},\n", *max_p95_ms);
  } else {
    json += "  \"max_p95_ms\": null,\n";
  }
  json += std::format("  \"passed\": {},\n", passed);
  json += "  \"counters\": {";
  bool first = true;
  for (const Counter& counter : sampler.Counters()) {
    json += std::format("{}\n    {}: {}", first ? "" : ",",
                        JsonString(counter.name), counter.value);
    first = false;
  }
  json += first ? "}\n" : "\n  }\n";
  json += "}\n";
  return json;
}

int Run(const Args& args) {
  if (args.list) {
    for (const SuiteDef& def : Suites()) {
      std::fputs(std::format("{:<20} {}\n", def.name, def.description).c_str(),
                 stdout);
    }
    return 0;
  }
  const SuiteDef* def = FindSuite(args.suite);
  if (def == nullptr) {
    std::fputs(
        std::format("se_bench: unknown suite '{}' (see --list)\n", args.suite)
            .c_str(),
        stderr);
    return 2;
  }
  SuiteOptions options;
  options.n = args.n.value_or(def->default_n);
  options.iterations = args.iterations.value_or(def->default_iterations);
  options.warmup = args.warmup.value_or(def->default_warmup);
  options.seed = args.seed;

  Sampler sampler(options);
  if (const auto ran = def->run(options, sampler); !ran) {
    std::fputs(
        std::format("se_bench: {} failed: {}\n", def->name, ran.error().message)
            .c_str(),
        stderr);
    return 1;
  }
  const Summary summary = Summarize(sampler.SamplesNs());
  const bool passed =
      !args.max_p95_ms || Ms(summary.p95_ns) <= *args.max_p95_ms;

  std::fputs(std::format("{} ({} build, {}): n={} iterations={} warmup={}\n",
                         def->name, SE_BENCH_CONFIG, SE_BENCH_COMPILER,
                         options.n, options.iterations, options.warmup)
                 .c_str(),
             stdout);
  std::fputs(std::format("  median {:.4f} ms  p95 {:.4f} ms  p99 {:.4f} ms  "
                         "max {:.4f} ms  (min {:.4f}, mean {:.4f})\n",
                         Ms(summary.median_ns), Ms(summary.p95_ns),
                         Ms(summary.p99_ns), Ms(summary.max_ns),
                         Ms(summary.min_ns), summary.mean_ns * 1e-6)
                 .c_str(),
             stdout);
  for (const Counter& counter : sampler.Counters()) {
    std::fputs(std::format("  {} {}\n", counter.name, counter.value).c_str(),
               stdout);
  }
  if (args.max_p95_ms) {
    std::fputs(std::format("  gate: p95 {:.4f} ms {} {:.4f} ms: {}\n",
                           Ms(summary.p95_ns), passed ? "<=" : ">",
                           *args.max_p95_ms, passed ? "ok" : "FAILED")
                   .c_str(),
               stdout);
  }
  if (!args.json.empty()) {
    std::ofstream out(args.json, std::ios::binary);
    out << Report(*def, options, summary, sampler, args.max_p95_ms, passed);
    if (!out) {
      std::fputs(std::format("se_bench: cannot write {}\n", args.json).c_str(),
                 stderr);
      return 1;
    }
  }
  return passed ? 0 : 1;
}

}  // namespace
}  // namespace se::bench

int main(int argc, char** argv) {
  // A tool may catch (ADR-006 exempts test and tool code): a suite's setup
  // can run out of memory, and formatting or file output can throw.
  try {
    const std::optional<se::bench::Args> args = se::bench::Parse(argc, argv);
    if (!args) {
      se::bench::Usage();
      return 2;
    }
    // The same floating-point setup as the engine (FMA3 off on MSVC x64), so
    // the suites measure the code the game runs.
    se::fp::ConfigureProcess();
    const se::bench::MainThreadScope main_thread;
    return se::bench::Run(*args);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "se_bench: %s\n", e.what());
    return 2;
  }
}
