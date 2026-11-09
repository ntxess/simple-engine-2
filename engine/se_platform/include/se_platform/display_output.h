// Copyright 2026 The simple-engine-2 Authors.
//
// The display outputs as the OS reports them (Windows; stubs elsewhere):
// which monitor shows a window, and that monitor's current mode. The vblank
// sampler (vblank_sampler.h) waits on the output of a MonitorHandle; the
// pacer compares the sampler's period with the mode.
//
// QueryMonitorMode reads the current refresh three ways, because they
// disagree on this machine (ADR-055): QueryDisplayConfig's target refresh
// (exact, the rate the output runs at), EnumDisplaySettings'
// ENUM_CURRENT_SETTINGS (whole hertz), and what SDL 3.4.20 reports, which
// is the refresh of IDXGIOutput::FindClosestMatchingMode for the current
// size and GDI rate (SDL_windowsmodes.c, WIN_GetRefreshRate): a mode from
// DXGI's mode list, so whenever that list lacks the current rate SDL
// reports another one.
//
// These are plain OS queries (no SDL), cheap enough for boot and display
// changes; call them from the main thread.

#ifndef SE_PLATFORM_DISPLAY_OUTPUT_H_
#define SE_PLATFORM_DISPLAY_OUTPUT_H_

#include <cstdint>
#include <string>
#include <vector>

namespace se {

// An OS monitor: the HMONITOR on Windows; nullptr means none.
using MonitorHandle = void*;

// The monitor showing the largest part of |native_window| (an HWND), or
// the nearest one; nullptr off Windows or without a window.
[[nodiscard]] MonitorHandle MonitorOfWindow(void* native_window) noexcept;

struct MonitorMode {
  // QueryDisplayConfig found the monitor's active path.
  bool known = false;
  // The GDI device name (\\.\DISPLAY2) and the monitor's friendly name.
  std::string device_name;
  std::string friendly_name;
  int width = 0;
  int height = 0;
  // The current refresh: QueryDisplayConfig's target refresh rate.
  std::uint32_t refresh_numerator = 0;
  std::uint32_t refresh_denominator = 0;
  double hz = 0.0;
  // EnumDisplaySettings(ENUM_CURRENT_SETTINGS).dmDisplayFrequency.
  std::uint32_t gdi_hz = 0;
  // The refresh SDL 3.4.20 reports for this mode (DXGI's closest match);
  // 0 when no DXGI output matched.
  double sdl_style_hz = 0.0;
  // DXGI modes listed at the current size, and the highest refresh among
  // them.
  std::uint32_t dxgi_modes_at_size = 0;
  double dxgi_max_hz_at_size = 0.0;
};

// The current mode of |monitor|; known = false off Windows or when the
// monitor has no active path.
[[nodiscard]] MonitorMode QueryMonitorMode(MonitorHandle monitor);

struct MonitorInfo {
  MonitorHandle handle = nullptr;
  bool primary = false;
  // The desktop rectangle.
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  MonitorMode mode;
};

// Every monitor on the desktop (EnumDisplayMonitors), primary first.
[[nodiscard]] std::vector<MonitorInfo> ListMonitors();

// A line describing |mode| for the log.
[[nodiscard]] std::string DescribeMonitorMode(const MonitorMode& mode);

}  // namespace se

#endif  // SE_PLATFORM_DISPLAY_OUTPUT_H_
