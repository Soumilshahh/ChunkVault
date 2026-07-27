// Standalone test runner, no external framework (project has a hard
// "no external dependencies" requirement — pulling in gtest would
// violate that just for tests). Each TEST block runs in isolation
// against a scratch .chunkvault_repo/ under a fresh temp directory.
#include <iostream>
#include <fstream>
#include <filesystem>
#include <random>
#include <cassert>

#include "ChunkEngine.hpp"
#include "MerkleTreeEngine.hpp"
#include "StorageEngine.hpp"
#include "MetadataManager.hpp"

namespace fs = std::filesystem;

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond) do { \
    tests_run++; \
    if (!(cond)) { \
        tests_failed++; \
        std::cerr << "FAIL: " << #cond << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
    } \
} while (0)

#define RUN(fn) do { \
    std::cout << "-- " << #fn << "\n"; \
    fn(); \
} while (0)

// Runs the same add/read pipeline main.cpp's run_add + cmd_read use,
// but in-process, so tests don't depend on the built ChunkVault binary.
static uint64_t add_file(const std::string& src, const std::string& name,
                          ChunkEngine& chunker, MerkleTreeEngine& merkler,
                          StorageEngine& storage, MetadataManager& meta) {
    auto chunks = chunker.split_file_cdc(src);
    uint64_t total_size = 0;
    for (auto& c : chunks) total_size += c.data.size();

    for (auto& c : chunks) {
        if (!storage.chunk_exists(c.hash)) storage.save_block(c.hash, c.data);
    }

    std::unordered_map<std::string, MerkleNode> local;
    std::string root = merkler.build_tree(chunks, local);
    meta.merge_tree_nodes(local);
    for (auto& c : chunks) meta.increment_chunk(c.hash);
    meta.register_file(name, root, total_size);
    return chunks.size();
}

static void read_file(const std::string& name, const std::string& out,
                       MerkleTreeEngine& merkler, StorageEngine& storage,
                       MetadataManager& meta) {
    FileNode f = meta.get_file(name);
    auto seq = merkler.extract_leaf_sequence(f.root_hash, meta.get_full_registry());
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    for (auto& h : seq) {
        auto block = storage.read_block(h);
        o.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
}

static void write_random_file(const std::string& path, size_t n_bytes) {
    std::ofstream f(path, std::ios::binary);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < n_bytes; i++) {
        char c = static_cast<char>(dist(rng));
        f.write(&c, 1);
    }
}

static bool files_equal(const std::string& a, const std::string& b) {
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(fa)), {}) ==
           std::vector<char>((std::istreambuf_iterator<char>(fb)), {});
}

// Isolates each test in its own scratch cwd, since StorageEngine and
// MetadataManager hardcode paths relative to the current directory.
struct ScratchDir {
    fs::path original;
    fs::path scratch;
    ScratchDir() {
        original = fs::current_path();
        scratch = fs::temp_directory_path() / fs::path("ChunkVault_test_" + std::to_string(rand()));
        fs::create_directories(scratch);
        fs::current_path(scratch);
    }
    ~ScratchDir() {
        fs::current_path(original);
        fs::remove_all(scratch);
    }
};

// ---------------------------------------------------------------------
// ChunkEngine
// ---------------------------------------------------------------------
void test_chunk_engine_empty_file() {
    ScratchDir sd;
    ChunkEngine ce;
    write_random_file("empty.bin", 0);
    auto chunks = ce.split_file_cdc("empty.bin");
    CHECK(chunks.empty());
}

void test_chunk_engine_small_file_single_chunk() {
    ScratchDir sd;
    ChunkEngine ce;
    write_random_file("tiny.bin", 11);
    auto chunks = ce.split_file_cdc("tiny.bin");
    CHECK(chunks.size() == 1);
    CHECK(chunks[0].data.size() == 11);
}

void test_chunk_engine_deterministic() {
    ScratchDir sd;
    ChunkEngine ce;
    write_random_file("f.bin", 50000);
    auto c1 = ce.split_file_cdc("f.bin");
    auto c2 = ce.split_file_cdc("f.bin");
    CHECK(c1.size() == c2.size());
    for (size_t i = 0; i < c1.size(); i++) CHECK(c1[i].hash == c2[i].hash);
}

void test_chunk_engine_sha256_known_vector() {
    ChunkEngine ce;
    // SHA-256("") is a well-known test vector.
    CHECK(ce.compute_sha256(std::string("")) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// ---------------------------------------------------------------------
// MerkleTreeEngine
// ---------------------------------------------------------------------
void test_merkle_single_leaf() {
    MerkleTreeEngine me;
    std::unordered_map<std::string, MerkleNode> reg;
    std::vector<RawChunk> leaves = {{"h1", {}}};
    std::string root = me.build_tree(leaves, reg);
    CHECK(root == "h1");
    auto seq = me.extract_leaf_sequence(root, reg);
    CHECK(seq.size() == 1 && seq[0] == "h1");
}

void test_merkle_even_leaves() {
    MerkleTreeEngine me;
    std::unordered_map<std::string, MerkleNode> reg;
    std::vector<RawChunk> leaves = {{"h1", {}}, {"h2", {}}, {"h3", {}}, {"h4", {}}};
    std::string root = me.build_tree(leaves, reg);
    auto seq = me.extract_leaf_sequence(root, reg);
    CHECK(seq.size() == 4);
    CHECK(seq[0] == "h1" && seq[1] == "h2" && seq[2] == "h3" && seq[3] == "h4");
}

// Regression test for the odd-layer self-pairing bug: a lone leftover
// node must not be visited twice during reconstruction.
void test_merkle_odd_leaves_no_duplicate_leaf() {
    MerkleTreeEngine me;
    std::unordered_map<std::string, MerkleNode> reg;
    std::vector<RawChunk> leaves = {{"h1", {}}, {"h2", {}}, {"h3", {}}};
    std::string root = me.build_tree(leaves, reg);
    auto seq = me.extract_leaf_sequence(root, reg);
    CHECK(seq.size() == 3);
    CHECK(seq[0] == "h1" && seq[1] == "h2" && seq[2] == "h3");
}

void test_merkle_11_leaves_matches_documented_bug_case() {
    // The exact leaf count (11) that historically round-tripped to 16
    // chunks / corrupted output under the self-pairing bug.
    MerkleTreeEngine me;
    std::unordered_map<std::string, MerkleNode> reg;
    std::vector<RawChunk> leaves;
    for (int i = 0; i < 11; i++) leaves.push_back({"h" + std::to_string(i), {}});
    std::string root = me.build_tree(leaves, reg);
    auto seq = me.extract_leaf_sequence(root, reg);
    CHECK(seq.size() == 11);
    for (int i = 0; i < 11; i++) CHECK(seq[i] == "h" + std::to_string(i));
}

void test_merkle_empty_leaves() {
    MerkleTreeEngine me;
    std::unordered_map<std::string, MerkleNode> reg;
    std::string root = me.build_tree({}, reg);
    CHECK(root.empty());
    auto seq = me.extract_leaf_sequence(root, reg);
    CHECK(seq.empty());
}

// ---------------------------------------------------------------------
// StorageEngine
// ---------------------------------------------------------------------
void test_storage_engine_save_read_roundtrip() {
    ScratchDir sd;
    StorageEngine se;
    std::vector<char> data = {'a', 'b', 'c'};
    CHECK(se.save_block("deadbeef", data));
    CHECK(se.chunk_exists("deadbeef"));
    auto back = se.read_block("deadbeef");
    CHECK(back == data);
}

void test_storage_engine_destroy() {
    ScratchDir sd;
    StorageEngine se;
    se.save_block("abc123", {'x'});
    CHECK(se.chunk_exists("abc123"));
    se.destroy_block("abc123");
    CHECK(!se.chunk_exists("abc123"));
}

void test_storage_engine_missing_chunk_throws() {
    ScratchDir sd;
    StorageEngine se;
    bool threw = false;
    try { se.read_block("nonexistent"); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

// ---------------------------------------------------------------------
// MetadataManager
// ---------------------------------------------------------------------
void test_metadata_manager_persistence_roundtrip() {
    ScratchDir sd;
    {
        MetadataManager meta;
        meta.deserialize_from_disk();
        meta.register_file("f1", "roothash1", 100);
        meta.add_tree_node("roothash1", MerkleNode{"roothash1", "l", "r", false});
        meta.increment_chunk("chunkhash1");
        meta.serialize_to_disk();
    }
    {
        MetadataManager meta;
        meta.deserialize_from_disk();
        CHECK(meta.file_exists("f1"));
        FileNode f = meta.get_file("f1");
        CHECK(f.root_hash == "roothash1");
        CHECK(f.total_size == 100);
        MerkleNode n = meta.get_tree_node("roothash1");
        CHECK(n.left_child == "l" && n.right_child == "r");
    }
}

void test_metadata_manager_ref_counting_orphans_correctly() {
    ScratchDir sd;
    MetadataManager meta;
    meta.deserialize_from_disk();
    meta.increment_chunk("shared_chunk");
    meta.increment_chunk("shared_chunk"); // referenced by two files
    meta.increment_chunk("unique_chunk");

    std::vector<std::string> orphans;
    meta.decrement_chunk("shared_chunk", orphans);
    CHECK(orphans.empty()); // still referenced once, not orphaned

    meta.decrement_chunk("shared_chunk", orphans);
    CHECK(orphans.size() == 1 && orphans[0] == "shared_chunk");

    orphans.clear();
    meta.decrement_chunk("unique_chunk", orphans);
    CHECK(orphans.size() == 1 && orphans[0] == "unique_chunk");
}

void test_metadata_manager_clear_file_mapping_shared_chunk_survives() {
    ScratchDir sd;
    ChunkEngine chunker;
    MerkleTreeEngine merkler;
    StorageEngine storage;
    MetadataManager meta;
    meta.deserialize_from_disk();

    write_random_file("a.bin", 30000);
    add_file("a.bin", "fileA", chunker, merkler, storage, meta);

    // fileB shares fileA's tree entirely (re-adding same content under a
    // new name reuses every chunk/tree node).
    add_file("a.bin", "fileB", chunker, merkler, storage, meta);

    std::vector<std::string> orphans;
    meta.clear_file_mapping("fileA", orphans);
    CHECK(orphans.empty()); // fileB still references every chunk
    CHECK(!meta.file_exists("fileA"));
    CHECK(meta.file_exists("fileB"));
}

// ---------------------------------------------------------------------
// End-to-end (mirrors main.cpp's add/read/mod flow)
// ---------------------------------------------------------------------
void test_e2e_roundtrip_various_sizes() {
    for (size_t size : {0ul, 11ul, 2048ul, 50000ul, 2000000ul}) {
        ScratchDir sd;
        ChunkEngine chunker;
        MerkleTreeEngine merkler;
        StorageEngine storage;
        MetadataManager meta;
        meta.deserialize_from_disk();

        write_random_file("in.bin", size);
        add_file("in.bin", "name", chunker, merkler, storage, meta);
        read_file("name", "out.bin", merkler, storage, meta);

        CHECK(files_equal("in.bin", "out.bin"));
    }
}

void test_e2e_mod_dedups_shared_chunks() {
    ScratchDir sd;
    ChunkEngine chunker;
    MerkleTreeEngine merkler;
    StorageEngine storage;
    MetadataManager meta;
    meta.deserialize_from_disk();

    write_random_file("v1.bin", 30000);
    add_file("v1.bin", "f", chunker, merkler, storage, meta);

    // v2 = v1 + 5000 appended bytes.
    fs::copy_file("v1.bin", "v2.bin");
    { std::ofstream o("v2.bin", std::ios::binary | std::ios::app);
      std::mt19937 rng(7); std::uniform_int_distribution<int> d(0, 255);
      for (int i = 0; i < 5000; i++) { char c = static_cast<char>(d(rng)); o.write(&c, 1); } }

    std::vector<std::string> orphans;
    meta.clear_file_mapping("f", orphans);
    add_file("v2.bin", "f", chunker, merkler, storage, meta);

    read_file("f", "out.bin", merkler, storage, meta);
    CHECK(files_equal("v2.bin", "out.bin"));
}

void test_e2e_gc_removes_only_true_orphans() {
    ScratchDir sd;
    ChunkEngine chunker;
    MerkleTreeEngine merkler;
    StorageEngine storage;
    MetadataManager meta;
    meta.deserialize_from_disk();

    write_random_file("shared.bin", 20000);
    add_file("shared.bin", "fA", chunker, merkler, storage, meta);
    add_file("shared.bin", "fB", chunker, merkler, storage, meta); // same content

    FileNode fa = meta.get_file("fA");
    auto leaves = merkler.extract_leaf_sequence(fa.root_hash, meta.get_full_registry());

    std::vector<std::string> orphans;
    meta.clear_file_mapping("fA", orphans);
    CHECK(orphans.empty()); // fB still holds every chunk alive

    for (auto& h : leaves) CHECK(storage.chunk_exists(h)); // nothing physically deleted yet

    meta.clear_file_mapping("fB", orphans);
    CHECK(orphans.size() == leaves.size()); // now truly orphaned

    for (auto& h : orphans) storage.destroy_block(h);
    for (auto& h : leaves) CHECK(!storage.chunk_exists(h));
}

int main() {
    RUN(test_chunk_engine_empty_file);
    RUN(test_chunk_engine_small_file_single_chunk);
    RUN(test_chunk_engine_deterministic);
    RUN(test_chunk_engine_sha256_known_vector);

    RUN(test_merkle_single_leaf);
    RUN(test_merkle_even_leaves);
    RUN(test_merkle_odd_leaves_no_duplicate_leaf);
    RUN(test_merkle_11_leaves_matches_documented_bug_case);
    RUN(test_merkle_empty_leaves);

    RUN(test_storage_engine_save_read_roundtrip);
    RUN(test_storage_engine_destroy);
    RUN(test_storage_engine_missing_chunk_throws);

    RUN(test_metadata_manager_persistence_roundtrip);
    RUN(test_metadata_manager_ref_counting_orphans_correctly);
    RUN(test_metadata_manager_clear_file_mapping_shared_chunk_survives);

    RUN(test_e2e_roundtrip_various_sizes);
    RUN(test_e2e_mod_dedups_shared_chunks);
    RUN(test_e2e_gc_removes_only_true_orphans);

    std::cout << "\n" << tests_run << " checks run, " << tests_failed << " failed\n";
    return tests_failed == 0 ? 0 : 1;
}


