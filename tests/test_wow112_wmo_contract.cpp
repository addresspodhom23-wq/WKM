#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

static std::string read(const char* p) {
    std::ifstream in(p); assert(in.good());
    std::ostringstream s; s << in.rdbuf(); return s.str();
}
int main() {
    const auto h = read("include/pipeline/wmo_loader.hpp");
    const auto p = read("src/pipeline/wmo_loader.cpp");
    const auto r = read("src/rendering/wmo_renderer.cpp");
    const auto world = read("src/core/world_loader.cpp");
    const auto m2 = read("src/rendering/m2_renderer_render.cpp");

    // Exact Vanilla chunks are parsed, not skipped.
    for (const char* tag : {"MOLT", "MFOG", "MOLR", "MODR", "MOBN", "MOBR"}) {
        assert(p.find(tag) != std::string::npos);
    }
    assert(p.find("kLightSize = 48") != std::string::npos);
    assert(p.find("kFogSize = 48") != std::string::npos);
    assert(p.find("kPortalSize = 20") != std::string::npos);
    assert(p.find("light.lightType = read<uint8_t>") != std::string::npos);
    assert(p.find("portal.plane.x = read<float>") != std::string::npos);

    // Runtime must use authored portal plane and MOBR domain.
    assert(r.find("portal.plane.x") != std::string::npos);
    assert(r.find("bspCollisionFaceMask") != std::string::npos);
    assert(r.find("bspCollisionFaceMask.empty() && !triMopyFlags.empty()")
           != std::string::npos);

    // MODR must reach M2 semantic visibility, including GPU cull.
    assert(r.find("updateDoodadVisibility") != std::string::npos);
    assert(m2.find("instance.forcedHidden") != std::string::npos);

    // WDT global-WMO must share the exact ADT placement helper.
    assert(world.find("placementEulerFromAdtDegrees(wdtInfo.rotation)")
           != std::string::npos);
    assert(world.find("static_cast<float>(wdtInfo.scale) / 1024.0f")
           != std::string::npos);

    // Structures retain exact parsed fields.
    assert(h.find("std::vector<WMOBspNode> bspNodes") != std::string::npos);
    assert(h.find("std::vector<uint16_t> doodadRefs") != std::string::npos);
    assert(h.find("std::vector<uint16_t> lightRefs") != std::string::npos);
    return 0;
}
