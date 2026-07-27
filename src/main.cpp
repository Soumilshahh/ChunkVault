#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

#include "ChunkEngine.hpp"
#include "MerkleTreeEngine.hpp"
#include "StorageEngine.hpp"
#include "MetadataManager.hpp"

namespace fs = std::filesystem;

static void print_usage() {
    std::cout <<
        "ChunkVault - Content-Addressable Storage File System (CDC + Merkle Tree)\n\n"
        "USAGE:\n"
        "  ChunkVault add  <source_file> <cfs_name>   Add a file under a given name\n"
        "  ChunkVault read <cfs_name> <output_file>   Reconstruct a file by name\n"
        "  ChunkVault del  <cfs_name>                 Remove a file's mapping\n"
        "  ChunkVault mod  <new_source_file> <cfs_name>  Replace an existing file's content\n";
}

// Shared by `ChunkVault add` and the second half of `ChunkVault mod`.
static void run_add(const std::string& source_path, const std::string& cfs_name,
                     ChunkEngine& chunker, MerkleTreeEngine& merkler,
                     StorageEngine& storage, MetadataManager& meta) {

    std::vector<RawChunk> chunks = chunker.split_file_cdc(source_path);

    uint64_t total_size = 0;
    for (const auto& c : chunks) {
        total_size += c.data.size();
    }

    int new_chunks = 0, dedup_chunks = 0;
    for (const auto& chunk : chunks) {
        if (!storage.chunk_exists(chunk.hash)) {
            if (!storage.save_block(chunk.hash, chunk.data)) {
                throw std::runtime_error("Failed to write chunk to disk: " + chunk.hash);
            }
            new_chunks++;
        } else {
            dedup_chunks++;
        }
    }

    std::unordered_map<std::string, MerkleNode> local_registry;
    std::string root_hash = merkler.build_tree(chunks, local_registry);
    meta.merge_tree_nodes(local_registry);

    for (const auto& chunk : chunks) {
        meta.increment_chunk(chunk.hash);
    }

    meta.register_file(cfs_name, root_hash, total_size);

    std::cout << "Added '" << cfs_name << "' ("
              << chunks.size() << " chunks: " << new_chunks << " new, "
              << dedup_chunks << " deduplicated, " << total_size << " bytes total)\n";
}

static int cmd_add(const std::string& source_path, const std::string& cfs_name) {
    ChunkEngine chunker;
    MerkleTreeEngine merkler;
    StorageEngine storage;
    MetadataManager meta;

    meta.deserialize_from_disk();

    if (meta.file_exists(cfs_name)) {
        std::cerr << "Error: '" << cfs_name << "' already exists. "
                     "Use 'ChunkVault mod' to replace its content.\n";
        return 1;
    }
    if (!fs::exists(source_path)) {
        std::cerr << "Error: source file not found: " << source_path << "\n";
        return 1;
    }

    try {
        run_add(source_path, cfs_name, chunker, merkler, storage, meta);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    meta.serialize_to_disk();
    return 0;
}

static int cmd_read(const std::string& cfs_name, const std::string& output_path) {
    MerkleTreeEngine merkler;
    StorageEngine storage;
    MetadataManager meta;

    meta.deserialize_from_disk();

    if (!meta.file_exists(cfs_name)) {
        std::cerr << "Error: no such file registered: " << cfs_name << "\n";
        return 1;
    }

    try {
        FileNode file = meta.get_file(cfs_name);

        std::vector<std::string> chunk_sequence =
            merkler.extract_leaf_sequence(file.root_hash, meta.get_full_registry());

        std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "Error: cannot open output path for writing: " << output_path << "\n";
            return 1;
        }

        uint64_t bytes_written = 0;
        for (const auto& chunk_hash : chunk_sequence) {
            std::vector<char> block = storage.read_block(chunk_hash);
            out.write(block.data(), static_cast<std::streamsize>(block.size()));
            bytes_written += block.size();
        }

        std::cout << "Reconstructed '" << cfs_name << "' -> " << output_path
                  << " (" << chunk_sequence.size() << " chunks, "
                  << bytes_written << " bytes)\n";

        if (bytes_written != file.total_size) {
            std::cerr << "Warning: reconstructed size (" << bytes_written
                      << ") does not match recorded size (" << file.total_size << ")\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

// `del` marks affected chunks orphaned immediately and persists that;
// actual deletion is deferred to `ChunkVault gc` (same pattern as `git gc`),
// since a short-lived CLI process can't run a background timer.
static int cmd_del(const std::string& cfs_name) {
    MetadataManager meta;
    meta.deserialize_from_disk();

    if (!meta.file_exists(cfs_name)) {
        std::cerr << "Error: no such file registered: " << cfs_name << "\n";
        return 1;
    }

    std::vector<std::string> orphan_queue;
    try {
        meta.clear_file_mapping(cfs_name, orphan_queue);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    meta.serialize_to_disk();

    std::cout << "File map dropped successfully. Scheduled cleanup pending "
                 "for orphaned elements.\n";
    if (!orphan_queue.empty()) {
        std::cout << "(" << orphan_queue.size() << " chunk(s) newly orphaned; "
                     "run 'ChunkVault gc' to reclaim disk space.)\n";
    }

    return 0;
}

static int cmd_mod(const std::string& new_source_path, const std::string& cfs_name) {
    ChunkEngine chunker;
    MerkleTreeEngine merkler;
    StorageEngine storage;
    MetadataManager meta;

    meta.deserialize_from_disk();

    if (!meta.file_exists(cfs_name)) {
        std::cerr << "Error: no such file registered: " << cfs_name
                  << " (use 'ChunkVault add' to create it first)\n";
        return 1;
    }
    if (!fs::exists(new_source_path)) {
        std::cerr << "Error: source file not found: " << new_source_path << "\n";
        return 1;
    }

    std::vector<std::string> old_orphans;
    try {
        // Decouple first: chunks shared with the new version dip to 0
        // in_degree here and rise back during run_add's increment step
        // below, so shared chunks survive and only real removals orphan.
        meta.clear_file_mapping(cfs_name, old_orphans);

        run_add(new_source_path, cfs_name, chunker, merkler, storage, meta);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    meta.serialize_to_disk();

    std::cout << "Modified '" << cfs_name << "' successfully.\n";
    return 0;
}

static int cmd_gc() {
    MetadataManager meta;
    StorageEngine storage;
    meta.deserialize_from_disk();

    // chunk_ledger is private and has no public "list orphans" accessor,
    // so we re-derive the orphan set by re-reading chunks.tbl directly.
    std::ifstream in(".chunkvault_repo/metadata/chunks.tbl");
    if (!in) {
        std::cout << "Nothing to clean up (no chunk ledger found).\n";
        return 0;
    }

    std::vector<std::string> to_delete;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        size_t p1 = line.find('|');
        size_t p2 = line.find('|', p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos) continue;
        std::string hash = line.substr(0, p1);
        std::string is_orphaned_flag = line.substr(p2 + 1);
        if (is_orphaned_flag == "1") {
            to_delete.push_back(hash);
        }
    }
    in.close();

    for (const auto& hash : to_delete) {
        storage.destroy_block(hash);
    }

    if (!to_delete.empty()) {
        std::ifstream in2(".chunkvault_repo/metadata/chunks.tbl");
        std::vector<std::string> keep_lines;
        std::string l;
        while (std::getline(in2, l)) {
            if (l.empty()) continue;
            size_t p2 = l.find_last_of('|');
            if (p2 != std::string::npos && l.substr(p2 + 1) != "1") {
                keep_lines.push_back(l);
            }
        }
        in2.close();
        std::ofstream out2(".chunkvault_repo/metadata/chunks.tbl", std::ios::trunc);
        for (const auto& l : keep_lines) {
            out2 << l << '\n';
        }
    }

    std::cout << "Garbage collection complete: " << to_delete.size()
              << " orphaned chunk(s) removed from disk.\n";
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];

    if (command == "add") {
        if (argc != 4) {
            std::cerr << "Usage: ChunkVault add <source_file> <cfs_name>\n";
            return 1;
        }
        return cmd_add(argv[2], argv[3]);
    } else if (command == "read") {
        if (argc != 4) {
            std::cerr << "Usage: ChunkVault read <cfs_name> <output_file>\n";
            return 1;
        }
        return cmd_read(argv[2], argv[3]);
    } else if (command == "del") {
        if (argc != 3) {
            std::cerr << "Usage: ChunkVault del <cfs_name>\n";
            return 1;
        }
        return cmd_del(argv[2]);
    } else if (command == "mod") {
        if (argc != 4) {
            std::cerr << "Usage: ChunkVault mod <new_source_file> <cfs_name>\n";
            return 1;
        }
        return cmd_mod(argv[2], argv[3]);
    } else if (command == "gc") {
        return cmd_gc();
    } else {
        std::cerr << "Unknown command: " << command << "\n\n";
        print_usage();
        return 1;
    }
}


