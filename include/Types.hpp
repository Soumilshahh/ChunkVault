#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

// One node in a Merkle tree. Leaves point at a chunk hash; internal nodes
// point at two children (right_child empty => single-child node, see
// MerkleTreeEngine::build_tree).
struct MerkleNode {
    std::string node_hash;
    std::string left_child;
    std::string right_child;
    bool is_leaf;
};

// Maps a user-facing name to the Merkle root representing its content.
struct FileNode {
    std::string cfs_name;
    std::string root_hash;
    uint64_t total_size = 0;
};

// Reference count for one physical chunk on disk.
struct ChunkMeta {
    uint32_t in_degree = 0;
    bool is_orphaned = false;
};

// A freshly-cut chunk, in flight between ChunkEngine and StorageEngine/
// MerkleTreeEngine during add/mod. Not persisted.
struct RawChunk {
    std::string hash;
    std::vector<char> data;
};
