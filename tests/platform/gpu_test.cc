// Copyright 2026 The simple-engine-2 Authors.
//
// The GPU device and the frame transaction on a window (M5's clear frames,
// drawn since M7 by Renderer2D: the composite cleared, then blitted).
// Driver resolution is a unit test; device creation needs a GPU (label gpu,
// excluded on Linux); anything with a window is label desktop. The windows
// open without taking focus.

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "se_core/thread_role.h"
#include "se_core/thread_safety.h"
#include "se_platform/sdl_context.h"
#include "se_platform/window.h"
#include "se_render/frame_recorder.h"
#include "se_render/gpu_device.h"
#include "se_render/renderer_2d.h"
#include "support/test_roles.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace se {
namespace {

TEST_CASE("The GPU driver comes from the command line, then SE_GPU_DRIVER",
          "[driver]") {
  REQUIRE(ResolveGpuDriver("vulkan", "direct3d12") == "vulkan");
  REQUIRE(ResolveGpuDriver("", "vulkan") == "vulkan");
  REQUIRE(ResolveGpuDriver("", "") == std::string(DefaultGpuDriver()));
#if defined(_WIN32)
  REQUIRE(DefaultGpuDriver() == "direct3d12");
#else
  REQUIRE(DefaultGpuDriver() == "vulkan");
#endif
}

void CreateDevice(const std::string& driver) {
  const testing::MainThreadScope main;
  {
    SdlInitConfig sdl_config;
    sdl_config.subsystems = SDL_INIT_VIDEO;
    auto sdl = SdlContext::Init(sdl_config);
    REQUIRE(sdl);
    GpuDeviceConfig config;
    config.driver = driver;
    auto device = GpuDevice::Create(config);
    REQUIRE(device);
    REQUIRE(device->Native() != nullptr);
    REQUIRE(device->Driver() == driver);
    CHECK(device->ClaimedWindow() == nullptr);
    device->WaitForIdle();
  }
  SDL_Quit();
}

TEST_CASE("A Direct3D 12 device is the Windows default", "[gpu]") {
#if defined(_WIN32)
  CreateDevice("direct3d12");
#else
  SKIP("Windows only");
#endif
}

TEST_CASE("A Vulkan device can be chosen instead", "[gpu]") {
  CreateDevice("vulkan");
}

TEST_CASE("An unknown GPU driver is an error", "[gpu]") {
  const testing::MainThreadScope main;
  {
    SdlInitConfig sdl_config;
    sdl_config.subsystems = SDL_INIT_VIDEO;
    auto sdl = SdlContext::Init(sdl_config);
    REQUIRE(sdl);
    GpuDeviceConfig config;
    config.driver = "no-such-driver";
    auto device = GpuDevice::Create(config);
    REQUIRE_FALSE(device);
    CHECK(device.error().message.find("no-such-driver") != std::string::npos);
  }
  SDL_Quit();
}

// A window claimed by a device, for the frame tests.
struct ClaimedWindow {
  std::unique_ptr<Window> window;
  std::unique_ptr<GpuDevice> device;
  std::unique_ptr<Renderer2D> renderer;
};

ClaimedWindow OpenClaimedWindow(const std::string& driver) SE_REQUIRES(g_main) {
  ClaimedWindow result;
  WindowConfig config;
  config.title = "se_platform_tests frame";
  config.width = 320;
  config.height = 240;
  config.activate_when_shown = false;
  auto window = Window::Create(config);
  REQUIRE(window);
  result.window = std::move(*window);
  GpuDeviceConfig device_config;
  device_config.driver = driver;
  auto device = GpuDevice::Create(device_config);
  REQUIRE(device);
  result.device = std::make_unique<GpuDevice>(std::move(*device));
  REQUIRE(result.device->ClaimWindow(result.window->Native()));
  auto renderer = Renderer2D::Create(*result.device);
  REQUIRE(renderer);
  result.renderer = std::move(*renderer);
  return result;
}

// Runs one frame that clears the composite to |color| and blits it; true
// when it presented.
bool ClearFrame(Renderer2D& renderer, const ClearColor& color)
    SE_REQUIRES(g_main) {
  SDL_PumpEvents();
  FrameRequest request;
  request.clear = color;
  const FrameResult result = renderer.Frame(request);
  REQUIRE(result.submitted);
  return result.presented;
}

#if defined(_WIN32)
// The colour at the centre of the window's client area, as composed
// (PrintWindow with PW_RENDERFULLCONTENT reads flip-model swapchains).
COLORREF CentrePixel(HWND hwnd) {
  RECT client{};
  GetClientRect(hwnd, &client);
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;
  HDC window_dc = GetDC(hwnd);
  HDC memory_dc = CreateCompatibleDC(window_dc);
  HBITMAP bitmap = CreateCompatibleBitmap(window_dc, width, height);
  HGDIOBJ previous = SelectObject(memory_dc, bitmap);
  constexpr UINT kRenderFullContent = 0x00000002;
  PrintWindow(hwnd, memory_dc, PW_CLIENTONLY | kRenderFullContent);
  const COLORREF pixel = GetPixel(memory_dc, width / 2, height / 2);
  SelectObject(memory_dc, previous);
  DeleteObject(bitmap);
  DeleteDC(memory_dc);
  ReleaseDC(hwnd, window_dc);
  return pixel;
}
#endif

TEST_CASE("Clear frames present on a visible window", "[desktop][gpu]") {
  const testing::MainThreadScope main;
  {
    auto sdl = SdlContext::Init({});
    REQUIRE(sdl);
    ClaimedWindow claimed = OpenClaimedWindow(std::string(DefaultGpuDriver()));
    // A saturated colour that is easy to recognise.
    const ClearColor color{0.0F, 0.5F, 1.0F, 1.0F};
    int presented = 0;
    for (int i = 0; i < 30; ++i) {
      if (ClearFrame(*claimed.renderer, color)) ++presented;
    }
    claimed.device->WaitForIdle();
    REQUIRE(presented > 20);
    const GpuFrameStats& stats = claimed.device->Stats();
    REQUIRE(stats.frames == 30);
    CHECK(stats.presented == static_cast<std::uint64_t>(presented));
    REQUIRE(stats.submit_failures == 0);
    REQUIRE(stats.acquire_failures == 0);
#if defined(_WIN32)
    const COLORREF pixel =
        CentrePixel(static_cast<HWND>(claimed.window->NativeHandle()));
    INFO("centre pixel r=" << int{GetRValue(pixel)}
                           << " g=" << int{GetGValue(pixel)}
                           << " b=" << int{GetBValue(pixel)});
    REQUIRE(std::abs(int{GetRValue(pixel)} - 0) <= 8);
    REQUIRE(std::abs(int{GetGValue(pixel)} - 128) <= 12);
    REQUIRE(std::abs(int{GetBValue(pixel)} - 255) <= 8);
#endif
    claimed.renderer.reset();
    claimed.device.reset();
    claimed.window.reset();
  }
  SDL_Quit();
}

TEST_CASE("Forced NULL acquires skip presents, then presenting resumes",
          "[desktop][gpu]") {
  const testing::MainThreadScope main;
  {
    auto sdl = SdlContext::Init({});
    REQUIRE(sdl);
    ClaimedWindow claimed = OpenClaimedWindow(std::string(DefaultGpuDriver()));
    const ClearColor color{0.2F, 0.2F, 0.2F, 1.0F};
    for (int i = 0; i < 5; ++i) ClearFrame(*claimed.renderer, color);
    claimed.device->ForceNullAcquires(30);
    for (int i = 0; i < 30; ++i) {
      REQUIRE_FALSE(ClearFrame(*claimed.renderer, color));
    }
    REQUIRE(claimed.device->Stats().forced_null_acquires == 30);
    // Presenting resumes within 2 frames once the hook stops.
    int frames_to_present = 0;
    while (!ClearFrame(*claimed.renderer, color) && frames_to_present < 10) {
      ++frames_to_present;
    }
    REQUIRE(frames_to_present < 2);
    claimed.device->WaitForIdle();
    REQUIRE(claimed.device->Stats().submit_failures == 0);
    claimed.renderer.reset();
    claimed.device.reset();
    claimed.window.reset();
  }
  SDL_Quit();
}

}  // namespace
}  // namespace se
