cmake_minimum_required(VERSION 3.16)
project(ChunkVault LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(OpenSSL REQUIRED)

add_executable(ChunkVault
    src/main.cpp
    src/ChunkEngine.cpp
    src/MerkleTreeEngine.cpp
    src/StorageEngine.cpp
    src/MetadataManager.cpp
)

target_include_directories(ChunkVault PRIVATE include)
target_link_libraries(ChunkVault PRIVATE OpenSSL::Crypto)
target_compile_options(ChunkVault PRIVATE -Wall -Wextra)

# --- Tests ---
enable_testing()
add_executable(ChunkVault_tests
    tests/test_main.cpp
    src/ChunkEngine.cpp
    src/MerkleTreeEngine.cpp
    src/StorageEngine.cpp
    src/MetadataManager.cpp
)
target_include_directories(ChunkVault_tests PRIVATE include)
target_link_libraries(ChunkVault_tests PRIVATE OpenSSL::Crypto)
add_test(NAME ChunkVault_tests COMMAND ChunkVault_tests)

