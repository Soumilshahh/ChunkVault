#pragma once
#include "Types.hpp"
#include "ChunkEngine.hpp"
#include <string>
#include <vector>
#include <unordered_map>

class MerkleTreeEngine {
private:
    ChunkEngine hasher;

    // parent_hash = SHA256(left + right). Order-sensitive.
    std::string combine_hashes(const std::string& left, const std::string& right);

public:
    // Builds a Merkle tree over `leaves`, writing every node into
    // global_registry, and returns the root hash ("" if leaves is empty).
    std::string build_tree(const std::vector<RawChunk>& leaves,
                            std::unordered_map<std::string, MerkleNode>& global_registry);

    // Walks down from root_hash, left-to-right, collecting leaf hashes in
    // original chunk order.
    std::vector<std::string> extract_leaf_sequence(
        const std::string& root_hash,
        const std::unordered_map<std::string, MerkleNode>& global_registry);
};
