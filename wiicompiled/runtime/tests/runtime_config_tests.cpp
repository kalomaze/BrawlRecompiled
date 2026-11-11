#include "runtime_config.h"

#include <iostream>
#include <sstream>

namespace {
bool Require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}
} // namespace

int main() {
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = true\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "enabled.toml");
        if (!Require(config.metalFxSpatialUpscaling == std::optional<bool>{true},
                     "valid MetalFX setting was not loaded")) return 1;
    }
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = false\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "disabled.toml");
        if (!Require(config.metalFxSpatialUpscaling == std::optional<bool>{false},
                     "false MetalFX setting was not loaded")) return 1;
    }
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = \"yes\"\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "invalid.toml");
        if (!Require(!config.metalFxSpatialUpscaling, "invalid MetalFX setting was accepted")) return 1;
    }
    for (const char* value : {"nan", "+nan", "-nan", "inf", "+inf", "-inf", "1e100"}) {
        std::istringstream input(std::string("[audio]\nvolume = ") + value + "\n");
        const auto document = toml::parse(input, "nonfinite.toml");
        if (!Require(!RuntimeConfigFile::FindConfigFloat(document, "audio", "volume"),
                     "non-finite or overflowing float setting was accepted")) return 1;
    }
    {
        std::istringstream input("[audio]\nvolume = 0.5\n");
        const auto config = RuntimeConfigFile::ParseConfig(input, "volume.toml");
        if (!Require(config.audioVolume == std::optional<float>{0.5f},
                     "valid audio volume was not loaded")) return 1;
    }
    return 0;
}
