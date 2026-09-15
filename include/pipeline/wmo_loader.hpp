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
    uint32_t flags;
    uint32_t shader;
    uint32_t blendMode;
    uint32_t texture1;          // Diffuse texture index
    uint32_t color1;
    uint32_t texture2;          // Environment/detail texture
    uint32_t color2;
    uint32_t texture3;
    uint32_t color3;
    float runtime[4];           // Runtime data
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
    uint16_t flags = 0;        // low bits: split axis, bit 0x4: leaf
    int16_t negativeChild = -1;
    int16_t positiveChild = -1;
    uint16_t faceCount = 0;
    uint32_t firstFace = 0;    // index into MOBR
    float planeDistance = 0.0f;
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
