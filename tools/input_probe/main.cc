// Copyright 2026 The simple-engine-2 Authors.
//
// input_probe: logs each real key event's stamps and reports their
// resolution (docs/milestone-specs.md M8 "Input"; docs/gates.md "Win32 key
// stamp resolution"). It needs a person at the keyboard: nothing runs it
// unattended except its --help check.
//
//   input_probe [--raw-keyboard on|off] [--presses N] [--seconds S]
//
// With --raw-keyboard on (the runtime's default, SDL_HINT_WINDOWS_RAW_
// KEYBOARD=1) SDL stamps keys on its raw-input thread, in the
// SDL_GetTicksNS domain; with off it stamps them from the Win32 message
// time (GetMessageTime, milliseconds), which is what the editor uses (M10).
// For every key press and release (repeats skipped) it prints the event's
// SDL stamp, the time SDL_PollEvent returned it and the difference, and at
// the end the stamps' granularity (the smallest nonzero difference between
// two stamps, and how many differences are whole milliseconds) and the
// stamp-to-poll delay's p50, p99 and maximum. It ends after N presses
// (default 50), S seconds (default 120), Escape or closing the window.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
  bool raw_keyboard = true;
  int presses = 50;
  double seconds = 120.0;
};

void PrintUsage() {
  std::printf(
      "usage: input_probe [--raw-keyboard on|off] [--presses N] "
      "[--seconds S]\n"
      "Press keys in the window; Escape or closing it ends the probe.\n");
}

bool ParseOptions(int argc, char** argv, Options& options, bool& help) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--help" || arg == "-h") {
      help = true;
    } else if (arg == "--raw-keyboard" && has_value) {
      const std::string_view value = argv[++i];
      if (value != "on" && value != "off") return false;
      options.raw_keyboard = value == "on";
    } else if (arg == "--presses" && has_value) {
      options.presses = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--seconds" && has_value) {
      options.seconds = std::max(1.0, std::atof(argv[++i]));
    } else {
      return false;
    }
  }
  return true;
}

std::uint64_t Percentile(std::vector<std::uint64_t> values, double p) {
  if (values.empty()) return 0;
  std::ranges::sort(values);
  const auto index = static_cast<std::size_t>(
      std::round(p * static_cast<double>(values.size() - 1)));
  return values[std::min(index, values.size() - 1)];
}

void Report(const Options& options, const std::vector<std::uint64_t>& stamps,
            const std::vector<std::uint64_t>& delays) {
  std::printf("\ninput_probe: raw keyboard %s, %zu key events\n",
              options.raw_keyboard ? "on" : "off", stamps.size());
  if (stamps.size() < 2) {
    std::printf("too few events to measure the stamps' resolution\n");
    return;
  }
  std::uint64_t smallest = UINT64_MAX;
  std::size_t differences = 0;
  std::size_t whole_ms = 0;
  for (std::size_t i = 1; i < stamps.size(); ++i) {
    if (stamps[i] <= stamps[i - 1]) continue;
    const std::uint64_t delta = stamps[i] - stamps[i - 1];
    smallest = std::min(smallest, delta);
    ++differences;
    if (delta % 1'000'000 == 0) ++whole_ms;
  }
  if (differences == 0) {
    std::printf("every stamp is equal: no resolution to report\n");
  } else {
    std::printf(
        "stamp granularity: smallest difference %.3f ms; %zu of %zu "
        "differences are whole milliseconds\n",
        static_cast<double>(smallest) / 1e6, whole_ms, differences);
  }
  std::printf("stamp to poll: p50 %.3f ms, p99 %.3f ms, max %.3f ms\n",
              static_cast<double>(Percentile(delays, 0.5)) / 1e6,
              static_cast<double>(Percentile(delays, 0.99)) / 1e6,
              static_cast<double>(Percentile(delays, 1.0)) / 1e6);
}

int Run(int argc, char** argv) {
  Options options;
  bool help = false;
  if (!ParseOptions(argc, argv, options, help)) {
    PrintUsage();
    return 2;
  }
  if (help) {
    PrintUsage();
    return 0;
  }
  SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD, options.raw_keyboard ? "1" : "0");
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  const std::string title = std::string("input_probe: raw keyboard ") +
                            (options.raw_keyboard ? "on" : "off") +
                            " (press keys; Escape ends)";
  SDL_Window* window = SDL_CreateWindow(title.c_str(), 640, 160, 0);
  if (window == nullptr) {
    std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  std::printf("input_probe: raw keyboard %s; press keys in the window\n",
              options.raw_keyboard ? "on" : "off");
  std::vector<std::uint64_t> stamps;
  std::vector<std::uint64_t> delays;
  int presses = 0;
  bool done = false;
  const std::uint64_t end_ns =
      SDL_GetTicksNS() + static_cast<std::uint64_t>(options.seconds * 1e9);
  while (!done && SDL_GetTicksNS() < end_ns) {
    SDL_Event event;
    if (!SDL_WaitEventTimeout(&event, 100)) continue;
    const std::uint64_t polled = SDL_GetTicksNS();
    if (event.type == SDL_EVENT_QUIT ||
        event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
      done = true;
      continue;
    }
    if (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP) {
      continue;
    }
    if (event.key.repeat) continue;
    if (event.key.key == SDLK_ESCAPE) {
      done = true;
      continue;
    }
    const std::uint64_t stamp = event.key.timestamp;
    const std::uint64_t delay = polled > stamp ? polled - stamp : 0;
    stamps.push_back(stamp);
    delays.push_back(delay);
    std::fputs(std::format("{:<12} {:<4} stamp {} ns, polled {} ns, delay "
                           "{:.3f} ms\n",
                           SDL_GetKeyName(event.key.key),
                           event.key.down ? "down" : "up", stamp, polled,
                           static_cast<double>(delay) / 1e6)
                   .c_str(),
               stdout);
    if (event.key.down && ++presses >= options.presses) done = true;
  }
  Report(options, stamps, delays);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // A tool may catch (ADR-006 exempts test and tool code).
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "input_probe: %s\n", error.what());
    return 1;
  }
}
