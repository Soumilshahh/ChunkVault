#include "MerkleTreeEngine.hpp"
#include <stdexcept>
#include <functional>

std::string MerkleTreeEngine::combine_hashes(const std::string& left, const std::string& right) {
    return hasher.compute_sha256(left + right);
}

std::string MerkleTreeEngine::build_tree(
    const std::vector<RawChunk>& leaves,
    std::unordered_map<std::string, MerkleNode>& global_registry) {

    if (leaves.empty()) {
        return "";
    }

    std::vector<std::string> current_layer;
    current_layer.reserve(leaves.size());
    for (const auto& leaf : leaves) {
        MerkleNode node{leaf.hash, "", "", true};
        global_registry[leaf.hash] = node;
        current_layer.push_back(leaf.hash);
    }

    if (current_layer.size() == 1) {
        return current_layer[0];
    }

    while (current_layer.size() > 1) {
        std::vector<std::string> next_layer;
        next_layer.reserve((current_layer.size() + 1) / 2);

        for (size_t i = 0; i < current_layer.size(); i += 2) {
            if (i + 1 < current_layer.size()) {
                const std::string& left = current_layer[i];
                const std::string& right = current_layer[i + 1];
                std::string parent_hash = combine_hashes(left, right);
                global_registry[parent_hash] = MerkleNode{parent_hash, left, right, false};
                next_layer.push_back(parent_hash);
            } else {
                // Odd one out: promote as a single-child node (right_child
                // empty) instead of self-pairing. Self-pairing makes
                // traversal visit the same leaf twice and corrupts output
                // (see extract_leaf_sequence).
                const std::string& lone = current_layer[i];
                std::string parent_hash = hasher.compute_sha256("SINGLE:" + lone);
                global_registry[parent_hash] = MerkleNode{parent_hash, lone, "", false};
                next_layer.push_back(parent_hash);
            }
        }
        current_layer = std::move(next_layer);
    }

    return current_layer[0];
}

std::vector<std::string> MerkleTreeEngine::extract_leaf_sequence(
    const std::string& root_hash,
    const std::unordered_map<std::string, MerkleNode>& global_registry) {

    std::vector<std::string> result;
    if (root_hash.empty()) {
        return result;
    }

    std::function<void(const std::string&)> visit = [&](const std::string& hash) {
        auto it = global_registry.find(hash);
        if (it == global_registry.end()) {
            throw std::runtime_error(
                "MerkleTreeEngine: dangling node hash during traversal: " + hash);
        }
        const MerkleNode& node = it->second;
        if (node.is_leaf) {
            result.push_back(node.node_hash);
            return;
        }
        visit(node.left_child);
        // Empty right_child = single-child promoted node; nothing to visit.
        if (!node.right_child.empty()) {
            visit(node.right_child);
        }
    };

    visit(root_hash);
    return result;
}
