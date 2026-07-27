# ChunkVault — Content-Addressable Storage File System

A CLI implementing content-defined chunking (CDC), Merkle-tree indexing, and reference-counted deduplication in native C++17 — no external database, SHA-256 via OpenSSL.

```
ChunkVault add  <source_file> <cfs_name>      store a file under a name
ChunkVault read <cfs_name> <output_file>      reconstruct a file by name
ChunkVault mod  <new_source_file> <cfs_name>  replace a name's content, dedup shared chunks
ChunkVault del  <cfs_name>                    drop a name's mapping, orphan unreferenced chunks
ChunkVault gc                                 physically delete orphaned chunks
```

## Build

```
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make

./ChunkVault add test.txt greeting
./ChunkVault read greeting output.txt
diff test.txt output.txt
```

Requires CMake ≥ 3.16, C++17, OpenSSL dev headers (`libssl-dev` / `brew install openssl`).

## Architecture

```
                 main.cpp (CLI router)
                          |
      +-------------------+-------------------+
      |                   |                   |
 ChunkEngine       MerkleTreeEngine      MetadataManager
 CDC split +        build/traverse        file_table
 SHA-256             tree                 tree_registry
      |                   |               chunk_ledger
      |                   |               (RAM + flat-file
      +--------+  +-------+                persistence)
               |  |
          StorageEngine
          .chunkvault_repo/chunks/<hash>
```

`.chunkvault_repo/` (created on first run):

```
chunks/     one file per chunk, named by SHA-256 hex hash
metadata/   files.tbl, tree.tbl, chunks.tbl (flat, '|'-delimited)
```

**Add flow:** split file into content-defined chunks → write new chunks to disk (skip if hash already exists) → build Merkle tree over chunk hashes → bump ref count per chunk → point name at root hash.

**Read flow:** look up name's root hash → walk Merkle tree left-to-right to recover original chunk order → concatenate chunks to output.

**Mod:** unlink name from its old tree (decrementing ref counts) then re-run add — chunks shared between old and new content net out to the same ref count and are never touched on disk.

**Del/gc:** `del` decrements ref counts and marks anything hitting 0 as orphaned; `gc` is a separate step that actually deletes orphaned chunks (same split as `git gc`) since a short-lived CLI process can't run a background timer.

## Tests

```
cd build && ctest --output-on-failure
# or directly:
./ChunkVault_tests
```

18 test cases / 65 assertions, no external framework: `ChunkEngine` determinism and edge sizes (0, 11, 2048, 50000, 2000000 bytes), `MerkleTreeEngine` even/odd/single/empty leaf counts (including the 11-leaf case that previously triggered a self-pairing double-visit bug — see below), `StorageEngine` save/read/destroy, `MetadataManager` persistence round-trip and reference counting, and full add→read / add→mod→read / add→del→gc end-to-end flows.

## Known bug, found by testing

Odd-sized layers during Merkle tree construction were originally "fixed" by pairing the leftover node with itself (`parent = hash(lone + lone)`). This compiles and looks correct, but during reconstruction both children of a node are visited — so a self-paired leaf gets read and appended to the output twice. An 11-chunk 50,000-byte file round-tripped to 16 chunks / 80,886 bytes with no error.

Fix: single-child promotion. A lone node becomes the left child of a new parent whose `right_child` stays empty; traversal descends left only when `right_child` is empty. See `MerkleTreeEngine::build_tree` and `test_merkle_11_leaves_matches_documented_bug_case`.

## Known limitations

- Flat-file metadata reloads fully into RAM per command — fine at CLI scale (tested to ~400 chunks), not built for millions of chunks.
- No multi-process locking (`shared_mutex` guards threads within one process, not concurrent `ChunkVault` invocations).
- `gc` is a manual/cron step, not a literal background timer, since each `ChunkVault` invocation is a short-lived process.
- Assumes no SHA-256 collisions, same as Git/IPFS/restic/Borg.
