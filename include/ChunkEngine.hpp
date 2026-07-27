#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "Types.hpp"

class ChunkEngine {
private:
    static constexpr uint32_t MIN_CHUNK_SIZE = 2048;
    static constexpr uint32_t MAX_CHUNK_SIZE = 8192;
    static constexpr uint32_t WINDOW_SIZE = 48;
    static constexpr uint32_t PRIME = 1013;
    // ~1-in-4096 positions qualify as a cut point.
    static constexpr uint32_t CUT_MASK = 0x0FFF;

public:
    // Splits source_path into content-defined chunks (rolling hash cut
    // points), each with its SHA-256 already computed.
    std::vector<RawChunk> split_file_cdc(const std::string& source_path);

    std::string compute_sha256(const std::vector<char>& data);
    std::string compute_sha256(const std::string& data);
};
