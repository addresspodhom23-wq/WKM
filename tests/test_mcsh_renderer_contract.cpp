#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

static std::string read(const char* path) {
    std::ifstream in(path);
    assert(in.good());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

int main() {
    const auto parser = read("src/pipeline/adt_loader.cpp");
    const auto renderer = read("src/rendering/terrain_renderer.cpp");
    const auto desktop = read("assets/shaders/terrain.frag.glsl");
    const auto mobile = read("assets/shaders/terrain_mobile.frag.glsl");

    assert(parser.find("ofsShadow = readUInt32(data, 44)") != std::string::npos);
    assert(parser.find("(chunk.flags & 0x1u)") != std::string::npos);
    assert(renderer.find("materialBindings[8].binding = 8") != std::string::npos);
    assert(renderer.find("createBakedShadowTexture") != std::string::npos);
    assert(desktop.find("binding = 8") != std::string::npos);
    assert(desktop.find("texture(uBakedShadow, LayerUV).r") != std::string::npos);
    assert(mobile.find("binding = 8") != std::string::npos);
    assert(mobile.find("texture(uBakedShadow, LayerUV).r") != std::string::npos);
    return 0;
}
