#include "ChunkEngine.hpp"
#include <fstream>
#include <stdexcept>
#include <iomanip>
#include <sstream>
#include <openssl/evp.h>

std::string ChunkEngine::compute_sha256(const std::vector<char>& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error("ChunkEngine: failed to allocate EVP_MD_CTX");
    }

    bool ok =
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, data.data(), data.size()) == 1 &&
        EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1;

    EVP_MD_CTX_free(ctx);

    if (!ok) {
        throw std::runtime_error("ChunkEngine: SHA-256 digest computation failed");
    }

    std::ostringstream hex_stream;
    hex_stream << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digest_len; ++i) {
        hex_stream << std::setw(2) << static_cast<int>(digest[i]);
    }
    return hex_stream.str();
}

std::string ChunkEngine::compute_sha256(const std::string& data) {
    std::vector<char> buffer(data.begin(), data.end());
    return compute_sha256(buffer);
}

// Polynomial rolling hash (Rabin-Karp style) over a 48-byte trailing
// window. Cut point = (hash & CUT_MASK) == 0, bounded by
// [MIN_CHUNK_SIZE, MAX_CHUNK_SIZE]. Cutting on content (not offset) means
// an insertion only disturbs the chunk it lands in — everything else
// still cuts at the same points and re-dedups.
std::vector<RawChunk> ChunkEngine::split_file_cdc(const std::string& source_path) {
    std::ifstream file(source_path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("ChunkEngine: cannot open source file: " + source_path);
    }

    std::vector<char> buffer(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>()
    );

    std::vector<RawChunk> chunks;
    if (buffer.empty()) {
        return chunks;
    }

    uint32_t current_hash = 0;
    uint32_t start_idx = 0;

    // P^(WINDOW_SIZE - 1), used to evict the oldest byte on each slide.
    // uint32_t wraparound is intentional (defined behavior, mod 2^32).
    uint32_t power = 1;
    for (uint32_t i = 0; i < WINDOW_SIZE - 1; i++) {
        power = power * PRIME;
    }

    for (uint32_t i = 0; i < buffer.size(); i++) {
        unsigned char byte_val = static_cast<unsigned char>(buffer[i]);

        current_hash = current_hash * PRIME + byte_val;

        uint32_t bytes_in_chunk_so_far = i - start_idx + 1;
        if (bytes_in_chunk_so_far > WINDOW_SIZE) {
            unsigned char evicted = static_cast<unsigned char>(buffer[i - WINDOW_SIZE]);
            current_hash -= evicted * power;
        }

        bool window_is_full = bytes_in_chunk_so_far >= WINDOW_SIZE;
        bool min_size_met = bytes_in_chunk_so_far >= MIN_CHUNK_SIZE;
        bool max_size_hit = bytes_in_chunk_so_far >= MAX_CHUNK_SIZE;
        bool is_last_byte = (i == buffer.size() - 1);

        bool content_cut_point = window_is_full && min_size_met &&
                                  ((current_hash & CUT_MASK) == 0);

        if (content_cut_point || max_size_hit || is_last_byte) {
            std::vector<char> chunk_data(
                buffer.begin() + start_idx,
                buffer.begin() + i + 1
            );
            std::string hash = compute_sha256(chunk_data);
            chunks.push_back(RawChunk{hash, std::move(chunk_data)});

            start_idx = i + 1;
            current_hash = 0;
        }
    }

    return chunks;
}
