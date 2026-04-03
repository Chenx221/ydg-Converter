#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "include/ydg.h"

#define QOI_IMPLEMENTATION
#include "include/qoi.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "include/stb_image_write.h"

#include "webp/decode.h"

namespace {

struct DecodedTile {
    uint16_t xOffset = 0;
    uint16_t logicalHeight = 0;
    int imgWidth = 0;
    int imgHeight = 0;
    std::vector<std::uint8_t> rgba;
};

static std::uint16_t ReadU16LE(const std::vector<std::uint8_t>& data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset]) |
        (static_cast<std::uint16_t>(data[offset + 1]) << 8);
}

static std::uint32_t ReadU32LE(const std::vector<std::uint8_t>& data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
        (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

static bool ReadAllBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return false;
    }

    const auto size = ifs.tellg();
    if (size <= 0) {
        return false;
    }

    out.resize(static_cast<std::size_t>(size));
    ifs.seekg(0, std::ios::beg);
    return static_cast<bool>(ifs.read(reinterpret_cast<char*>(out.data()), size));
}

static bool DecodeYdg(
    const std::filesystem::path& inputPath,
    std::vector<std::uint8_t>& canvas,
    int& canvasWidth,
    int& canvasHeight,
    std::string& errorMessage) {

    std::vector<std::uint8_t> fileBytes;
    if (!ReadAllBytes(inputPath, fileBytes)) {
        errorMessage = "Failed to read file";
        return false;
    }

    if (fileBytes.size() < 0x44) {
        errorMessage = "File too small, not a valid YDG";
        return false;
    }

    if (!(fileBytes[0] == 'Y' && fileBytes[1] == 'D' && fileBytes[2] == 'G' && fileBytes[3] == '\0')) {
        errorMessage = "Signature is not YDG\\0";
        return false;
    }

    const std::uint32_t headerDataSize = ReadU32LE(fileBytes, 0x0C);
    const std::size_t headerStart = 0x10;
    const std::size_t headerEnd = headerStart + static_cast<std::size_t>(headerDataSize);
    if (headerEnd > fileBytes.size()) {
        errorMessage = "headerDataSize out of range";
        return false;
    }

    const std::uint16_t width = ReadU16LE(fileBytes, 0x20);
    const std::uint16_t height = ReadU16LE(fileBytes, 0x22);
    const std::uint32_t tileCount = ReadU32LE(fileBytes, 0x30);

    if (width == 0 || height == 0) {
        errorMessage = "Invalid canvas size";
        return false;
    }

    if (tileCount == 0) {
        errorMessage = "tileCount is 0";
        return false;
    }

    const std::size_t tileBytes = static_cast<std::size_t>(tileCount) * sizeof(YDG_TileIndex);
    if (tileBytes > headerDataSize) {
        errorMessage = "Tile index area exceeds headerDataSize";
        return false;
    }

    const std::size_t tileTableOffset = headerEnd - tileBytes;
    if (tileTableOffset + tileBytes > fileBytes.size()) {
        errorMessage = "Tile index table out of range";
        return false;
    }

    std::vector<DecodedTile> decodedTiles;
    decodedTiles.reserve(tileCount);

    for (std::uint32_t i = 0; i < tileCount; ++i) {
        const std::size_t entryOffset = tileTableOffset + static_cast<std::size_t>(i) * sizeof(YDG_TileIndex);

        const std::uint32_t blockOffset = ReadU32LE(fileBytes, entryOffset + 0);
        const std::uint32_t blockSize   = ReadU32LE(fileBytes, entryOffset + 4);
        const std::uint16_t xOffset     = ReadU16LE(fileBytes, entryOffset + 8);
        const std::uint16_t tileHeight  = ReadU16LE(fileBytes, entryOffset + 10);

        if (blockSize == 0) {
            errorMessage = "Found empty image block";
            return false;
        }

        std::size_t dataOffset = static_cast<std::size_t>(blockOffset);
        const std::size_t dataSize = static_cast<std::size_t>(blockSize);

        if (dataOffset + dataSize > fileBytes.size()) {
            const std::size_t relativeOffset = headerEnd + dataOffset;
            if (relativeOffset + dataSize <= fileBytes.size()) {
                dataOffset = relativeOffset;
            }
            else {
                errorMessage = "Image block range out of bounds";
                return false;
            }
        }

        const std::uint8_t* blockData = fileBytes.data() + dataOffset;
        int imgW = 0, imgH = 0;
        std::vector<std::uint8_t> tileRgba;

        // Detect format: WebP = "RIFF....WEBP", otherwise treat as QOI.
        const bool isWebP = dataSize >= 12 &&
            blockData[0] == 'R' && blockData[1] == 'I' &&
            blockData[2] == 'F' && blockData[3] == 'F' &&
            blockData[8] == 'W' && blockData[9] == 'E' &&
            blockData[10] == 'B' && blockData[11] == 'P';

        if (isWebP) {
            uint8_t* pixels = WebPDecodeRGBA(blockData, dataSize, &imgW, &imgH);
            if (pixels == nullptr) {
                errorMessage = "Failed to decode WebP block";
                return false;
            }
            tileRgba.assign(pixels, pixels + static_cast<std::size_t>(imgW) * static_cast<std::size_t>(imgH) * 4);
            WebPFree(pixels);
        } else {
            qoi_desc desc{};
            void* pixels = qoi_decode(blockData, static_cast<int>(dataSize), &desc, 4);
            if (pixels == nullptr) {
                errorMessage = "Failed to decode QOI block";
                return false;
            }
            imgW = static_cast<int>(desc.width);
            imgH = static_cast<int>(desc.height);
            tileRgba.resize(static_cast<std::size_t>(imgW) * static_cast<std::size_t>(imgH) * 4);
            std::memcpy(tileRgba.data(), pixels, tileRgba.size());
            QOI_FREE(pixels);
        }

        DecodedTile tile;
        tile.xOffset = xOffset;
        tile.logicalHeight = tileHeight;
        tile.imgWidth = imgW;
        tile.imgHeight = imgH;
        tile.rgba = std::move(tileRgba);

        decodedTiles.emplace_back(std::move(tile));
    }

    canvasWidth = width;
    canvasHeight = height;
    canvas.assign(static_cast<std::size_t>(canvasWidth) * static_cast<std::size_t>(canvasHeight) * 4, 0);

    std::size_t cursorY = 0;
    for (const auto& tile : decodedTiles) {
        const int dstX = static_cast<int>(tile.xOffset);
        const int dstY = static_cast<int>(cursorY);
        const int srcW = tile.imgWidth;
        const int srcH = tile.imgHeight;

        if (dstY >= canvasHeight) {
            break;
        }

        const int logicalH = tile.logicalHeight > 0 ? static_cast<int>(tile.logicalHeight) : srcH;
        const int copyH = std::max(0, std::min({ srcH, logicalH, canvasHeight - dstY }));

        const int srcX0 = std::max(0, -dstX);
        const int dstX0 = std::max(0, dstX);
        const int copyW = std::max(0, std::min(srcW - srcX0, canvasWidth - dstX0));

        if (copyW > 0 && copyH > 0) {
            for (int row = 0; row < copyH; ++row) {
                const std::size_t srcIndex =
                    (static_cast<std::size_t>(row) * static_cast<std::size_t>(srcW) + static_cast<std::size_t>(srcX0)) * 4;
                const std::size_t dstIndex =
                    (static_cast<std::size_t>(dstY + row) * static_cast<std::size_t>(canvasWidth) + static_cast<std::size_t>(dstX0)) * 4;
                std::memcpy(canvas.data() + dstIndex, tile.rgba.data() + srcIndex, static_cast<std::size_t>(copyW) * 4);
            }
        }

        cursorY += static_cast<std::size_t>(logicalH);
    }

    return true;
}

static std::filesystem::path MakeOutputPath(const std::filesystem::path& input) {
    std::filesystem::path out = input;
    out.replace_extension(".png");
    return out;
}

static bool ConvertSingleFile(const std::filesystem::path& inputPath) {
    std::vector<std::uint8_t> canvas;
    int width = 0;
    int height = 0;
    std::string error;

    if (!DecodeYdg(inputPath, canvas, width, height, error)) {
        std::cerr << "[ERROR] " << inputPath.string() << " : " << error << "\n";
        return false;
    }

    const auto outputPath = MakeOutputPath(inputPath);
    const int writeOk = stbi_write_png(
        outputPath.string().c_str(),
        width,
        height,
        4,
        canvas.data(),
        width * 4);

    if (writeOk == 0) {
        std::cerr << "[ERROR] " << inputPath.string() << " : Failed to write PNG\n";
        return false;
    }

    std::cout << "[OK] " << inputPath.string() << " -> " << outputPath.string()
        << " (" << width << "x" << height << ")\n";
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage:\n"
            << "  ydg_converter <file1.ydg> [file2.ydg ...]\n";
        return 1;
    }

    int successCount = 0;
    int failCount = 0;

    for (int i = 1; i < argc; ++i) {
        const std::filesystem::path inputPath = argv[i];

        if (!std::filesystem::exists(inputPath)) {
            std::cerr << "[ERROR] " << inputPath.string() << " : File does not exist\n";
            ++failCount;
            continue;
        }

        if (!std::filesystem::is_regular_file(inputPath)) {
            std::cerr << "[ERROR] " << inputPath.string() << " : Not a regular file\n";
            ++failCount;
            continue;
        }

        if (ConvertSingleFile(inputPath)) {
            ++successCount;
        }
        else {
            ++failCount;
        }
    }

    std::cout << "Summary: success " << successCount << ", failed " << failCount << "\n";
    return failCount == 0 ? 0 : 2;
}
