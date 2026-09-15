#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

static std::string read(const char* p) {
    std::ifstream in(p); assert(in.good());
    std::ostringstream s; s << in.rdbuf(); return s.str();
}
int main() {
    const auto frame=read("include/rendering/vk_frame_data.hpp");
    const auto renderer=read("src/rendering/renderer.cpp");
    const auto settings=read("src/ui/settings_panel.cpp");
    const auto terrain=read("assets/shaders/terrain.frag.glsl");
    const auto m2=read("assets/shaders/m2.frag.glsl");
    const auto wmo=read("assets/shaders/wmo.frag.glsl");

    assert(frame.find("w = Vanilla 1.12 renderer") != std::string::npos);
    assert(renderer.find("classicRendering_ ? 1.0f : 0.0f") != std::string::npos);
    assert(settings.find("setClassicRendering") != std::string::npos);
    assert(terrain.find("vanillaRendering = shadowParams.w > 0.5") != std::string::npos);
    assert(m2.find("vanillaRendering = shadowParams.w > 0.5") != std::string::npos);
    assert(wmo.find("vanillaRendering = shadowParams.w > 0.5") != std::string::npos);
    assert(wmo.find("result = texColor.rgb * VertColor.rgb") != std::string::npos);
    return 0;
}
