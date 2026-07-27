#include "MetadataManager.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <functional>
#include <mutex>

namespace fs = std::filesystem;

void MetadataManager::ensure_metadata_dir_exists() const {
    fs::path dir(METADATA_PATH);
    if (!fs::exists(dir)) {
        fs::create_directories(dir);
    }
}

// Explicit index-based split (not getline-in-a-loop) so a trailing empty
// field isn't silently dropped, e.g. "a|b|" -> {"a","b",""}.
std::vector<std::string> MetadataManager::split_line(const std::string& line, char delim) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        size_t pos = line.find(delim, start);
        if (pos == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return fields;
}

bool MetadataManager::file_exists(const std::string& cfs_name) {
    std::shared_lock lock(state_mutex);
    return file_table.find(cfs_name) != file_table.end();
}

void MetadataManager::register_file(const std::string& cfs_name, const std::string& root_hash, uint64_t size) {
    std::unique_lock lock(state_mutex);
    file_table[cfs_name] = FileNode{cfs_name, root_hash, size};
}

FileNode MetadataManager::get_file(const std::string& cfs_name) {
    std::shared_lock lock(state_mutex);
    auto it = file_table.find(cfs_name);
    if (it == file_table.end()) {
        throw std::runtime_error("MetadataManager: no such file registered: " + cfs_name);
    }
    return it->second;
}

void MetadataManager::add_tree_node(const std::string& hash, const MerkleNode& node) {
    std::unique_lock lock(state_mutex);
    tree_registry[hash] = node;
}

MerkleNode MetadataManager::get_tree_node(const std::string& hash) {
    std::shared_lock lock(state_mutex);
    auto it = tree_registry.find(hash);
    if (it == tree_registry.end()) {
        throw std::runtime_error("MetadataManager: no such Merkle node: " + hash);
    }
    return it->second;
}

void MetadataManager::merge_tree_nodes(const std::unordered_map<std::string, MerkleNode>& nodes) {
    std::unique_lock lock(state_mutex);
    for (const auto& [hash, node] : nodes) {
        tree_registry[hash] = node;
    }
}

const std::unordered_map<std::string, MerkleNode>& MetadataManager::get_full_registry() const {
    // No lock: safe only because the CLI is single-threaded per process.
    return tree_registry;
}

void MetadataManager::increment_chunk(const std::string& chunk_hash) {
    std::unique_lock lock(state_mutex);
    auto& meta = chunk_ledger[chunk_hash];
    meta.in_degree += 1;
    meta.is_orphaned = false;
}

void MetadataManager::decrement_chunk(const std::string& chunk_hash, std::vector<std::string>& orphan_queue) {
    std::unique_lock lock(state_mutex);
    auto it = chunk_ledger.find(chunk_hash);
    if (it == chunk_ledger.end()) {
        return;
    }
    if (it->second.in_degree > 0) {
        it->second.in_degree -= 1;
    }
    if (it->second.in_degree == 0) {
        it->second.is_orphaned = true;
        orphan_queue.push_back(chunk_hash);
    }
}

void MetadataManager::clear_file_mapping(const std::string& cfs_name, std::vector<std::string>& orphan_queue) {
    std::unique_lock lock(state_mutex);

    auto file_it = file_table.find(cfs_name);
    if (file_it == file_table.end()) {
        throw std::runtime_error("MetadataManager: no such file registered: " + cfs_name);
    }
    std::string root_hash = file_it->second.root_hash;

    // Mirrors MerkleTreeEngine::extract_leaf_sequence's traversal rule
    // (duplicated here to keep MetadataManager independent of
    // MerkleTreeEngine).
    std::vector<std::string> leaf_hashes;
    if (!root_hash.empty()) {
        std::function<void(const std::string&)> visit = [&](const std::string& hash) {
            auto node_it = tree_registry.find(hash);
            if (node_it == tree_registry.end()) {
                return;
            }
            const MerkleNode& node = node_it->second;
            if (node.is_leaf) {
                leaf_hashes.push_back(node.node_hash);
                return;
            }
            visit(node.left_child);
            if (!node.right_child.empty()) {
                visit(node.right_child);
            }
        };
        visit(root_hash);
    }

    for (const auto& chunk_hash : leaf_hashes) {
        auto ledger_it = chunk_ledger.find(chunk_hash);
        if (ledger_it == chunk_ledger.end()) {
            continue;
        }
        if (ledger_it->second.in_degree > 0) {
            ledger_it->second.in_degree -= 1;
        }
        if (ledger_it->second.in_degree == 0) {
            ledger_it->second.is_orphaned = true;
            orphan_queue.push_back(chunk_hash);
        }
    }

    file_table.erase(file_it);
}

// Flat format, one record per line, '|'-delimited:
//   files.tbl  -> cfs_name|root_hash|total_size
//   tree.tbl   -> node_hash|left_child|right_child|is_leaf(0/1)
//   chunks.tbl -> chunk_hash|in_degree|is_orphaned(0/1)
void MetadataManager::serialize_to_disk() {
    std::shared_lock lock(state_mutex);
    ensure_metadata_dir_exists();

    {
        std::ofstream out(METADATA_PATH + "files.tbl", std::ios::trunc);
        for (const auto& [name, node] : file_table) {
            out << node.cfs_name << '|' << node.root_hash << '|' << node.total_size << '\n';
        }
    }
    {
        std::ofstream out(METADATA_PATH + "tree.tbl", std::ios::trunc);
        for (const auto& [hash, node] : tree_registry) {
            out << node.node_hash << '|' << node.left_child << '|'
                << node.right_child << '|' << (node.is_leaf ? 1 : 0) << '\n';
        }
    }
    {
        std::ofstream out(METADATA_PATH + "chunks.tbl", std::ios::trunc);
        for (const auto& [hash, meta] : chunk_ledger) {
            out << hash << '|' << meta.in_degree << '|' << (meta.is_orphaned ? 1 : 0) << '\n';
        }
    }
}

void MetadataManager::deserialize_from_disk() {
    std::unique_lock lock(state_mutex);

    file_table.clear();
    tree_registry.clear();
    chunk_ledger.clear();

    {
        std::ifstream in(METADATA_PATH + "files.tbl");
        if (in) {
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                auto fields = split_line(line, '|');
                if (fields.size() != 3) continue;
                FileNode node;
                node.cfs_name = fields[0];
                node.root_hash = fields[1];
                node.total_size = std::stoull(fields[2]);
                file_table[node.cfs_name] = node;
            }
        }
    }
    {
        std::ifstream in(METADATA_PATH + "tree.tbl");
        if (in) {
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                auto fields = split_line(line, '|');
                if (fields.size() != 4) continue;
                MerkleNode node;
                node.node_hash = fields[0];
                node.left_child = fields[1];
                node.right_child = fields[2];
                node.is_leaf = (fields[3] == "1");
                tree_registry[node.node_hash] = node;
            }
        }
    }
    {
        std::ifstream in(METADATA_PATH + "chunks.tbl");
        if (in) {
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                auto fields = split_line(line, '|');
                if (fields.size() != 3) continue;
                ChunkMeta meta;
                meta.in_degree = static_cast<uint32_t>(std::stoul(fields[1]));
                meta.is_orphaned = (fields[2] == "1");
                chunk_ledger[fields[0]] = meta;
            }
        }
    }
}
