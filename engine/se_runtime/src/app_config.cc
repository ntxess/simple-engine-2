// Copyright 2026 The simple-engine-2 Authors.

#include "se_runtime/app_config.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "se_core/error.h"
#include "se_core/log.h"
#include "se_render/frame_faults.h"

namespace se {
namespace {

constexpr std::array<std::string_view, 10> kInitSteps = {
    "crash-handler",  "logging", "sdl",        "event-filter",
    "content-paths",  "window",  "gpu-device", "cpu-topology",
    "helper-threads", "game",
};

std::unexpected<Error> Bad(std::string message) {
  return Fail(ErrorCode::kInvalidArgument, std::move(message));
}

template <class T>
std::optional<T> ParseNumber(std::string_view text) {
  T value{};
  const char* first = text.data();
  const char* last = first + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc() || ptr != last) return std::nullopt;
  return value;
}

std::optional<LogLevel> ParseLevel(std::string_view text) {
  if (text == "trace") return LogLevel::kTrace;
  if (text == "debug") return LogLevel::kDebug;
  if (text == "info") return LogLevel::kInfo;
  if (text == "warn") return LogLevel::kWarn;
  if (text == "error") return LogLevel::kError;
  return std::nullopt;
}

// Walks the arguments, splitting "--name=value".
class ArgReader {
 public:
  explicit ArgReader(std::span<const char* const> args) : args_(args) {}

  [[nodiscard]] bool Done() const noexcept { return index_ >= args_.size(); }

  // The next option name; its inline value, if any, is kept for Value().
  std::string_view Next() {
    std::string_view arg = args_[index_++];
    inline_value_.reset();
    if (arg.starts_with("--")) {
      const std::size_t eq = arg.find('=');
      if (eq != std::string_view::npos) {
        inline_value_ = arg.substr(eq + 1);
        arg = arg.substr(0, eq);
      }
    }
    return arg;
  }

  std::expected<std::string_view, Error> Value(std::string_view option) {
    if (inline_value_) return *std::exchange(inline_value_, std::nullopt);
    if (Done()) return Bad(std::format("{} needs a value", option));
    return std::string_view(args_[index_++]);
  }

  [[nodiscard]] bool HasInlineValue() const noexcept {
    return inline_value_.has_value();
  }

  std::span<const char* const> Rest() { return args_.subspan(index_); }

 private:
  std::span<const char* const> args_;
  std::size_t index_ = 0;
  std::optional<std::string_view> inline_value_;
};

std::expected<int, Error> ParseInitStep(std::string_view text) {
  if (auto number = ParseNumber<int>(text)) {
    if (*number >= 1 && *number <= static_cast<int>(kInitSteps.size())) {
      return *number;
    }
    return Bad(std::format("--fail-init-step {} is not in 1..{}", text,
                           kInitSteps.size()));
  }
  for (std::size_t i = 0; i < kInitSteps.size(); ++i) {
    if (kInitSteps[i] == text) return static_cast<int>(i + 1);
  }
  return Bad(std::format("--fail-init-step: unknown step '{}'", text));
}

std::expected<void, Error> ParseThrow(std::string_view text,
                                      AppConfig& config) {
  std::string_view site = text;
  std::string_view count;
  if (const std::size_t colon = text.find(':');
      colon != std::string_view::npos) {
    site = text.substr(0, colon);
    count = text.substr(colon + 1);
  }
  if (site == "iterate") {
    config.throw_site = ThrowSite::kIterate;
  } else if (site == "event") {
    config.throw_site = ThrowSite::kEvent;
  } else {
    return Bad(std::format("--inject-throw: unknown site '{}'", site));
  }
  if (!count.empty()) {
    const auto n = ParseNumber<std::int64_t>(count);
    if (!n || *n < 1) {
      return Bad(std::format("--inject-throw: bad count '{}'", count));
    }
    config.throw_on_call = *n;
  }
  return {};
}

std::expected<void, Error> ParseNullAcquire(std::string_view text,
                                            AppConfig& config) {
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) {
    return Bad("--inject-null-acquire needs FROM:COUNT");
  }
  const auto from = ParseNumber<std::int64_t>(text.substr(0, colon));
  const auto count = ParseNumber<std::int64_t>(text.substr(colon + 1));
  if (!from || !count || *from < 1 || *count < 0) {
    return Bad(std::format("--inject-null-acquire: bad value '{}'", text));
  }
  config.null_acquire_from = *from;
  config.null_acquire_count = *count;
  return {};
}

// "after-ticks:N", N >= 1 (the M4 exit hooks).
std::expected<std::int64_t, Error> ParseAfterTicks(std::string_view option,
                                                   std::string_view text) {
  constexpr std::string_view kPrefix = "after-ticks:";
  if (text.starts_with(kPrefix)) {
    const auto n = ParseNumber<std::int64_t>(text.substr(kPrefix.size()));
    if (n && *n >= 1) return *n;
  }
  return Bad(
      std::format("{}: bad value '{}' (expected after-ticks:N)", option, text));
}

// NAME@FRAME[=FILE].
std::expected<TextureReloadHook, Error> ParseReload(std::string_view text) {
  const std::size_t at = text.find('@');
  if (at == std::string_view::npos || at == 0) {
    return Bad(
        std::format("--reload-texture: bad value '{}' (expected "
                    "NAME@FRAME[=FILE])",
                    text));
  }
  TextureReloadHook hook;
  hook.name = std::string(text.substr(0, at));
  std::string_view rest = text.substr(at + 1);
  const std::size_t eq = rest.find('=');
  if (eq != std::string_view::npos) {
    hook.file = std::string(rest.substr(eq + 1));
    rest = rest.substr(0, eq);
    if (hook.file.empty()) {
      return Bad(std::format("--reload-texture: empty FILE in '{}'", text));
    }
  }
  const auto frame = ParseNumber<std::int64_t>(rest);
  if (!frame || *frame < 1) {
    return Bad(std::format("--reload-texture: bad FRAME in '{}'", text));
  }
  hook.frame = *frame;
  return hook;
}

// FRAME:FILE (FILE may hold a drive letter's colon).
std::expected<ReadbackHook, Error> ParseReadback(std::string_view text) {
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos || colon + 1 >= text.size()) {
    return Bad(
        std::format("--readback: bad value '{}' (expected FRAME:FILE)", text));
  }
  const auto frame = ParseNumber<std::int64_t>(text.substr(0, colon));
  if (!frame || *frame < 1) {
    return Bad(std::format("--readback: bad FRAME in '{}'", text));
  }
  return ReadbackHook{*frame, std::string(text.substr(colon + 1))};
}

std::expected<void, Error> NeedTestHooks(std::string_view option) {
  if (kRuntimeTestHooks) return {};
  return Bad(
      std::format("{} is a test hook, not built into this build", option));
}

}  // namespace

std::span<const std::string_view> InitStepNames() noexcept {
  return kInitSteps;
}

std::expected<AppConfig, Error> ParseAppConfig(
    std::span<const char* const> args) {
  AppConfig config;
  ArgReader reader(args);
  while (!reader.Done()) {
    const std::string_view option = reader.Next();
    auto flag = [&]() -> std::expected<void, Error> {
      if (reader.HasInlineValue()) {
        return Bad(std::format("{} takes no value", option));
      }
      return {};
    };
    if (option == "--") {
      for (const char* rest : reader.Rest()) {
        config.game_args.emplace_back(rest);
      }
      break;
    }
    if (option == "--frames") {
      auto value = reader.Value(option);
      if (!value) return std::unexpected(value.error());
      const auto n = ParseNumber<std::int64_t>(*value);
      if (!n || *n < 0) {
        return Bad(std::format("--frames: bad value '{}'", *value));
      }
      config.frames = *n;
    } else if (option == "--fixed-frame-dt") {
      auto value = reader.Value(option);
      if (!value) return std::unexpected(value.error());
      const auto ms = ParseNumber<double>(*value);
      if (!ms || !(*ms > 0.0) || *ms > 1000.0) {
        return Bad(
            std::format("--fixed-frame-dt: bad value '{}' (ms)", *value));
      }
      config.fixed_frame_dt_ms = *ms;
    } else if (option == "--gpu-driver") {
      auto value = reader.Value(option);
      if (!value) return std::unexpected(value.error());
      config.gpu_driver = std::string(*value);
    } else if (option == "--sim-driver") {
      auto value = reader.Value(option);
      if (!value) return std::unexpected(value.error());
      if (*value != "inline") {
        return Bad(std::format(
            "--sim-driver {}: only 'inline' exists until M17", *value));
      }
      config.sim_driver = std::string(*value);
    } else if (option == "--profile") {
      if (auto ok = flag(); !ok) {
        return std::unexpected(ok.error());
      }
      config.profile = true;
    } else if (option == "--headless") {
      if (auto ok = flag(); !ok) {
        return std::unexpected(ok.error());
      }
      config.headless = true;
    } else if (option == "--no-activate") {
      if (auto ok = flag(); !ok) {
        return std::unexpected(ok.error());
      }
      config.activate_when_shown = false;
    } else if (option == "--no-crash-capture") {
      if (auto ok = flag(); !ok) {
        return std::unexpected(ok.error());
      }
      config.crash_capture = false;
    } else if (option == "--return-on-quit") {
      if (auto ok = flag(); !ok) {
        return std::unexpected(ok.error());
      }
      config.terminate_on_quit = false;
    } else if (option == "--content-root") {
      auto value = reader.Value(option);
      if (!value) return std::unexpected(value.error());
      config.content_root = std::string(*value);
    } else if (option == "--pref-dir") {
      auto value = reader.Value(option);
      if (!value) return std::unexpected(value.error());
      config.pref_dir = std::string(*value);
    } else if (option == "--log-level") {
      auto value = reader.Value(option);
      if (!value) return std::unexpected(value.error());
      const auto level = ParseLevel(*value);
      if (!level) {
        return Bad(std::format("--log-level: bad value '{}'", *value));
      }
      config.log_level = *level;
    } else if (option == "--fail-init-step") {
      if (auto ok = NeedTestHooks(option); !ok) {
        return std::unexpected(ok.error());
      }
      auto value = reader.Value(option);
      if (!value) return std::unexpected(value.error());
      auto step = ParseInitStep(*value);
      if (!step) return std::unexpected(step.error());
      config.fail_init_step = *step;
    } else if (option == "--inject-throw") {
      if (auto ok = NeedTestHooks(option); !ok) {
        return std::unexpected(ok.error());
      }
      auto value = reader.Value(option);
      if (!value) return std::unexpected(value.error());
      if (auto ok = ParseThrow(*value, config); !ok) {
        return std::unexpected(ok.error());
      }
    } else if (option == "--inject-null-acquire") {
      if (auto ok = NeedTestHooks(option); !ok) {
        return std::unexpected(ok.error());
      }
      auto value = reader.Value(option);
      if (!value) return std::unexpected(value.error());
      if (auto ok = ParseNullAcquire(*value, config); !ok) {
        return std::unexpected(ok.error());
      }
    } else if (option == "--crash-test" || option == "--dump-test") {
      if (auto ok = NeedTestHooks(option); !ok) {
        return std::unexpected(ok.error());
      }
      auto value = reader.Value(option);
      if (!value) return std::unexpected(value.error());
      auto ticks = ParseAfterTicks(option, *value);
      if (!ticks) return std::unexpected(ticks.error());
      if (option == "--crash-test") {
        config.crash_test_after_ticks = *ticks;
      } else {
        config.dump_test_after_ticks = *ticks;
      }
    } else if (option == "--inject-frame-fault") {
      if (auto ok = NeedTestHooks(option); !ok) {
        return std::unexpected(ok.error());
      }
      auto value = reader.Value(option);
      if (!value) return std::unexpected(value.error());
      auto fault = ParseFrameFault(*value);
      if (!fault) return std::unexpected(fault.error());
      config.frame_fault = *fault;
    } else if (option == "--reload-texture") {
      if (auto ok = NeedTestHooks(option); !ok) {
        return std::unexpected(ok.error());
      }
      auto value = reader.Value(option);
      if (!value) return std::unexpected(value.error());
      auto reload = ParseReload(*value);
      if (!reload) return std::unexpected(reload.error());
      config.texture_reloads.push_back(std::move(*reload));
    } else if (option == "--readback") {
      if (auto ok = NeedTestHooks(option); !ok) {
        return std::unexpected(ok.error());
      }
      auto value = reader.Value(option);
      if (!value) return std::unexpected(value.error());
      auto readback = ParseReadback(*value);
      if (!readback) return std::unexpected(readback.error());
      config.readbacks.push_back(std::move(*readback));
    } else {
      return Bad(std::format("unknown option '{}'", option));
    }
  }
  return config;
}

std::expected<AppConfig, Error> ParseAppConfig(int argc, char** argv) {
  if (argc <= 1 || argv == nullptr) return AppConfig{};
  const char* const* first = argv + 1;
  return ParseAppConfig(
      std::span<const char* const>(first, static_cast<std::size_t>(argc - 1)));
}

}  // namespace se
