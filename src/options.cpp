//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "options.h"

#include "core/error.h"
#include "core/platform_win32.h"

#include <charconv>
#include <cstdio>
#include <optional>
#include <sstream>
#include <vector>

namespace
{

uint32_t ParseUint(std::string_view text, std::string_view flag)
{
    uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    {
        Fatal("%.*s expects an unsigned integer, got '%.*s'.", static_cast<int>(flag.size()), flag.data(),
              static_cast<int>(text.size()), text.data());
    }
    return value;
}

constexpr std::string_view kFromLeavesPrefix = "from-leaves:";
constexpr std::string_view kFromRootPrefix = "from-root:";

// Parses a SceneLevel written as "from-leaves:<n>" or "from-root:<n>". Returns nullopt when
// `text` has neither prefix, so callers can try other spellings first.
std::optional<SceneLevel> ParseSceneLevel(std::string_view text, std::string_view flag)
{
    if (text.starts_with(kFromLeavesPrefix))
    {
        return SceneLevel{SceneLevelOrigin::FromLeaves, ParseUint(text.substr(kFromLeavesPrefix.size()), flag)};
    }
    if (text.starts_with(kFromRootPrefix))
    {
        return SceneLevel{SceneLevelOrigin::FromRoot, ParseUint(text.substr(kFromRootPrefix.size()), flag)};
    }

    // The old "scenes:<signed-depth>" spelling meant a height when non-negative and a root depth
    // when negative. Name its replacement rather than just rejecting it.
    constexpr std::string_view kLegacyPrefix = "scenes:";
    if (text.starts_with(kLegacyPrefix))
    {
        const std::string_view depthText = text.substr(kLegacyPrefix.size());
        int64_t depth = 0;
        const auto result = std::from_chars(depthText.data(), depthText.data() + depthText.size(), depth);
        if (result.ec == std::errc{} && result.ptr == depthText.data() + depthText.size())
        {
            // Unsigned negation, so the most negative int64_t does not overflow.
            const uint64_t distance = depth >= 0 ? static_cast<uint64_t>(depth) : 0 - static_cast<uint64_t>(depth);
            Fatal("%.*s %.*s is no longer supported; use %s%llu.", static_cast<int>(flag.size()), flag.data(),
                  static_cast<int>(text.size()), text.data(), depth >= 0 ? "from-leaves:" : "from-root:",
                  static_cast<unsigned long long>(distance));
        }
        Fatal("%.*s scenes:<depth> is no longer supported; use from-leaves:<n> or from-root:<n>.",
              static_cast<int>(flag.size()), flag.data());
    }
    return std::nullopt;
}

float ParseFloat(std::string_view text, std::string_view flag)
{
    try
    {
        return std::stof(std::string(text));
    }
    catch (const std::exception&)
    {
        Fatal("%.*s expects a number, got '%.*s'.", static_cast<int>(flag.size()), flag.data(),
              static_cast<int>(text.size()), text.data());
    }
}

std::vector<std::string> Split(std::string_view text, char separator)
{
    std::vector<std::string> parts;
    std::string current;
    std::istringstream stream{std::string(text)};
    while (std::getline(stream, current, separator))
    {
        parts.push_back(current);
    }
    return parts;
}

} // namespace

std::string Options::Usage()
{
    return "Usage: AccelerationStructurePlayground <scene.gltf|scene.glb> [options]\n"
           "\n"
           "  --out <file.png>        Render headless to a PNG file and exit.\n"
           "  --benchmark <n>         Run n measured frames headless and print average GPU times.\n"
           "  --warmup <n>            Benchmark warmup frames (default 2; zero allowed).\n"
           "  --width <n>             Render width  (default 1280).\n"
           "  --height <n>            Render height (default 720).\n"
           "  --time <seconds>        Animation time to evaluate (default 0).\n"
           "  --frames <n>            Frames to render before writing the PNG (default 1).\n"
           "  --blas-level <level>    meshes | hybrid | from-leaves:<n> | from-root:<n> (default meshes).\n"
           "                          from-leaves/from-root group by scene instance, at height n or root depth n.\n"
           "  --mdlas-level <level>   from-leaves:<n> | from-root:<n>. Add an MDLAS scene-grouping level;\n"
           "                          repeat at most twice.\n"
           "  --deform <mode>         auto | compute | transform (default auto).\n"
           "  --max-bounces <n>       Reflection ray bounces (default 3, 0 disables reflections).\n"
           "  --sun <x,y,z>           Direction the sunlight travels (default -0.45,-0.8,-0.4).\n"
           "  --camera <ex,ey,ez,tx,ty,tz>  Explicit eye and target; default frames the scene bounds.\n"
           "  --no-vsync              Disable vertical sync in the interactive path.\n"
           "  --mdlas-full-barriers   Debug: build MDLASes one at a time with a full memory barrier after each.\n"
           "  --debug                 Enable the Vulkan validation layers.\n"
           "  --help, -h              Print this message and exit.\n";
}

Options Options::Parse(int argc, char** argv)
{
    Options options;

    auto requireValue = [&](int& i, std::string_view flag) -> std::string_view {
        if (i + 1 >= argc)
        {
            Fatal("%.*s requires a value.\n\n%s", static_cast<int>(flag.size()), flag.data(), Usage().c_str());
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument = argv[i];

        if (argument == "--help" || argument == "-h")
        {
            printf("%s", Usage().c_str());
            options.helpRequested = true;
            return options;
        }

        if (!argument.starts_with("--"))
        {
            if (!options.scenePath.empty())
            {
                Fatal("Unexpected extra positional argument '%.*s'.", static_cast<int>(argument.size()),
                      argument.data());
            }
            options.scenePath = argument;
        }
        else if (argument == "--out")
        {
            options.outputPath = Widen(requireValue(i, argument));
        }
        else if (argument == "--benchmark")
        {
            options.benchmarkFrames = ParseUint(requireValue(i, argument), argument);
            if (options.benchmarkFrames == 0)
            {
                Fatal("--benchmark requires at least one frame.");
            }
        }
        else if (argument == "--warmup")
        {
            options.warmupFrames = ParseUint(requireValue(i, argument), argument);
        }
        else if (argument == "--width")
        {
            options.width = ParseUint(requireValue(i, argument), argument);
        }
        else if (argument == "--height")
        {
            options.height = ParseUint(requireValue(i, argument), argument);
        }
        else if (argument == "--time")
        {
            options.time = ParseFloat(requireValue(i, argument), argument);
        }
        else if (argument == "--frames")
        {
            options.frames = ParseUint(requireValue(i, argument), argument);
        }
        else if (argument == "--max-bounces")
        {
            options.maxBounces = ParseUint(requireValue(i, argument), argument);
        }
        else if (argument == "--blas-level")
        {
            const std::string_view value = requireValue(i, argument);
            AccelStructGrouping grouping;
            if (value == "meshes")
            {
                grouping.target = AccelStructGroupingTarget::Meshes;
            }
            else if (value == "hybrid")
            {
                grouping.target = AccelStructGroupingTarget::Hybrid;
            }
            else if (const std::optional<SceneLevel> sceneLevel = ParseSceneLevel(value, argument))
            {
                grouping.target = AccelStructGroupingTarget::Scenes;
                grouping.sceneLevel = *sceneLevel;
            }
            else
            {
                Fatal("Unknown --blas-level '%.*s'. Expected meshes, hybrid, from-leaves:<n> or from-root:<n>.",
                      static_cast<int>(value.size()), value.data());
            }
            options.accelStructPlan.blasLevel = grouping;
        }
        // Accept old name for MDLAS too
        else if ((argument == "--mdlas-level") || (argument == "--mlas-level"))
        {
            if (options.accelStructPlan.mlasLevels.size() >= kMaxMlasLevels)
            {
                Fatal("At most %u --mdlas-level options are supported.", kMaxMlasLevels);
            }
            const std::string_view value = requireValue(i, argument);
            const std::optional<SceneLevel> sceneLevel = ParseSceneLevel(value, argument);
            if (!sceneLevel)
            {
                Fatal("Unknown --mdlas-level '%.*s'. Expected from-leaves:<n> or from-root:<n>.",
                      static_cast<int>(value.size()), value.data());
            }
            options.accelStructPlan.mlasLevels.push_back(*sceneLevel);
        }
        else if (argument == "--deform")
        {
            const std::string_view value = requireValue(i, argument);
            if (value == "auto")
            {
                options.deformMode = DeformMode::Auto;
            }
            else if (value == "compute")
            {
                options.deformMode = DeformMode::Compute;
            }
            else if (value == "transform")
            {
                options.deformMode = DeformMode::Transform;
            }
            else
            {
                Fatal("Unknown --deform '%.*s'. Expected auto, compute or transform.", static_cast<int>(value.size()),
                      value.data());
            }
        }
        else if (argument == "--sun")
        {
            const std::vector<std::string> parts = Split(requireValue(i, argument), ',');
            if (parts.size() != 3)
            {
                Fatal("--sun expects three comma-separated components.");
            }
            for (size_t c = 0; c < 3; ++c)
            {
                options.sunDirection[c] = ParseFloat(parts[c], argument);
            }
        }
        else if (argument == "--camera")
        {
            const std::vector<std::string> parts = Split(requireValue(i, argument), ',');
            if (parts.size() != 6)
            {
                Fatal("--camera expects six comma-separated components (eye then target).");
            }
            std::array<float, 6> values{};
            for (size_t c = 0; c < 6; ++c)
            {
                values[c] = ParseFloat(parts[c], argument);
            }
            options.camera = values;
        }
        else if (argument == "--no-vsync")
        {
            options.vsync = false;
        }
        // Accept old name for MDLAS too
        else if ((argument == "--mdlas-full-barriers") || (argument == "--mlas-full-barriers"))
        {
            options.mlasFullBarriers = true;
        }
        else if (argument == "--debug")
        {
            options.debugLayer = true;
        }
        else
        {
            Fatal("Unknown option '%.*s'.\n\n%s", static_cast<int>(argument.size()), argument.data(), Usage().c_str());
        }
    }

    if (options.width == 0 || options.height == 0)
    {
        Fatal("--width and --height must be greater than zero.");
    }
    if (options.benchmarkFrames != 0 && !options.outputPath.empty())
    {
        Fatal("--benchmark and --out cannot be used together.");
    }
    if (options.scenePath.empty())
    {
        Fatal("No glTF scene file given.\n\n%s", Usage().c_str());
    }

    return options;
}
