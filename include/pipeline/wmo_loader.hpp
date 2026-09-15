#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace wowee {
namespace pipeline {

/// Retail 1.12's group classifier: a true interior has neither the EXTERIOR
/// bit (0x08) nor the exterior-lit bit (0x40). Do not use later-era 0x2000 as
/// an indoor bit for Vanilla data.
inline constexpr bool wmoGroupIsInterior112(uint32_t flags) {
    return (flags & 0x48u) == 0;
}

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
    // Exact Vanilla 1.12 MOMT disk fields. The record is 64 bytes, but only
    // TWO texture-name offsets exist in the file: +0x0c and +0x18. The final
    // two dwords (+0x38/+0x3c) are runtime texture handles in WoW.exe, not a
    // third authored texture.
    uint32_t flags = 0;             // +0x00
    uint32_t shader = 0;            // +0x04
    uint32_t blendMode = 0;         // +0x08
    uint32_t texture1 = 0;          // +0x0c MOTX byte offset
    uint32_t sidnColor = 0;         // +0x10
    uint32_t frameSidnColor = 0;    // +0x14
    uint32_t texture2 = 0;          // +0x18 MOTX byte offset
    uint32_t diffColor = 0;         // +0x1c
    uint32_t groundType = 0;        // +0x20 TerrainType.dbc id
    uint32_t color2 = 0;            // +0x24
    uint32_t raw28 = 0;             // +0x28
    uint32_t raw2C = 0;             // +0x2c
    uint32_t raw30 = 0;             // +0x30
    uint32_t raw34 = 0;             // +0x34
    uint32_t runtimeTexture1 = 0;   // +0x38 overwritten by retail loader
    uint32_t runtimeTexture2 = 0;   // +0x3c overwritten by retail loader
};

// WMO Group Info
struct WMOGroupInfo {
    uint32_t flags;
    glm::vec3 boundingBoxMin;
    glm::vec3 boundingBoxMax;
    int32_t nameOffset;         // Group name in MOGN chunk
};

// WMO Light
struct WMOLight {
    // Exact 48-byte Vanilla MOLT record.
    uint8_t lightType = 0;          // +0x00: omni/spot/directional/ambient
    uint8_t type = 0;               // +0x01
    uint8_t useAttenuation = 0;     // +0x02
    uint8_t pad = 0;                // +0x03
    uint32_t packedColor = 0;       // +0x04, CImVector/BGRA bytes
    glm::vec4 color{1.0f};          // decoded runtime RGBA
    glm::vec3 position{0.0f};       // +0x08
    float intensity = 1.0f;         // +0x14
    float attenuationStart = 0.0f;  // +0x18
    float attenuationEnd = 0.0f;    // +0x1c
    float unknown[4]{};             // +0x20..+0x2c
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

// WMO Fog
struct WMOFog {
    // Exact Vanilla MFOG record, 0x30 bytes.
    uint32_t flags = 0;
    glm::vec3 position{0.0f};
    float smallRadius = 0.0f;
    float largeRadius = 0.0f;
    float endDist = 0.0f;
    float startFactor = 0.0f;       // fraction of endDist, not a distance
    uint32_t packedColor = 0;       // 0xAARRGGBB as a little-endian CImVector
    float underwaterEndDist = 0.0f;
    float underwaterStartFactor = 0.0f;
    uint32_t packedUnderwaterColor = 0;
};

// WMO Portal
struct WMOPortal {
    // Exact Vanilla MOPT record, 20 bytes.
    uint16_t startVertex = 0;
    uint16_t vertexCount = 0;
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    float distance = 0.0f; // signed plane: dot(normal, p) + distance
};

// WMO Portal Plane
struct WMOPortalPlane {
    glm::vec3 normal;
    float distance;
};

// WMO Portal Reference (MOPR chunk) - links portals to groups
struct WMOPortalRef {
    uint16_t portalIndex;   // Index into portals array
    uint16_t groupIndex;    // Group on other side of portal
    int16_t side;           // Which side of portal plane (-1 or 1)
    uint16_t padding;
};

// Vanilla MOBN BSP node, exact 16-byte on-disk record.
struct WMOBSPNode {
    int16_t planeType = 0;
    int16_t children[2]{-1, -1};
    uint16_t faceCount = 0;
    uint16_t firstFace = 0;
    int16_t unknown = 0;
    float distance = 0.0f;
};

// WMO Liquid (MLIQ chunk data)
struct WMOLiquid {
    uint32_t xVerts = 0;        // Vertices in X direction
    uint32_t yVerts = 0;        // Vertices in Y direction
    uint32_t xTiles = 0;        // Tiles in X (= xVerts - 1)
    uint32_t yTiles = 0;        // Tiles in Y (= yVerts - 1)
    glm::vec3 basePosition;     // Corner position in model space
    uint16_t materialId = 0;    // Liquid material/type
    std::vector<float> heights; // Height per vertex (xVerts * yVerts)
    std::vector<uint8_t> opacity; // Authored first byte of each 8-byte MLIQ vertex
    std::vector<uint8_t> flags; // Flags per tile (xTiles * yTiles)

    [[nodiscard]] bool hasLiquid() const { return xVerts > 0 && yVerts > 0; }
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
    uint16_t batchCountA;
    uint16_t batchCountB;
    uint8_t fogIndices[4]{};    // MOGP +0x30, indices into root MFOG
    uint32_t liquidType = 0x0F;  // MOGP +0x34, 0x0F = no whole-group liquid
    uint32_t areaTableId = 0;    // MOGP +0x38, WMOAreaTable group id
    uint32_t groupId = 0;

    // Geometry
    std::vector<WMOVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<WMOBatch> batches;
    std::vector<uint8_t> triFlags;        // Per-triangle MOPY flags
    std::vector<uint8_t> triMaterialIds;  // Per-triangle MOPY material id; 0xFF = collision-only

    // Portals
    std::vector<WMOPortal> portals;
    std::vector<glm::vec3> portalVertices;

    // Group ownership and local-light references authored by Vanilla.
    std::vector<uint16_t> doodadRefs; // MODR -> root MODD indices
    std::vector<uint16_t> lightRefs;  // MOLR -> root MOLT indices

    // Exact WMO collision BSP: MOBN nodes + MOBR triangle-number refs.
    std::vector<WMOBSPNode> bspNodes;
    std::vector<uint16_t> bspFaceRefs;

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
    uint32_t rootId = 0;       // MOHD +0x20, WMOAreaTable WMOID
    uint32_t headerFlags = 0;  // MOHD +0x3c

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
    std::vector<WMOPortalPlane> portalPlanes;
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

    // Optional WMO-local skybox M2 (MOSB). Empty for almost all roots.
    std::string skyboxPath;

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
