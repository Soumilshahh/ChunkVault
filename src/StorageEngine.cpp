#include "StorageEngine.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

void StorageEngine::ensure_chunk_dir_exists() const {
    fs::path dir(CHUNK_DIR);
    if (!fs::exists(dir)) {
        fs::create_directories(dir);
    }
}

bool StorageEngine::chunk_exists(const std::string& chunk_hash) const {
    fs::path target = fs::path(CHUNK_DIR) / chunk_hash;
    return fs::exists(target);
}

bool StorageEngine::save_block(const std::string& chunk_hash, const std::vector<char>& data) const {
    ensure_chunk_dir_exists();
    fs::path target = fs::path(CHUNK_DIR) / chunk_hash;

    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    if (!data.empty()) {
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    return out.good();
}

std::vector<char> StorageEngine::read_block(const std::string& chunk_hash) const {
    fs::path target = fs::path(CHUNK_DIR) / chunk_hash;
    std::ifstream in(target, std::ios::binary);
    if (!in) {
        throw std::runtime_error(
            "StorageEngine: chunk missing from disk (possible corruption): " + chunk_hash);
    }
    return std::vector<char>(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>()
    );
}

void StorageEngine::destroy_block(const std::string& chunk_hash) const {
    fs::path target = fs::path(CHUNK_DIR) / chunk_hash;
    fs::remove(target);
}
