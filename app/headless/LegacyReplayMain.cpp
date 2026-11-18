// legacy_replay: runs the legacy Sandbox scene from a scenario and recorded inputs, and writes
// its semantic projection (docs/milestone-specs.md, M1a). Run it from the repository root: the
// legacy tree reads config/ and assets/ relative to the working directory. --image-out TICK:FILE
// (M7) also writes the Sandbox scene at tick TICK as a PNG, for the engine's legacy image
// comparison (tools/render_shot).

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include <SFML/Graphics/Color.hpp>
#include <SFML/Graphics/Image.hpp>
#include <SFML/Graphics/RenderTexture.hpp>

#include "Engine.hpp"
#include "component/SceneViewRenderer.hpp"
#include "headless/ScenarioLoader.hpp"
#include "component/Sprite.hpp"
#include "scene/Sandbox.hpp"
#include "util/LegacyRandom.hpp"
#include "util/Logger.hpp"
#include "util/StressGenerator.hpp"

namespace
{
    struct Arguments
    {
        EngineOptions engine;
        std::string scenarioPath;
        std::string scenarioOut;
        std::size_t generate = 0;
        bool live = false;
        bool log = false;
        std::uint64_t imageTick = 0;
        std::string imagePath;
    };

    void printUsage()
    {
        std::fprintf(stderr,
            "usage: legacy_replay [--scenario FILE.json] [--inputs FILE.seinput] [--ticks N]\n"
            "                     [--projection-out FILE.seproj] [--fps HZ] [--seed N]\n"
            "                     [--live [--record FILE.seinput]] [--generate N] [--scenario-out FILE.json]\n"
            "                     [--log] [--image-out TICK:FILE]\n"
            "\n"
            "Runs the Sandbox for N ticks (default: the last tick of --inputs) with a hidden window\n"
            "and no wall clock, or paced in a visible window with --live, where the keyboard is\n"
            "sampled unless --inputs is given. --fps sets the frame rate: headless, a synthetic\n"
            "frame clock; live, the frame-rate limit. --generate adds N entities from the editor's\n"
            "seeded stress generator; --scenario-out writes the resulting scenario. Prints\n"
            "\"ticks <n>\", \"frames <rendered frames>\" and \"projection-hash <hex>\" (the run\n"
            "hash over ticks 0..n). --image-out runs to tick TICK (it replaces --ticks) and writes\n"
            "the Sandbox scene then, drawn as Sandbox::render draws it, into a render texture of the\n"
            "configured resolution cleared to black, as a PNG.\n");
    }

    bool parseUnsigned(const char* text, std::uint64_t& out)
    {
        char* end = nullptr;
        out = std::strtoull(text, &end, 0);
        return end != text && *end == '\0';
    }

    // TICK:FILE, TICK >= 1; FILE may hold a drive letter's colon.
    bool parseImageOut(std::string_view value, Arguments& args)
    {
        const std::size_t colon = value.find(':');
        if (colon == std::string_view::npos || colon + 1 >= value.size())
            return false;
        const std::string tick(value.substr(0, colon));
        if (!parseUnsigned(tick.c_str(), args.imageTick) || args.imageTick == 0)
            return false;
        args.imagePath = std::string(value.substr(colon + 1));
        return true;
    }

    // Draws the Sandbox's sprites as Sandbox::render does (registry view order) into a fresh
    // render texture of the configured resolution and saves it.
    bool writeImage(sf::RenderTexture& target, entt::registry& reg, const std::string& path)
    {
        target.clear(sf::Color::Black);
        const auto& view = reg.view<Sprite>();
        for (const auto& entity : view)
        {
            if (reg.valid(entity))
                target.draw(view.get<Sprite>(entity));
        }
        target.display();
        return target.getTexture().copyToImage().saveToFile(path);
    }

    bool parseArguments(int argc, char** argv, Arguments& args)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
            std::uint64_t number = 0;

            if (arg == "--live") { args.live = true; continue; }
            if (arg == "--log") { args.log = true; continue; }
            if (!value)
                return false;
            ++i;

            if (arg == "--scenario") args.scenarioPath = value;
            else if (arg == "--inputs") args.engine.replayPath = value;
            else if (arg == "--record") args.engine.recordPath = value;
            else if (arg == "--projection-out") args.engine.projectionPath = value;
            else if (arg == "--scenario-out") args.scenarioOut = value;
            else if (arg == "--ticks" && parseUnsigned(value, number)) args.engine.maxTicks = number;
            else if (arg == "--fps" && parseUnsigned(value, number) && number <= 1000) args.engine.fpsOverride = static_cast<unsigned int>(number);
            else if (arg == "--seed" && parseUnsigned(value, number)) args.engine.seed = number;
            else if (arg == "--generate" && parseUnsigned(value, number)) args.generate = static_cast<std::size_t>(number);
            else if (arg == "--image-out" && parseImageOut(value, args)) {}
            else return false;
        }
        return true;
    }
}

int main(int argc, char** argv)
{
    Arguments args;
    if (!parseArguments(argc, argv, args))
    {
        printUsage();
        return 2;
    }

    if (args.log)
        Logger::getInstance().setupConsoleLog();
    else
        Logger::getInstance().toggleLogging(false);

    args.engine.headless = !args.live;
    args.engine.computeProjection = true;
    if (args.imageTick != 0)
    {
        if (args.live)
        {
            std::fprintf(stderr, "legacy_replay: --image-out needs a headless run (no --live)\n");
            return 2;
        }
        args.engine.maxTicks = args.imageTick;
    }

    Engine engine("config/config.json", std::make_unique<Sandbox>(), args.engine);
    if (!engine.ok())
    {
        std::fprintf(stderr, "legacy_replay: the engine did not start (config/config.json, input or output files; run with --log)\n");
        return 1;
    }

    ApplicationContext& ctx = engine.context();
    Sandbox& sandbox = *static_cast<Sandbox*>(ctx.sceneManager.getActiveScene());

    Scenario scenario;
    std::string error;
    if (!args.scenarioPath.empty() && !ScenarioLoader::load(args.scenarioPath, scenario, error))
    {
        std::fprintf(stderr, "legacy_replay: %s\n", error.c_str());
        return 1;
    }

    const float width = static_cast<float>(ctx.configData.get<int>("width").value_or(1920));
    const float height = static_cast<float>(ctx.configData.get<int>("height").value_or(1080));
    if (args.generate != 0)
    {
        // The first press of the editor's Generate button in a session with the same seed.
        std::mt19937 rng = makeLegacyRng(ctx.seed, RngStream::StressGenerator);
        ScenarioLoader::appendStressEntities(scenario, generateStressPaths(rng, width, height, args.generate));
    }

    if (!args.scenarioOut.empty() && !ScenarioLoader::save(args.scenarioOut, scenario, error))
    {
        std::fprintf(stderr, "legacy_replay: %s\n", error.c_str());
        return 1;
    }

    if (!ScenarioLoader::spawn(scenario, sandbox, ctx, error))
    {
        std::fprintf(stderr, "legacy_replay: %s: %s\n", args.scenarioPath.c_str(), error.c_str());
        return 1;
    }

    // Draw the scene into an off-screen target every frame, as the editor's scene view does, so
    // the number of rendered frames is exercised (it must not change the simulation).
    entt::registry& reg = sandbox.getRegistry();
    reg.emplace<SceneViewRenderer>(reg.create(), static_cast<unsigned int>(width), static_cast<unsigned int>(height));

    // Created while the engine's hidden window provides the OpenGL context.
    sf::RenderTexture imageTarget;
    if (args.imageTick != 0 && !imageTarget.resize({ static_cast<unsigned int>(width), static_cast<unsigned int>(height) }))
    {
        std::fprintf(stderr, "legacy_replay: cannot create the %gx%g render texture for --image-out\n", width, height);
        return 1;
    }

    engine.run();

    if (args.imageTick != 0)
    {
        if (engine.ticksRun() != args.imageTick)
        {
            std::fprintf(stderr, "legacy_replay: the run ended at tick %" PRIu64 ", before --image-out's tick %" PRIu64 "\n",
                engine.ticksRun(), args.imageTick);
            return 1;
        }
        if (!writeImage(imageTarget, reg, args.imagePath))
        {
            std::fprintf(stderr, "legacy_replay: cannot write %s\n", args.imagePath.c_str());
            return 1;
        }
        std::printf("image %s\n", args.imagePath.c_str());
    }

    std::printf("ticks %" PRIu64 "\nframes %" PRIu64 "\nprojection-hash %016" PRIx64 "\n",
        engine.ticksRun(), engine.framesRendered(), engine.projectionHash());
    return 0;
}
