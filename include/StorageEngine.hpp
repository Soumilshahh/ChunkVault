#pragma once
#include <string>
#include <vector>

// Raw block store: hash in, bytes out. No knowledge of files, trees, or
// ref counts — the hash is the address (content-addressable storage).
class StorageEngine {
private:
    const std::string CHUNK_DIR = ".chunkvault_repo/chunks/";

    void ensure_chunk_dir_exists() const;

public:
    bool chunk_exists(const std::string& chunk_hash) const;
    bool save_block(const std::string& chunk_hash, const std::vector<char>& data) const;
    // Throws if the chunk is missing (metadata/storage corruption).
    std::vector<char> read_block(const std::string& chunk_hash) const;
    void destroy_block(const std::string& chunk_hash) const;
};

