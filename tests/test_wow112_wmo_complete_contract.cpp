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
    const auto loaderH = read("include/pipeline/wmo_loader.hpp");
    const auto loader  = read("src/pipeline/wmo_loader.cpp");
    const auto wmoH    = read("include/rendering/wmo_renderer.hpp");
    const auto wmo     = read("src/rendering/wmo_renderer.cpp");
    const auto terrain = read("src/rendering/terrain_manager.cpp");
    const auto m2H     = read("include/rendering/m2_renderer.hpp");
    const auto m2V     = read("assets/shaders/m2.vert.glsl");
    const auto m2F     = read("assets/shaders/m2.frag.glsl");

    // Root Vanilla WMO records.
    assert(loader.find("constexpr uint32_t kLightSize = 48") != std::string::npos);
    assert(loader.find("light.lightType = read<uint8_t>") != std::string::npos);
    assert(loader.find("light.color = unpackBGRA") != std::string::npos);
    assert(loader.find("constexpr uint32_t kPortalSize = 20") != std::string::npos);
    assert(loader.find("portal.plane.w = read<float>") != std::string::npos);
    assert(loader.find("constexpr uint32_t kFogSize = 48") != std::string::npos);

    // Exact 68-byte Vanilla MOGP header.
    assert(loader.find("group.batchCountA = read<uint16_t>") != std::string::npos);
    assert(loader.find("group.batchCountD = read<uint16_t>") != std::string::npos);
    assert(loader.find("group.groupId = read<uint32_t>") != std::string::npos);
    assert(loaderH.find("uint16_t batchCountA") != std::string::npos);

    // Missing group chunks now survive parsing.
    assert(loader.find("subChunkId == MOLR") != std::string::npos);
    assert(loader.find("subChunkId == MODR") != std::string::npos);
    assert(loader.find("subChunkId == MOBN") != std::string::npos);
    assert(loader.find("subChunkId == MOBR") != std::string::npos);

    // Collision grid is restricted to authored MOBR faces when BSP is present.
    assert(wmo.find("bspCollisionFaceMask") != std::string::npos);
    assert(wmo.find("!bspCollisionFaceMask[triIndex]") != std::string::npos);

    // MOPT plane is used directly; MFOG reaches frame fog.
    assert(wmo.find("authoredNormal(portal.plane.x") != std::string::npos);
    assert(wmo.find("queryVanillaFog") != std::string::npos);
    assert(wmoH.find("VanillaFogSample") != std::string::npos);

    // MODD per-instance colour reaches the M2 shader.
    assert(terrain.find("doodadReady.color = doodad.color") != std::string::npos);
    assert(terrain.find("setInstanceColor(wmoDoodadInstId") != std::string::npos);
    assert(m2H.find("glm::vec4 instanceColor") != std::string::npos);
    assert(m2H.find("112 bytes") != std::string::npos);
    assert(m2V.find("vec4 instanceColor") != std::string::npos);
    assert(m2F.find("vInstanceColor") != std::string::npos);

    // Do not leak post-Vanilla MCVP into the 1.12 path.
    assert(loader.find("MCVP") == std::string::npos);
    assert(loaderH.find("convexVolumePlanes") == std::string::npos);

    return 0;
}
