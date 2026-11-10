// Copyright 2026 The simple-engine-2 Authors.
//
// The heartbeat (docs/milestone-specs.md M12; docs/design/mt-design.md
// "watchdog"): known long work on main, such as a large synchronous load or
// a blocking save at session end, raises the App's heartbeat flag while it
// runs, so the watchdog excuses the periods it spans instead of counting
// them towards a hang. Nothing else raises it (ADR-106): a flag that is
// always up would excuse every hang, and a hang inside known long work is
// then not declared.
//
//   {
//     const HeartbeatScope beat = app.KnownLongWork("recovery files");
//     WriteRecoveryFiles();  // may take seconds
//   }
//
// Main only; scopes nest (each restores what it found). The App counts the
// scopes and keeps the longest (its name and length), which it logs at
// shutdown.

#ifndef SE_RUNTIME_HEARTBEAT_H_
#define SE_RUNTIME_HEARTBEAT_H_

#include <atomic>
#include <cstdint>
#include <string_view>

namespace se {

// Known long work so far.
struct LongWorkStats {
  std::uint64_t scopes = 0;
  std::uint64_t longest_ns = 0;
  std::string_view longest;
};

class HeartbeatScope {
 public:
  // Raises |flag| (a release store). |what| must outlive |stats| (a
  // literal, normally); |stats| may be null.
  HeartbeatScope(std::atomic<bool>& flag, std::string_view what,
                 LongWorkStats* stats) noexcept;
  ~HeartbeatScope();
  HeartbeatScope(const HeartbeatScope&) = delete;
  HeartbeatScope& operator=(const HeartbeatScope&) = delete;

 private:
  std::atomic<bool>* flag_;
  std::string_view what_;
  LongWorkStats* stats_;
  std::uint64_t start_ns_;
  bool previous_;
};

}  // namespace se

#endif  // SE_RUNTIME_HEARTBEAT_H_
