// legacy_replay: runs the legacy Sandbox scene from a scenario and recorded inputs, and writes
// its semantic projection (docs/milestone-specs.md, M1a). Run it from the repository root: the
// legacy tree reads config/ and assets/ relative to the working directory.

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include "Engine.hpp"
#include "component/SceneViewRenderer.hpp"
#include "headless/ScenarioLoader.hpp"
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
    };

    void printUsage()
    {
        std::fprintf(stderr,
            "usage: legacy_replay [--scenario FILE.json] [--inputs FILE.seinput] [--ticks N]\n"
            "                     [--projection-out FILE.seproj] [--fps HZ] [--seed N]\n"
            "                     [--live [--record FILE.seinput]] [--generate N] [--scenario-out FILE.json]\n"
            "                     [--log]\n"
            "\n"
            "Runs the Sandbox for N ticks (default: the last tick of --inputs) with a hidden window\n"
            "and no wall clock, or paced in a visible window with --live, where the keyboard is\n"
            "sampled unless --inputs is given. --fps sets the frame rate: headless, a synthetic\n"
            "frame clock; live, the frame-rate limit. --generate adds N entities from the editor's\n"
            "seeded stress generator; --scenario-out writes the resulting scenario. Prints\n"
            "\"ticks <n>\", \"frames <rendered frames>\" and \"projection-hash <hex>\" (the run\n"
            "hash over ticks 0..n).\n");
    }

    bool parseUnsigned(const char* text, std::uint64_t& out)
    {
        char* end = nullptr;
        out = std::strtoull(text, &end, 0);
        return end != text && *end == '\0';
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

    engine.run();

    std::printf("ticks %" PRIu64 "\nframes %" PRIu64 "\nprojection-hash %016" PRIx64 "\n",
        engine.ticksRun(), engine.framesRendered(), engine.projectionHash());
    return 0;
}
