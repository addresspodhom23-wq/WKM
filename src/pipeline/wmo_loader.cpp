#include "pipeline/wmo_loader.hpp"
#include "core/logger.hpp"
#include <cstring>
#include <glm/gtc/quaternion.hpp>

namespace wowee {
namespace pipeline {

namespace {

// WMO chunk identifiers
constexpr uint32_t MVER = 0x4D564552;  // Version
constexpr uint32_t MOHD = 0x4D4F4844;  // Header
constexpr uint32_t MOTX = 0x4D4F5458;  // Textures
constexpr uint32_t MOMT = 0x4D4F4D54;  // Materials
constexpr uint32_t MOGN = 0x4D4F474E;  // Group names
constexpr uint32_t MOGI = 0x4D4F4749;  // Group info
constexpr uint32_t MOLT = 0x4D4F4C54;  // Lights
constexpr uint32_t MODN = 0x4D4F444E;  // Doodad names
constexpr uint32_t MODD = 0x4D4F4444;  // Doodad definitions
constexpr uint32_t MODS = 0x4D4F4453;  // Doodad sets
constexpr uint32_t MOPV = 0x4D4F5056;  // Portal vertices
constexpr uint32_t MOPT = 0x4D4F5054;  // Portal info
constexpr uint32_t MOPR = 0x4D4F5052;  // Portal references
constexpr uint32_t MFOG = 0x4D464F47;  // Fog records
constexpr uint32_t MOSB = 0x4D4F5342;  // WMO-local skybox model path

// WMO group chunk identifiers
constexpr uint32_t MOGP = 0x4D4F4750;  // Group header
constexpr uint32_t MOVI = 0x4D4F5649;  // Indices
constexpr uint32_t MOBA = 0x4D4F4241;  // Batches
constexpr uint32_t MOCV = 0x4D4F4356;  // Vertex colors
constexpr uint32_t MONR = 0x4D4F4E52;  // Normals
constexpr uint32_t MOTV = 0x4D4F5456;  // Texture coords
constexpr uint32_t MLIQ = 0x4D4C4951;  // Liquid
constexpr uint32_t MOLR = 0x4D4F4C52;  // Light references (u16 MOLT indices)
constexpr uint32_t MODR = 0x4D4F4452;  // Doodad references (u16 MODD indices)
constexpr uint32_t MOBN = 0x4D4F424E;  // Collision BSP nodes
constexpr uint32_t MOBR = 0x4D4F4252;  // Collision BSP face refs (u16 triangle indices)

// Read utilities
template<typename T>
T read(const std::vector<uint8_t>& data, uint32_t& offset) {
    if (offset + sizeof(T) > data.size()) {
        return T{};
    }
    T value;
    std::memcpy(&value, &data[offset], sizeof(T));
    offset += sizeof(T);
    return value;
}

template<typename T>
std::vector<T> readArray(const std::vector<uint8_t>& data, uint32_t offset, uint32_t count) {
    std::vector<T> result;
    // Use 64-bit arithmetic to prevent uint32 overflow on crafted count values.
    // A large count (e.g., 0x20000001 with sizeof(T)=8) would wrap to a small
    // value in 32-bit, pass the bounds check, then cause a multi-GB allocation.
    uint64_t totalBytes = static_cast<uint64_t>(count) * sizeof(T);
    constexpr uint64_t kMaxReadBytes = 64u * 1024u * 1024u;  // 64MB sanity cap
    if (totalBytes > kMaxReadBytes || static_cast<uint64_t>(offset) + totalBytes > data.size()) {
        return result;
    }
    result.resize(count);
    std::memcpy(result.data(), &data[offset], static_cast<size_t>(totalBytes));
    return result;
}

std::string readString(const std::vector<uint8_t>& data, uint32_t offset) {
    std::string result;
    while (offset < data.size() && data[offset] != 0) {
        result += static_cast<char>(data[offset++]);
    }
    return result;
}

} // anonymous namespace

WMOModel WMOLoader::load(const std::vector<uint8_t>& wmoData) {
    WMOModel model;

    if (wmoData.size() < 8) {
        core::Logger::getInstance().error("WMO data too small");
        return model;
    }

    // WMO loader logs disabled

    uint32_t offset = 0;

    // Parse chunks
    while (offset + 8 <= wmoData.size()) {
        uint32_t chunkId = read<uint32_t>(wmoData, offset);
        uint32_t chunkSize = read<uint32_t>(wmoData, offset);

        // 64-bit, for the reason readArray gives above: offset and chunkSize
        // are both uint32 and both come from the file, so the sum wraps and a
        // crafted chunkSize walks the loop past the end of the buffer.
        // Retail 1.12 tolerates a final WMO chunk whose declared size runs
        // slightly past EOF; clamp it instead of rejecting the whole remainder.
        const uint64_t declaredEnd = static_cast<uint64_t>(offset) + chunkSize;
        uint32_t chunkStart = offset;
        uint32_t chunkEnd = static_cast<uint32_t>(
            std::min<uint64_t>(declaredEnd, wmoData.size()));
        if (declaredEnd > wmoData.size()) {
            core::Logger::getInstance().warning(
                "WMO chunk extends beyond EOF; clamping declared end ", declaredEnd,
                " to ", wmoData.size());
        }

        switch (chunkId) {
            case MVER: {
                model.version = read<uint32_t>(wmoData, offset);
                // WMO version log disabled
                break;
            }

            case MOHD: {
                // Header - SMOHeader structure (WotLK 3.3.5a)
                model.nTextures = read<uint32_t>(wmoData, offset);   // Was missing!
                model.nGroups = read<uint32_t>(wmoData, offset);
                model.nPortals = read<uint32_t>(wmoData, offset);
                model.nLights = read<uint32_t>(wmoData, offset);
                model.nDoodadNames = read<uint32_t>(wmoData, offset);
                model.nDoodadDefs = read<uint32_t>(wmoData, offset);
                model.nDoodadSets = read<uint32_t>(wmoData, offset);

                uint32_t ambColor = read<uint32_t>(wmoData, offset);  // Ambient color (BGRA)
                // Unpack BGRA bytes to normalized [0,1] RGB
                model.ambientColor.r = ((ambColor >> 16) & 0xFF) / 255.0f;
                model.ambientColor.g = ((ambColor >>  8) & 0xFF) / 255.0f;
                model.ambientColor.b = ((ambColor >>  0) & 0xFF) / 255.0f;
                model.rootId = read<uint32_t>(wmoData, offset); // +0x20

                model.boundingBoxMin.x = read<float>(wmoData, offset);
                model.boundingBoxMin.y = read<float>(wmoData, offset);
                model.boundingBoxMin.z = read<float>(wmoData, offset);

                model.boundingBoxMax.x = read<float>(wmoData, offset);
                model.boundingBoxMax.y = read<float>(wmoData, offset);
                model.boundingBoxMax.z = read<float>(wmoData, offset);

                // Vanilla v17 stores a 32-bit root flags dword at +0x3c.
                // Later clients split/reinterpreted this tail; do not apply that
                // later layout to 1.12 data.
                model.headerFlags = read<uint32_t>(wmoData, offset);

                core::Logger::getInstance().debug(
                    "WMO header: nTextures=", model.nTextures,
                    " nGroups=", model.nGroups,
                    " wmoID=", model.rootId,
                    " flags=0x", std::hex, model.headerFlags, std::dec);
                break;
            }

            case MOTX: {
                // Textures - raw block of null-terminated strings
                // Material texture1/texture2/texture3 are byte offsets into this chunk.
                // We must map every offset to its texture index.
                uint32_t texOffset = chunkStart;
                uint32_t texIndex = 0;
                core::Logger::getInstance().debug("MOTX chunk: ", chunkSize, " bytes");
                while (texOffset < chunkEnd) {
                    uint32_t relativeOffset = texOffset - chunkStart;

                    std::string texName = readString(wmoData, texOffset);
                    if (texName.empty()) {
                        // Skip null bytes (empty entries or padding)
                        texOffset++;
                        continue;
                    }

                    // Store mapping from byte offset to texture index
                    model.textureOffsetToIndex[relativeOffset] = texIndex;
                    model.textures.push_back(texName);
                    // MOTX texture log disabled
                    texOffset += texName.length() + 1;
                    texIndex++;
                }
                // WMO textures log disabled
                break;
            }

            case MOMT: {
                // Vanilla 1.12 MOMT is exactly 64 bytes. There are only TWO
                // authored texture offsets: +0x0c and +0x18. +0x38/+0x3c are
                // runtime handles which retail overwrites after resolving MOTX.
                const uint32_t nMaterials = (chunkEnd - chunkStart) / 64;
                for (uint32_t i = 0; i < nMaterials; ++i) {
                    const uint32_t rec = chunkStart + i * 64;
                    uint32_t p = rec;
                    WMOMaterial mat;
                    mat.flags = read<uint32_t>(wmoData, p);          // +00
                    mat.shader = read<uint32_t>(wmoData, p);         // +04
                    mat.blendMode = read<uint32_t>(wmoData, p);      // +08
                    mat.texture1 = read<uint32_t>(wmoData, p);       // +0c
                    mat.sidnColor = read<uint32_t>(wmoData, p);      // +10
                    mat.frameSidnColor = read<uint32_t>(wmoData, p); // +14
                    mat.texture2 = read<uint32_t>(wmoData, p);       // +18
                    mat.diffColor = read<uint32_t>(wmoData, p);      // +1c
                    mat.groundType = read<uint32_t>(wmoData, p);     // +20
                    mat.color2 = read<uint32_t>(wmoData, p);         // +24
                    mat.raw28 = read<uint32_t>(wmoData, p);
                    mat.raw2C = read<uint32_t>(wmoData, p);
                    mat.raw30 = read<uint32_t>(wmoData, p);
                    mat.raw34 = read<uint32_t>(wmoData, p);
                    mat.runtimeTexture1 = read<uint32_t>(wmoData, p);
                    mat.runtimeTexture2 = read<uint32_t>(wmoData, p);
                    model.materials.push_back(mat);
                }
                core::Logger::getInstance().debug("WMO materials: ", model.materials.size());
                break;
            }

            case MOGN: {
                // Group names - store raw chunk for offset-based lookup (MOGI nameOffset)
                if (chunkSize > 0 && chunkEnd <= wmoData.size()) {
                    model.groupNameRaw.assign(wmoData.begin() + chunkStart, wmoData.begin() + chunkEnd);
                }
                uint32_t nameOffset = chunkStart;
                while (nameOffset < chunkEnd) {
                    std::string name = readString(wmoData, nameOffset);
                    if (name.empty()) break;
                    model.groupNames.push_back(name);
                    nameOffset += name.length() + 1;
                }
                // WMO group names log disabled
                break;
            }

            case MOGI: {
                // Group info
                uint32_t nGroupInfo = chunkSize / 32;  // Each group info is 32 bytes
                for (uint32_t i = 0; i < nGroupInfo; i++) {
                    WMOGroupInfo info;
                    info.flags = read<uint32_t>(wmoData, offset);
                    info.boundingBoxMin.x = read<float>(wmoData, offset);
                    info.boundingBoxMin.y = read<float>(wmoData, offset);
                    info.boundingBoxMin.z = read<float>(wmoData, offset);
                    info.boundingBoxMax.x = read<float>(wmoData, offset);
                    info.boundingBoxMax.y = read<float>(wmoData, offset);
                    info.boundingBoxMax.z = read<float>(wmoData, offset);
                    info.nameOffset = read<int32_t>(wmoData, offset);

                    model.groupInfo.push_back(info);
                }
                core::Logger::getInstance().debug("WMO group info: ", model.groupInfo.size());
                break;
            }

            case MOLT: {
                // Exact Vanilla MOLT record, stride 0x30 (48 bytes).
                const uint32_t nLights = (chunkEnd - chunkStart) / 48;
                for (uint32_t i = 0; i < nLights; ++i) {
                    uint32_t p = chunkStart + i * 48;
                    WMOLight light;
                    light.lightType = read<uint8_t>(wmoData, p);
                    light.type = read<uint8_t>(wmoData, p);
                    light.useAttenuation = read<uint8_t>(wmoData, p);
                    light.pad = read<uint8_t>(wmoData, p);
                    light.packedColor = read<uint32_t>(wmoData, p);
                    light.color.r = ((light.packedColor >> 16) & 0xFF) / 255.0f;
                    light.color.g = ((light.packedColor >> 8) & 0xFF) / 255.0f;
                    light.color.b = (light.packedColor & 0xFF) / 255.0f;
                    light.color.a = ((light.packedColor >> 24) & 0xFF) / 255.0f;
                    light.position.x = read<float>(wmoData, p);
                    light.position.y = read<float>(wmoData, p);
                    light.position.z = read<float>(wmoData, p);
                    light.intensity = read<float>(wmoData, p);
                    light.attenuationStart = read<float>(wmoData, p);
                    light.attenuationEnd = read<float>(wmoData, p);
                    for (float& value : light.unknown) value = read<float>(wmoData, p);
                    model.lights.push_back(light);
                }
                core::Logger::getInstance().debug("WMO lights: ", model.lights.size());
                break;
            }

            case MFOG: {
                // Exact Vanilla MFOG record, stride 0x30.
                const uint32_t nFogs = (chunkEnd - chunkStart) / 48;
                for (uint32_t i = 0; i < nFogs; ++i) {
                    uint32_t p = chunkStart + i * 48;
                    WMOFog fog;
                    fog.flags = read<uint32_t>(wmoData, p);
                    fog.position.x = read<float>(wmoData, p);
                    fog.position.y = read<float>(wmoData, p);
                    fog.position.z = read<float>(wmoData, p);
                    fog.smallRadius = read<float>(wmoData, p);
                    fog.largeRadius = read<float>(wmoData, p);
                    fog.endDist = read<float>(wmoData, p);
                    fog.startFactor = read<float>(wmoData, p);
                    fog.packedColor = read<uint32_t>(wmoData, p);
                    fog.underwaterEndDist = read<float>(wmoData, p);
                    fog.underwaterStartFactor = read<float>(wmoData, p);
                    fog.packedUnderwaterColor = read<uint32_t>(wmoData, p);
                    model.fogs.push_back(fog);
                }
                break;
            }

            case MOSB: {
                // Optional WMO-local skybox model path. Most roots store an
                // empty string; preserve it for the handful that author one.
                model.skyboxPath = readString(wmoData, chunkStart);
                break;
            }

            case MODN: {
                // Doodad names - stored by byte offset into the MODN chunk
                // (MODD nameIndex is a byte offset, not a vector index)
                uint32_t nameOffset = 0;  // Offset relative to chunk start
                while (chunkStart + nameOffset < chunkEnd) {
                    std::string name = readString(wmoData, chunkStart + nameOffset);
                    if (!name.empty()) {
                        model.doodadNames[nameOffset] = name;
                    }
                    nameOffset += name.length() + 1;
                }
                core::Logger::getInstance().debug("Loaded ", model.doodadNames.size(), " doodad names");
                break;
            }

            case MODD: {
                // Doodad definitions
                uint32_t nDoodads = chunkSize / 40;  // Each doodad is 40 bytes
                for (uint32_t i = 0; i < nDoodads; i++) {
                    WMODoodad doodad;

                    // WMO doodad placement: name index packed in lower 24 bits, flags in upper 8.
                    // The name index is an offset into the MODN string table (doodad names).
                    constexpr uint32_t kDoodadNameIndexMask = 0x00FFFFFF;
                    uint32_t nameAndFlags = read<uint32_t>(wmoData, offset);
                    doodad.nameIndex = nameAndFlags & kDoodadNameIndexMask;

                    doodad.position.x = read<float>(wmoData, offset);
                    doodad.position.y = read<float>(wmoData, offset);
                    doodad.position.z = read<float>(wmoData, offset);

                    // C4Quaternion in file: x, y, z, w
                    doodad.rotation.x = read<float>(wmoData, offset);
                    doodad.rotation.y = read<float>(wmoData, offset);
                    doodad.rotation.z = read<float>(wmoData, offset);
                    doodad.rotation.w = read<float>(wmoData, offset);

                    doodad.scale = read<float>(wmoData, offset);

                    uint32_t color = read<uint32_t>(wmoData, offset);
                    doodad.color.b = ((color >> 0) & 0xFF) / 255.0f;
                    doodad.color.g = ((color >> 8) & 0xFF) / 255.0f;
                    doodad.color.r = ((color >> 16) & 0xFF) / 255.0f;
                    doodad.color.a = ((color >> 24) & 0xFF) / 255.0f;

                    model.doodads.push_back(doodad);
                }
                core::Logger::getInstance().debug("WMO doodads: ", model.doodads.size());
                break;
            }

            case MODS: {
                // Doodad sets: 20-byte name + 3×uint32 = 32 bytes each.
                // Use bounds check before memcpy to avoid OOB on truncated files
                // (the raw memcpy bypassed the safe read<T> template).
                uint32_t nSets = chunkSize / 32;
                for (uint32_t i = 0; i < nSets; i++) {
                    WMODoodadSet set;
                    if (offset + 20 > wmoData.size()) break;
                    std::memcpy(set.name, &wmoData[offset], 20);
                    offset += 20;
                    set.startIndex = read<uint32_t>(wmoData, offset);
                    set.count = read<uint32_t>(wmoData, offset);
                    set.padding = read<uint32_t>(wmoData, offset);

                    model.doodadSets.push_back(set);
                }
                core::Logger::getInstance().debug("WMO doodad sets: ", model.doodadSets.size());
                break;
            }

            case MOPV: {
                // Portal vertices
                uint32_t nVerts = chunkSize / 12;  // Each vertex is 3 floats
                for (uint32_t i = 0; i < nVerts; i++) {
                    glm::vec3 vert;
                    vert.x = read<float>(wmoData, offset);
                    vert.y = read<float>(wmoData, offset);
                    vert.z = read<float>(wmoData, offset);
                    model.portalVertices.push_back(vert);
                }
                break;
            }

            case MOPT: {
                // Exact Vanilla MOPT: u16 start/count + C4Plane.
                const uint32_t nPortals = (chunkEnd - chunkStart) / 20;
                for (uint32_t i = 0; i < nPortals; ++i) {
                    uint32_t p = chunkStart + i * 20;
                    WMOPortal portal;
                    portal.startVertex = read<uint16_t>(wmoData, p);
                    portal.vertexCount = read<uint16_t>(wmoData, p);
                    portal.normal.x = read<float>(wmoData, p);
                    portal.normal.y = read<float>(wmoData, p);
                    portal.normal.z = read<float>(wmoData, p);
                    portal.distance = read<float>(wmoData, p);
                    model.portals.push_back(portal);
                }
                core::Logger::getInstance().debug("WMO portals: ", model.portals.size());
                break;
            }

            case MOPR: {
                // Portal references - links groups via portals
                uint32_t nRefs = chunkSize / 8;  // Each reference is 8 bytes
                for (uint32_t i = 0; i < nRefs; i++) {
                    WMOPortalRef ref;
                    ref.portalIndex = read<uint16_t>(wmoData, offset);
                    ref.groupIndex = read<uint16_t>(wmoData, offset);
                    ref.side = read<int16_t>(wmoData, offset);
                    ref.padding = read<uint16_t>(wmoData, offset);
                    model.portalRefs.push_back(ref);
                }
                core::Logger::getInstance().debug("WMO portal refs: ", model.portalRefs.size());
                break;
            }

            default:
                // Unknown chunk, skip it
                break;
        }

        offset = chunkEnd;
    }

    // Initialize groups array. Cap at a sanity limit so a hostile or
    // corrupted WMO header can't trigger a multi-gigabyte allocation -
    // Blizzard's largest real WMOs cap out around a few hundred groups.
    constexpr uint32_t kMaxWMOGroups = 4096;
    if (model.nGroups > kMaxWMOGroups) {
        core::Logger::getInstance().warning(
            "WMO: nGroups=", model.nGroups,
            " exceeds sanity cap ", kMaxWMOGroups, ", clamping");
        model.nGroups = kMaxWMOGroups;
    }
    model.groups.resize(model.nGroups);

    // WMO loaded log disabled
    return model;
}

bool WMOLoader::loadGroup(const std::vector<uint8_t>& groupData,
                          WMOModel& model,
                          uint32_t groupIndex) {
    if (groupIndex >= model.groups.size()) {
        core::Logger::getInstance().error("Invalid group index: ", groupIndex);
        return false;
    }

    if (groupData.size() < 20) {
        core::Logger::getInstance().error("WMO group file too small");
        return false;
    }

    auto& group = model.groups[groupIndex];
    group.groupId = groupIndex;

    uint32_t offset = 0;

    // Parse chunks in group file
    while (offset + 8 <= groupData.size()) {
        uint32_t chunkId = read<uint32_t>(groupData, offset);
        uint32_t chunkSize = read<uint32_t>(groupData, offset);
        const uint64_t declaredEnd = static_cast<uint64_t>(offset) + chunkSize;
        uint32_t chunkEnd = static_cast<uint32_t>(
            std::min<uint64_t>(declaredEnd, groupData.size()));

        if (chunkId == MVER) {
            // Version - skip
        }
        else if (chunkId == MOGP) {
            // Group header - parse sub-chunks
            // MOGP header is 68 bytes, followed by sub-chunks
            if (chunkSize < 68) {
                offset = chunkEnd;
                continue;
            }

            // Read MOGP header
            // MOGP starts with groupName(4) + descriptiveName(4) offsets into MOGN,
            // followed by flags at offset +8.
            uint32_t mogpOffset = offset;
            mogpOffset += 4; // skip groupName offset
            mogpOffset += 4; // skip descriptiveGroupName offset
            group.flags = read<uint32_t>(groupData, mogpOffset);
            bool isInterior = wmoGroupIsInterior112(group.flags);
            core::Logger::getInstance().debug(
                "  Group flags: 0x", std::hex, group.flags, std::dec,
                (isInterior ? " (Vanilla INTERIOR)" : " (Vanilla exterior/exterior-lit)"));
            group.boundingBoxMin.x = read<float>(groupData, mogpOffset);
            group.boundingBoxMin.y = read<float>(groupData, mogpOffset);
            group.boundingBoxMin.z = read<float>(groupData, mogpOffset);
            group.boundingBoxMax.x = read<float>(groupData, mogpOffset);
            group.boundingBoxMax.y = read<float>(groupData, mogpOffset);
            group.boundingBoxMax.z = read<float>(groupData, mogpOffset);
            group.portalStart = read<uint16_t>(groupData, mogpOffset);
            group.portalCount = read<uint16_t>(groupData, mogpOffset);
            mogpOffset += 8; // transBatchCount, intBatchCount, extBatchCount, padding
            // fogIndices: 4 × uint8 (4 bytes total, NOT 4 × uint32)
            group.fogIndices[0] = read<uint8_t>(groupData, mogpOffset);
            group.fogIndices[1] = read<uint8_t>(groupData, mogpOffset);
            group.fogIndices[2] = read<uint8_t>(groupData, mogpOffset);
            group.fogIndices[3] = read<uint8_t>(groupData, mogpOffset);
            group.liquidType = read<uint32_t>(groupData, mogpOffset); // +0x34
            group.areaTableId = read<uint32_t>(groupData, mogpOffset); // +0x38
            // Remaining bytes to the exact 0x44-byte header are reserved.
            mogpOffset = offset + 68;

            // Parse sub-chunks within MOGP
            int groupLogCount = 0;
            while (mogpOffset + 8 < chunkEnd) {
                uint32_t subChunkId = read<uint32_t>(groupData, mogpOffset);
                uint32_t subChunkSize = read<uint32_t>(groupData, mogpOffset);
                // Widened for the same reason as the root chunk loop: both
                // operands are uint32 read from the file, and a wrapped
                // subChunkEnd is smaller than mogpOffset, which every
                // "parseOffset + n <= subChunkEnd" test below then reads as
                // "no room" or, worse, as room that is not there.
                const uint64_t subChunkEnd64 =
                    static_cast<uint64_t>(mogpOffset) + subChunkSize;
                if (subChunkEnd64 > chunkEnd) {
                    break;
                }
                // Past the guard it is <= chunkEnd, so this cannot truncate.
                const uint32_t subChunkEnd = static_cast<uint32_t>(subChunkEnd64);

                // Debug: log chunk magic as string
                char magic[5] = {0};
                magic[0] = (subChunkId >> 0) & 0xFF;
                magic[1] = (subChunkId >> 8) & 0xFF;
                magic[2] = (subChunkId >> 16) & 0xFF;
                magic[3] = (subChunkId >> 24) & 0xFF;
                // Not static - previously this throttle was per-process, silencing
                // all WMO group logging after the first 30 sub-chunks globally.
                if (groupLogCount < 30) {
                    core::Logger::getInstance().debug("  WMO sub-chunk: ", magic, " (0x", std::hex, subChunkId, std::dec, ") size=", subChunkSize);
                    groupLogCount++;
                }

                if (subChunkId == 0x4D4F5654) { // MOVT - Vertices
                    uint32_t vertexCount = subChunkSize / 12; // 3 floats per vertex
                    for (uint32_t i = 0; i < vertexCount; i++) {
                        WMOVertex vertex;
                        // Keep vertices in WoW model-local coords - coordinate swap done in model matrix
                        vertex.position.x = read<float>(groupData, mogpOffset);
                        vertex.position.y = read<float>(groupData, mogpOffset);
                        vertex.position.z = read<float>(groupData, mogpOffset);
                        vertex.normal = glm::vec3(0, 0, 1);
                        vertex.texCoord = glm::vec2(0, 0);
                        vertex.color = glm::vec4(1, 1, 1, 1);
                        group.vertices.push_back(vertex);
                    }
                }
                else if (subChunkId == MOVI) { // Indices
                    uint32_t indexCount = subChunkSize / 2; // uint16_t per index
                    for (uint32_t i = 0; i < indexCount; i++) {
                        group.indices.push_back(read<uint16_t>(groupData, mogpOffset));
                    }
                }
                else if (subChunkId == 0x4D4F5059) { // MOPY - Triangle material info
                    // 2 bytes per triangle: flags (uint8) + materialId (uint8)
                    // flag 0x04 = detail/decorative geometry (no collision)
                    uint32_t triCount = subChunkSize / 2;
                    group.triFlags.resize(triCount);
                    group.triMaterialIds.resize(triCount);
                    for (uint32_t i = 0; i < triCount; i++) {
                        group.triFlags[i] = read<uint8_t>(groupData, mogpOffset);
                        group.triMaterialIds[i] = read<uint8_t>(groupData, mogpOffset);
                    }
                }
                else if (subChunkId == MONR) { // Normals
                    uint32_t normalCount = subChunkSize / 12;
                    core::Logger::getInstance().debug("  MONR: ", normalCount, " normals for ", group.vertices.size(), " vertices");
                    for (uint32_t i = 0; i < normalCount && i < group.vertices.size(); i++) {
                        group.vertices[i].normal.x = read<float>(groupData, mogpOffset);
                        group.vertices[i].normal.y = read<float>(groupData, mogpOffset);
                        group.vertices[i].normal.z = read<float>(groupData, mogpOffset);
                    }
                    if (normalCount > 0 && !group.vertices.empty()) {
                        const auto& n = group.vertices[0].normal;
                        core::Logger::getInstance().debug("    First normal: (", n.x, ", ", n.y, ", ", n.z, ")");
                    }
                }
                else if (subChunkId == MOTV) { // Texture coords
                    // Update texture coords for existing vertices
                    uint32_t texCoordCount = subChunkSize / 8;
                    core::Logger::getInstance().debug("  MOTV: ", texCoordCount, " tex coords for ", group.vertices.size(), " vertices");
                    for (uint32_t i = 0; i < texCoordCount && i < group.vertices.size(); i++) {
                        group.vertices[i].texCoord.x = read<float>(groupData, mogpOffset);
                        group.vertices[i].texCoord.y = read<float>(groupData, mogpOffset);
                    }
                    if (texCoordCount > 0 && !group.vertices.empty()) {
                        core::Logger::getInstance().debug("    First UV: (", group.vertices[0].texCoord.x, ", ", group.vertices[0].texCoord.y, ")");
                    }
                }
                else if (subChunkId == MODR) {
                    // Exact group ownership of root MODD doodads.
                    const uint32_t count = subChunkSize / 2;
                    group.doodadRefs.reserve(group.doodadRefs.size() + count);
                    for (uint32_t i = 0; i < count; ++i) {
                        group.doodadRefs.push_back(read<uint16_t>(groupData, mogpOffset));
                    }
                }
                else if (subChunkId == MOLR) {
                    // Indices into root MOLT. These lights belong to this group.
                    const uint32_t count = subChunkSize / 2;
                    group.lightRefs.reserve(group.lightRefs.size() + count);
                    for (uint32_t i = 0; i < count; ++i) {
                        group.lightRefs.push_back(read<uint16_t>(groupData, mogpOffset));
                    }
                }
                else if (subChunkId == MOBN) {
                    // Vanilla collision BSP, exact 16-byte nodes.
                    const uint32_t count = subChunkSize / 16;
                    group.bspNodes.reserve(group.bspNodes.size() + count);
                    for (uint32_t i = 0; i < count; ++i) {
                        WMOBSPNode n;
                        n.planeType = read<int16_t>(groupData, mogpOffset);
                        n.children[0] = read<int16_t>(groupData, mogpOffset);
                        n.children[1] = read<int16_t>(groupData, mogpOffset);
                        n.faceCount = read<uint16_t>(groupData, mogpOffset);
                        n.firstFace = read<uint16_t>(groupData, mogpOffset);
                        n.unknown = read<int16_t>(groupData, mogpOffset);
                        n.distance = read<float>(groupData, mogpOffset);
                        group.bspNodes.push_back(n);
                    }
                }
                else if (subChunkId == MOBR) {
                    // Triangle-number references into MOVI/3, consumed by MOBN leaves.
                    const uint32_t count = subChunkSize / 2;
                    group.bspFaceRefs.reserve(group.bspFaceRefs.size() + count);
                    for (uint32_t i = 0; i < count; ++i) {
                        group.bspFaceRefs.push_back(read<uint16_t>(groupData, mogpOffset));
                    }
                }
                else if (subChunkId == MOCV) { // Vertex colors
                    // Update vertex colors
                    uint32_t colorCount = subChunkSize / 4;
                    core::Logger::getInstance().debug("  MOCV: ", colorCount, " vertex colors for ", group.vertices.size(), " vertices");
                    for (uint32_t i = 0; i < colorCount && i < group.vertices.size(); i++) {
                        uint8_t b = read<uint8_t>(groupData, mogpOffset);
                        uint8_t g = read<uint8_t>(groupData, mogpOffset);
                        uint8_t r = read<uint8_t>(groupData, mogpOffset);
                        uint8_t a = read<uint8_t>(groupData, mogpOffset);
                        group.vertices[i].color = glm::vec4(r/255.0f, g/255.0f, b/255.0f, a/255.0f);
                    }
                    if (colorCount > 0 && !group.vertices.empty()) {
                        const auto& c = group.vertices[0].color;
                        core::Logger::getInstance().debug("    First color: (", c.r, ", ", c.g, ", ", c.b, ", ", c.a, ")");
                    }
                }
                else if (subChunkId == MOBA) { // Batches
                    // SMOBatch structure (24 bytes):
                    // - 6 x int16 bounding box (12 bytes)
                    // - uint32 startIndex (4 bytes)
                    // - uint16 count (2 bytes)
                    // - uint16 minIndex (2 bytes)
                    // - uint16 maxIndex (2 bytes)
                    // - uint8 flags (1 byte)
                    // - uint8 material_id (1 byte)
                    uint32_t batchCount = subChunkSize / 24;
                    for (uint32_t i = 0; i < batchCount; i++) {
                        WMOBatch batch;
                        mogpOffset += 12; // Skip bounding box (6 x int16 = 12 bytes)
                        batch.startIndex = read<uint32_t>(groupData, mogpOffset);
                        batch.indexCount = read<uint16_t>(groupData, mogpOffset);
                        batch.startVertex = read<uint16_t>(groupData, mogpOffset);
                        batch.lastVertex = read<uint16_t>(groupData, mogpOffset);
                        batch.flags = read<uint8_t>(groupData, mogpOffset);
                        batch.materialId = read<uint8_t>(groupData, mogpOffset);
                        group.batches.push_back(batch);

                        // Non-static so each group gets its own throttle window.
                        if (static_cast<int>(i) < 15) {
                            core::Logger::getInstance().debug("  Batch[", i, "]: start=", batch.startIndex,
                                " count=", batch.indexCount, " verts=[", batch.startVertex, "-",
                                batch.lastVertex, "] mat=", static_cast<int>(batch.materialId), " flags=", static_cast<int>(batch.flags));
                        }
                    }
                }
                else if (subChunkId == MLIQ) { // MLIQ - WMO liquid data
                    // Basic WotLK layout:
                    // uint32 xVerts, yVerts, xTiles, yTiles
                    // float  baseX, baseY, baseZ
                    // uint16 materialId
                    // (optional pad/unknown bytes)
                    // followed by vertex/tile payload
                    uint32_t parseOffset = mogpOffset;
                    if (parseOffset + 30 <= subChunkEnd) {
                        group.liquid.xVerts = read<uint32_t>(groupData, parseOffset);
                        group.liquid.yVerts = read<uint32_t>(groupData, parseOffset);
                        group.liquid.xTiles = read<uint32_t>(groupData, parseOffset);
                        group.liquid.yTiles = read<uint32_t>(groupData, parseOffset);
                        group.liquid.basePosition.x = read<float>(groupData, parseOffset);
                        group.liquid.basePosition.y = read<float>(groupData, parseOffset);
                        group.liquid.basePosition.z = read<float>(groupData, parseOffset);
                        group.liquid.materialId = read<uint16_t>(groupData, parseOffset);

                        // Keep parser resilient across minor format variants:
                        // prefer explicit per-vertex floats, otherwise fall back to flat.
                        const size_t vertexCount =
                            static_cast<size_t>(group.liquid.xVerts) * static_cast<size_t>(group.liquid.yVerts);
                        const size_t tileCount =
                            static_cast<size_t>(group.liquid.xTiles) * static_cast<size_t>(group.liquid.yTiles);
                        const size_t bytesRemaining = (subChunkEnd > parseOffset) ? (subChunkEnd - parseOffset) : 0;

                        group.liquid.heights.clear();
                        group.liquid.opacity.clear();
                        group.liquid.flags.clear();

                        // Vanilla MLIQ vertex record is exactly 8 bytes. Byte 0
                        // is the authored opacity/depth channel; bytes 4..7 are
                        // the float height. Do not reinterpret a truncated record
                        // as a packed float array: that invents geometry.
                        constexpr size_t VERTEX_STRIDE = 8;
                        if (vertexCount > 0 &&
                            bytesRemaining >= vertexCount * VERTEX_STRIDE) {
                            group.liquid.heights.resize(vertexCount);
                            group.liquid.opacity.resize(vertexCount);
                            for (size_t i = 0; i < vertexCount; ++i) {
                                group.liquid.opacity[i] = groupData[parseOffset];
                                parseOffset += 4; // opacity + three authored aux bytes
                                group.liquid.heights[i] = read<float>(groupData, parseOffset);
                            }
                        }

                        if (tileCount > 0 && parseOffset + tileCount <= subChunkEnd) {
                            group.liquid.flags.resize(tileCount);
                            std::memcpy(group.liquid.flags.data(), &groupData[parseOffset], tileCount);
                        } else if (tileCount > 0) {
                            group.liquid.flags.resize(tileCount, 0);
                        }

                        if (group.liquid.materialId == 0) {
                            group.liquid.materialId = static_cast<uint16_t>(group.liquidType);
                        }
                    }
                }

                mogpOffset = subChunkEnd;
            }
        }

        offset = chunkEnd;
    }

    // Create a default batch if none were loaded
    if (group.batches.empty() && !group.indices.empty()) {
        WMOBatch batch;
        batch.startIndex = 0;
        batch.indexCount = static_cast<uint16_t>(group.indices.size());
        batch.materialId = 0;
        group.batches.push_back(batch);
    }

    core::Logger::getInstance().debug("WMO group ", groupIndex, " loaded: ",
                                      group.vertices.size(), " vertices, ",
                                      group.indices.size(), " indices, ",
                                      group.batches.size(), " batches");
    return !group.vertices.empty() && !group.indices.empty();
}

} // namespace pipeline
} // namespace wowee
