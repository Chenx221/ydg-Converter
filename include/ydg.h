#include <stdint.h>

#pragma pack(push, 1)

typedef struct {
    uint32_t offset;         // QOI data block offset
    uint32_t size;           // QOI data block size
    uint16_t x_offset;       // x_offset (guess)
    uint16_t tileHeight;     // tile height
    uint32_t unknown;        // unknown
} YDG_TileIndex;

typedef struct {
    // 0x10 - 0x20: unknown1
    uint32_t reserved1[4];

    // 0x20: Width, Height
    uint16_t width;
    uint16_t height;

    // 0x24: unknown2
    uint32_t reserved2[3];

    // 0x30: Part Number
    uint32_t tileCount;

    // Tile index array is stored immediately after this struct in file data.
} YDG_HeaderData;

static inline const YDG_TileIndex* YDG_GetTilesConst(const YDG_HeaderData* headerData) {
    return (const YDG_TileIndex*)((const uint8_t*)headerData + sizeof(YDG_HeaderData));
}

static inline YDG_TileIndex* YDG_GetTiles(YDG_HeaderData* headerData) {
    return (YDG_TileIndex*)((uint8_t*)headerData + sizeof(YDG_HeaderData));
}

typedef struct {
    // 0x00: Magic "YDG\0"
    char signature[4];

    // 0x04: "YU-RIS"
    char engine_id[8];

    // 0x0C: HeaderData Size
    uint32_t headerDataSize;

    YDG_HeaderData headerData;
} YDG_Header;

#pragma pack(pop)