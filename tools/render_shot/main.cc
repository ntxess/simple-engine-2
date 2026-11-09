// Copyright 2026 The simple-engine-2 Authors.
//
// render_shot: renders a rampage scenario at one tick into a PNG with
// se_render on a GPU device without a window (docs/milestone-specs.md M7,
// "Legacy image comparison").
//
//   render_shot (--legacy-scenario FILE | --scene FILE) [--inputs FILE]
//               --tick N --out FILE.png [--gpu-driver NAME] [--root DIR]
//               [--legacy-rules v1|v2] [--seed S]
//               [--compare FILE [--diff FILE]]
//   render_shot --compare A.png --with B.png [--diff FILE]
//   render_shot --probe-driver NAME
//
// The World runs headless as se_replay runs it (one tick per frame; the
// input from a legacy .seinput file, else scripted from the seed), and at
// tick N its RenderList through SandboxCamera at alpha 1 (the Transform,
// as the legacy Sandbox draws it) is drawn into a composite of the
// camera's canvas size, cleared to opaque black, with the textures of
// <root>/config/texture.json. legacy_replay --image-out N:FILE draws the
// same scenario from the same scene and seed into the same size. The
// driver comes from --gpu-driver, else SE_GPU_DRIVER, else the platform
// default.
//
// --compare compares the rendered image (or, with --with and no scene, two
// files) with the golden tolerance of tests/render_gpu/support/
// image_compare.h: the same size, at least 98% of the pixels within 8/255
// on every channel, a mean absolute difference of at most 2/255; on a
// mismatch the difference goes to --diff (default <out>.diff.png).
// tools/render_shot/compare_images.py is the same rule in Python.
//
// --probe-driver exits 0 when SDL_GPU can run the driver here, else 77.
//
// Exit status: 0 written (and matching), 1 a failure or a mismatch, 2 bad
// arguments, 77 the GPU driver is not available (CTest's skip code).

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>

#include <charconv>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "game_rampage/module.h"
#include "game_rampage/view.h"
#include "rampage/support/rampage_session.h"
#include "rampage/support/seinput.h"
#include "render_gpu/support/image_compare.h"
#include "se_core/thread_role.h"
#include "se_core/types.h"
#include "se_core/world.h"
#include "se_platform/fatal.h"
#include "se_platform/sdl_context.h"
#include "se_render/gpu_device.h"
#include "se_render/image_io.h"
#include "se_render/renderer_2d.h"
#include "se_render/texture_table.h"
#include "se_render_core/render_list.h"
#include "support/test_roles.h"

namespace se {
namespace {

constexpr int kSkipStatus = 77;

bool DriverAvailable(const std::string& driver) {
  return SDL_GPUSupportsShaderFormats(
      SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV, driver.c_str());
}

struct Args {
  std::string scene;
  std::string legacy_scenario;
  std::string inputs;
  std::string out;
  std::string driver;
  std::string root;
  std::string legacy_rules = "v2";
  Tick tick = 0;
  std::uint64_t seed = 42;
  std::string compare;
  std::string with;
  std::string diff;
  std::string probe;
};

void Usage() {
  std::fputs(
      "usage: render_shot (--legacy-scenario FILE | --scene FILE) "
      "[--inputs FILE]\n"
      "                   --tick N --out FILE.png [--gpu-driver NAME] "
      "[--root DIR]\n"
      "                   [--legacy-rules v1|v2] [--seed S]\n"
      "                   [--compare FILE [--diff FILE]]\n"
      "       render_shot --compare A.png --with B.png [--diff FILE]\n"
      "       render_shot --probe-driver NAME\n",
      stderr);
}

std::optional<std::uint64_t> Number(std::string_view text) {
  std::uint64_t value = 0;
  const char* first = text.data();
  const char* last = first + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc() || ptr != last) return std::nullopt;
  return value;
}

std::optional<Args> Parse(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (i + 1 >= argc) return std::nullopt;
    const std::string value = argv[++i];
    if (flag == "--scene") {
      args.scene = value;
    } else if (flag == "--legacy-scenario") {
      args.legacy_scenario = value;
    } else if (flag == "--inputs") {
      args.inputs = value;
    } else if (flag == "--out") {
      args.out = value;
    } else if (flag == "--gpu-driver") {
      args.driver = value;
    } else if (flag == "--root") {
      args.root = value;
    } else if (flag == "--legacy-rules") {
      args.legacy_rules = value;
    } else if (flag == "--tick") {
      const auto tick = Number(value);
      if (!tick || *tick == 0) return std::nullopt;
      args.tick = *tick;
    } else if (flag == "--probe-driver") {
      args.probe = value;
    } else if (flag == "--compare") {
      args.compare = value;
    } else if (flag == "--with") {
      args.with = value;
    } else if (flag == "--diff") {
      args.diff = value;
    } else if (flag == "--seed") {
      const auto seed = Number(value);
      if (!seed) return std::nullopt;
      args.seed = *seed;
    } else {
      return std::nullopt;
    }
  }
  if (!args.probe.empty()) return args;
  if (!args.with.empty()) {
    // Comparing two files only.
    if (args.compare.empty() || !args.scene.empty() ||
        !args.legacy_scenario.empty()) {
      return std::nullopt;
    }
    return args;
  }
  if (args.out.empty() || args.tick == 0 ||
      args.scene.empty() == args.legacy_scenario.empty()) {
    return std::nullopt;
  }
  return args;
}

// Compares |image| (from |name|) with the PNG |expected_path|.
int Compare(const TexturePayload& image, const std::string& name,
            const std::string& expected_path, std::string diff_path) {
  auto expected = LoadPng(expected_path);
  if (!expected) {
    std::fprintf(stderr, "render_shot: %s (%s)\n",
                 expected.error().message.c_str(),
                 expected.error().path.c_str());
    return 1;
  }
  const testing::ImageDiff diff = testing::CompareImages(image, *expected);
  std::printf("compare %s (%ux%u) with %s (%ux%u): %s\n", name.c_str(),
              image.width, image.height, expected_path.c_str(), expected->width,
              expected->height, diff.Describe().c_str());
  if (diff.Matches()) {
    std::printf("compare: match\n");
    return 0;
  }
  if (diff_path.empty()) diff_path = name + ".diff.png";
  if (auto written = WritePng(diff_path, testing::DiffImage(image, *expected));
      !written) {
    std::fprintf(stderr, "render_shot: %s\n", written.error().message.c_str());
  }
  std::printf("compare: MISMATCH; the difference is in %s\n",
              diff_path.c_str());
  return 1;
}

int CompareFiles(const Args& args) {
  auto image = LoadPng(args.compare);
  if (!image) {
    std::fprintf(stderr, "render_shot: %s (%s)\n",
                 image.error().message.c_str(), image.error().path.c_str());
    return 1;
  }
  return Compare(*image, args.compare, args.with, args.diff);
}

int Run(const Args& args) SE_REQUIRES(g_main) SE_EXCLUDES(g_sim_owner) {
  const std::string root =
      args.root.empty() ? testing::RampageRepoRoot() : args.root;
  testing::RampageSessionOptions options;
  options.scene = testing::SandboxSceneRequest();
  options.scene.legacy_root = root;
  options.scene.asset_root = root + "/assets";
  if (!args.legacy_scenario.empty()) {
    options.scene.format = rampage::SceneRequest::Format::kLegacyScenario;
    options.scene.path = args.legacy_scenario;
  } else {
    options.scene.path = args.scene;
  }
  if (args.legacy_rules == "v1") {
    options.scene.rules = rampage::RulesVersion::kV1;
  } else if (args.legacy_rules != "v2") {
    Usage();
    return 2;
  }
  options.seed = args.seed;
  options.ticks = args.tick;
  std::unique_ptr<testing::FrameListInput> input;
  if (!args.inputs.empty()) {
    auto frames = testing::ReadSeinput(args.inputs);
    if (!frames) {
      std::fprintf(stderr, "render_shot: %s\n", frames.error().message.c_str());
      return 1;
    }
    input = std::make_unique<testing::FrameListInput>(std::move(*frames));
    options.input = input.get();
  }

  // The RenderList of the last tick: the Transform (alpha 1), as the
  // legacy Sandbox draws the state after the tick.
  RenderList list;
  Camera2D camera;
  Tick drawn = 0;
  options.on_world = [&](const World& world) {
    if (world.Clock().tick != args.tick) return;
    camera = rampage::SandboxCamera(world);
    BuildRenderList(world, camera, 1.0F, list);
    drawn = world.Clock().tick;
  };
  const testing::RampageSessionResult session =
      testing::RunRampageSession(options);
  if (!session.error.empty()) {
    std::fprintf(stderr, "render_shot: %s\n", session.error.c_str());
    return 1;
  }
  if (drawn != args.tick) {
    std::fprintf(stderr,
                 "render_shot: the run never reached tick %" PRIu64 "\n",
                 static_cast<std::uint64_t>(args.tick));
    return 1;
  }

  SdlInitConfig sdl_config;
  sdl_config.subsystems = SDL_INIT_VIDEO;
  auto sdl = SdlContext::Init(sdl_config);
  if (!sdl) {
    std::fprintf(stderr, "render_shot: %s\n", sdl.error().message.c_str());
    return 1;
  }
  std::string driver = args.driver;
  if (driver.empty()) {
    const char* environment = SDL_getenv("SE_GPU_DRIVER");
    driver = ResolveGpuDriver("", environment != nullptr ? environment : "");
  }
  if (!DriverAvailable(driver)) {
    std::fprintf(stderr, "render_shot: the GPU driver %s is not available\n",
                 driver.c_str());
    return kSkipStatus;
  }
  int status = 0;
  {
    GpuDeviceConfig device_config;
    device_config.driver = driver;
    auto device = GpuDevice::Create(device_config);
    if (!device) {
      std::fprintf(stderr, "render_shot: %s\n", device.error().message.c_str());
      return 1;
    }
    auto renderer = Renderer2D::Create(*device);
    if (!renderer) {
      std::fprintf(stderr, "render_shot: %s\n",
                   renderer.error().message.c_str());
      return 1;
    }
    auto textures =
        LoadTextureTable((*renderer)->Resources(), root, "config/texture.json");
    if (!textures) {
      std::fprintf(stderr, "render_shot: %s (%s)\n",
                   textures.error().message.c_str(),
                   textures.error().path.c_str());
      return 1;
    }
    for (const Error& error : textures->errors) {
      std::fprintf(stderr, "render_shot: %s (%s)\n", error.message.c_str(),
                   error.path.c_str());
    }
    if (auto flushed = (*renderer)->Resources().FlushUploads(); !flushed) {
      std::fprintf(stderr, "render_shot: %s\n",
                   flushed.error().message.c_str());
      return 1;
    }
    (*renderer)->SetCompositeSize(
        static_cast<std::uint32_t>(camera.virtual_size.x),
        static_cast<std::uint32_t>(camera.virtual_size.y));
    RenderLayer layer;
    layer.list = &list;
    layer.camera = camera;
    const std::vector<RenderLayer> layers = {layer};
    FrameRequest request;
    request.layers = layers;
    request.clear = ClearColor{0.0F, 0.0F, 0.0F, 1.0F};
    request.present = false;
    const FrameResult result = (*renderer)->Frame(request);
    auto image = (*renderer)->Readback();
    if (!result.submitted || !image) {
      std::fprintf(stderr, "render_shot: the frame was not rendered\n");
      status = 1;
    } else if (auto written = WritePng(args.out, *image); !written) {
      std::fprintf(stderr, "render_shot: %s\n",
                   written.error().message.c_str());
      status = 1;
    } else if (!args.compare.empty()) {
      status = Compare(*image, args.out, args.compare, args.diff);
    } else {
      const std::string name(device->Driver());
      std::printf("image %s (%ux%u, %u quads, driver %s, tick %" PRIu64 ")\n",
                  args.out.c_str(), image->width, image->height,
                  list.stats.quads, name.c_str(),
                  static_cast<std::uint64_t>(args.tick));
    }
    renderer->reset();
  }
  return status;
}

}  // namespace
}  // namespace se

int Main(int argc, char** argv) {
  // A test tool: nothing waits for a click, SDL's assertions included
  // (ADR-070).
  se::SuppressFaultDialogs();
  const auto args = se::Parse(argc, argv);
  if (!args) {
    se::Usage();
    return 2;
  }
  if (!args->probe.empty()) {
    // SDL's driver checks need the video subsystem.
    const bool video = SDL_Init(SDL_INIT_VIDEO);
    const bool available = video && se::DriverAvailable(args->probe);
    std::printf("render_shot: %s is %savailable\n", args->probe.c_str(),
                available ? "" : "not ");
    SDL_Quit();
    return available ? 0 : se::kSkipStatus;
  }
  if (!args->with.empty()) return se::CompareFiles(*args);
  const se::testing::MainThreadScope main_thread;
  const int status = se::Run(*args);
  SDL_Quit();
  return status;
}

int main(int argc, char** argv) {
  // A tool may catch (ADR-006 exempts test and tool code).
  try {
    return Main(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "render_shot: %s\n", error.what());
    return 1;
  }
}
