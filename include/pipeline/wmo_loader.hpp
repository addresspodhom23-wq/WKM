#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace wowee {
namespace pipeline {

/**
 * WMO (World Model Object) Format
 *
 * WMO files contain buildings, dungeons, and large structures.
 * Structure:
 * - Root WMO file: Contains groups, materials, doodad sets
 * - Group WMO files: Individual rooms/sections (_XXX.wmo)
 *
 * Reference: https://wowdev.wiki/WMO
 */

// WMO Material
struct WMOMaterial {
    // Exact Vanilla 1.12 MOMT record: 16 dwords / 64 bytes. Only texture1
    // (+0x0c) and texture2 (+0x18) are authored MOTX offsets. The last two
    // dwords are runtime texture handles written by the original client.
    uint32_t flags = 0;             // +0x00
    uint32_t shader = 0;            // +0x04
    uint32_t blendMode = 0;         // +0x08
    uint32_t texture1 = 0;          // +0x0c
    uint32_t sidnColor = 0;         // +0x10
    uint32_t frameSidnColor = 0;    // +0x14
    uint32_t texture2 = 0;          // +0x18
    uint32_t diffColor = 0;         // +0x1c
    uint32_t groundType = 0;        // +0x20
    uint32_t color2 = 0;            // +0x24
    uint32_t flags2 = 0;            // +0x28
    uint32_t raw2C = 0;             // +0x2c
    uint32_t raw30 = 0;             // +0x30
    uint32_t raw34 = 0;             // +0x34
    uint32_t runtimeTexture1 = 0;   // +0x38
    uint32_t runtimeTexture2 = 0;   // +0x3c
};

// WMO Group Info
struct WMOGroupInfo {
    uint32_t flags;
    glm::vec3 boundingBoxMin;
    glm::vec3 boundingBoxMax;
    int32_t nameOffset;         // Group name in MOGN chunk
};

// WMO Light (MOLT, Vanilla 1.12: 48 bytes on disk)
struct WMOLight {
    uint8_t lightType = 0;      // 0=omni, 1=spot, 2=directional, 3=ambient
    uint8_t type = 0;           // Vanilla auxiliary light flag
    uint8_t useAttenuation = 0;
    uint8_t pad = 0;
    glm::vec4 color{1.0f};      // unpacked BGRA CImVector -> RGBA
    glm::vec3 position{0.0f};
    float intensity = 1.0f;
    float attenuationStart = 0.0f;
    float attenuationEnd = 0.0f;
    float unknown[4] = {0, 0, 0, 0};
};

// WMO Doodad Set (collection of M2 models placed in WMO)
struct WMODoodadSet {
    char name[20];
    uint32_t startIndex;        // First doodad in MODD
    uint32_t count;             // Number of doodads
    uint32_t padding;
};

// WMO Doodad Instance
struct WMODoodad {
    uint32_t nameIndex;         // Index into MODN (doodad names)
    glm::vec3 position;
    glm::quat rotation;         // Quaternion rotation
    float scale;
    glm::vec4 color;           // BGRA color
};

// WMO Fog (MFOG, Vanilla 1.12: 48 bytes on disk)
struct WMOFog {
    uint32_t flags = 0;
    glm::vec3 position{0.0f};
    float smallRadius = 0.0f;
    float largeRadius = 0.0f;
    float endDist = 0.0f;
    float startFactor = 0.0f;
    glm::vec4 color1{0.0f};    // packed BGRA -> RGBA
    float endDist2 = 0.0f;
    float startFactor2 = 0.0f;
    glm::vec4 color2{0.0f};    // underwater fog
};

// WMO Portal (MOPT, Vanilla 1.12: 20 bytes on disk)
struct WMOPortal {
    uint16_t startVertex = 0;
    uint16_t vertexCount = 0;
    glm::vec4 plane{0.0f};     // xyz normal, w plane distance
};

// WMO Portal Reference (MOPR chunk) - links portals to groups
struct WMOPortalRef {
    uint16_t portalIndex;   // Index into portals array
    uint16_t groupIndex;    // Group on other side of portal
    int16_t side;           // Which side of portal plane (-1 or 1)
    uint16_t padding;
};

// WMO collision BSP node (MOBN, Vanilla 1.12: 16 bytes)
struct WMOBspNode {
    uint16_t planeType = 0;
    int16_t negativeChild = -1;
    int16_t positiveChild = -1;
    uint16_t faceCount = 0;
    uint16_t firstFace = 0;    // first entry in MOBR
    int16_t unknown = 0;
    float planeDistance = 0.0f;
};

// One exact Vanilla MLIQ vertex: 4 interpretation-dependent bytes + height.
struct WMOLiquidVertex {
    uint8_t flow1 = 0;
    uint8_t flow2 = 0;
    uint8_t flow1Pct = 0;
    uint8_t filler = 0;
    float height = 0.0f;

    [[nodiscard]] int16_t magmaS() const {
        return static_cast<int16_t>(
            static_cast<uint16_t>(flow1) |
            (static_cast<uint16_t>(flow2) << 8));
    }
    [[nodiscard]] int16_t magmaT() const {
        return static_cast<int16_t>(
            static_cast<uint16_t>(flow1Pct) |
            (static_cast<uint16_t>(filler) << 8));
    }
};

// WMO Liquid (MLIQ, Vanilla 1.12)
struct WMOLiquid {
    uint32_t xVerts = 0;
    uint32_t yVerts = 0;
    uint32_t xTiles = 0;
    uint32_t yTiles = 0;
    glm::vec3 basePosition{0.0f};
    uint16_t materialId = 0;       // raw MLIQ material field
    uint16_t liquidTypeId = 0;     // resolved Vanilla LiquidType ID
    std::vector<WMOLiquidVertex> vertices;
    std::vector<float> heights;    // convenience mirror for queries/render upload
    std::vector<uint8_t> flags;    // SMOLTile bytes

    [[nodiscard]] bool hasLiquid() const {
        return xVerts > 0 && yVerts > 0 &&
               vertices.size() == static_cast<size_t>(xVerts) * yVerts;
    }
};

// WMO Group Vertex
struct WMOVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 texCoord;
    glm::vec4 color;           // Vertex color
};

// WMO Batch (render batch)
struct WMOBatch {
    uint32_t startIndex;   // First index (this is uint32 in file format)
    uint16_t indexCount;   // Number of indices
    uint16_t startVertex;
    uint16_t lastVertex;
    uint8_t flags;
    uint8_t materialId;
};

// WMO Group (individual room/section)
struct WMOGroup {
    uint32_t flags;
    glm::vec3 boundingBoxMin;
    glm::vec3 boundingBoxMax;
    uint16_t portalStart;
    uint16_t portalCount;
    uint16_t batchCountA = 0;
    uint16_t batchCountB = 0;
    uint16_t batchCountC = 0;
    uint16_t batchCountD = 0;
    uint8_t fogIndices[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t liquidType = 0;
    uint32_t groupId = 0;       // WMOAreaTable area/group id

    // Geometry
    std::vector<WMOVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<WMOBatch> batches;
    std::vector<uint8_t> triFlags;        // Per-triangle MOPY flags
    std::vector<uint8_t> triMaterialIds;  // Per-triangle MOPY material id; 0xFF = collision-only

    // Per-group references into root tables.
    std::vector<uint16_t> lightRefs;   // MOLR -> WMOModel::lights
    std::vector<uint16_t> doodadRefs;  // MODR -> WMOModel::doodads

    // BSP tree used by the original client for WMO collision.
    std::vector<WMOBspNode> bspNodes;      // MOBN
    std::vector<uint16_t> bspFaceIndices;  // MOBR -> triangle indices

    // Liquid data (MLIQ chunk)
    WMOLiquid liquid;

    std::string name;
    std::string description;
};

// Complete WMO Model
struct WMOModel {
    // Runtime source path when known. The binary format does not store its root
    // filename, but a few rendering classifications need the owning WMO family.
    std::string sourcePath;

    // Root WMO data (from MOHD chunk)
    uint32_t version;
    uint32_t nTextures;  // Added - was missing, caused offset issues
    uint32_t nGroups;
    uint32_t nPortals;
    uint32_t nLights;
    uint32_t nDoodadNames;
    uint32_t nDoodadDefs;
    uint32_t nDoodadSets;
    uint16_t headerFlags = 0;     // MOHD +0x3c
    uint16_t numLod = 0;          // MOHD +0x3e (zero in Vanilla assets)

    glm::vec3 ambientColor;     // MOHD ambient color (used for interior group lighting)
    glm::vec3 boundingBoxMin;
    glm::vec3 boundingBoxMax;

    // Materials and textures
    std::vector<WMOMaterial> materials;
    std::vector<std::string> textures;
    std::unordered_map<uint32_t, uint32_t> textureOffsetToIndex;  // MOTX offset -> texture array index

    // Groups (rooms/sections)
    std::vector<WMOGroupInfo> groupInfo;
    std::vector<WMOGroup> groups;

    // Portals (visibility culling)
    std::vector<WMOPortal> portals;
    std::vector<glm::vec3> portalVertices;
    std::vector<WMOPortalRef> portalRefs;  // MOPR chunk - portal-to-group links

    // Lights
    std::vector<WMOLight> lights;

    // Doodads (M2 models placed in WMO)
    // Keyed by byte offset into MODN chunk (nameIndex in MODD references these offsets)
    std::unordered_map<uint32_t, std::string> doodadNames;
    std::vector<WMODoodad> doodads;
    std::vector<WMODoodadSet> doodadSets;

    // Fog
    std::vector<WMOFog> fogs;

    // Group names
    std::vector<std::string> groupNames;
    std::vector<uint8_t> groupNameRaw;  // Raw MOGN chunk for offset-based name lookup

    [[nodiscard]] bool isValid() const {
        return nGroups > 0 && !groups.empty();
    }
};

class WMOLoader {
public:
    /**
     * Load root WMO file
     *
     * @param wmoData Raw WMO file bytes
     * @return Parsed WMO model (without group geometry)
     */
    static WMOModel load(const std::vector<uint8_t>& wmoData);

    /**
     * Load WMO group file
     *
     * @param groupData Raw WMO group file bytes
     * @param model Model to populate with group data
     * @param groupIndex Group index to load
     * @return True if successful
     */
    static bool loadGroup(const std::vector<uint8_t>& groupData,
                         WMOModel& model,
                         uint32_t groupIndex);
};

} // namespace pipeline
} // namespace wowee
