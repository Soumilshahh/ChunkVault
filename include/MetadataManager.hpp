#pragma once
#include "Types.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <shared_mutex>

// In-RAM state for the repo (file names, Merkle nodes, chunk ref counts),
// persisted as flat delimited files under .chunkvault_repo/metadata/ between
// process invocations. No external DB dependency.
class MetadataManager {
private:
    std::unordered_map<std::string, FileNode> file_table;
    std::unordered_map<std::string, MerkleNode> tree_registry;
    std::unordered_map<std::string, ChunkMeta> chunk_ledger;

    const std::string METADATA_PATH = ".chunkvault_repo/metadata/";
    mutable std::shared_mutex state_mutex;

    void ensure_metadata_dir_exists() const;
    static std::vector<std::string> split_line(const std::string& line, char delim);

public:
    bool file_exists(const std::string& cfs_name);
    void register_file(const std::string& cfs_name, const std::string& root_hash, uint64_t size);
    FileNode get_file(const std::string& cfs_name);

    void add_tree_node(const std::string& hash, const MerkleNode& node);
    MerkleNode get_tree_node(const std::string& hash);
    void merge_tree_nodes(const std::unordered_map<std::string, MerkleNode>& nodes);
    const std::unordered_map<std::string, MerkleNode>& get_full_registry() const;

    void increment_chunk(const std::string& chunk_hash);
    // Decrements in_degree; hitting 0 marks the chunk orphaned and queues it.
    void decrement_chunk(const std::string& chunk_hash, std::vector<std::string>& orphan_queue);

    void serialize_to_disk();
    void deserialize_from_disk();

    // Unlinks cfs_name from its tree, decrementing every leaf chunk's
    // ref count and queuing any that hit 0. Used by `del` and by `mod`
    // (which re-registers the name right after).
    void clear_file_mapping(const std::string& cfs_name, std::vector<std::string>& orphan_queue);
};

