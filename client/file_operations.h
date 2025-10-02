#ifndef FILE_OPERATIONS_H
#define FILE_OPERATIONS_H

#include <string>
#include <cstdint>  // for uint64_t
#include <vector>
#include <openssl/sha.h>

using namespace std;

// --------------------------------------------
// Structures for metadata
// --------------------------------------------
struct FileChunk {
    int chunk_number;
    std::string hash;
    bool is_available;
    bool is_verified;   // NEW: track per-chunk verification
};

struct FileMetadata {
    std::string file_name;
    std::string file_path;
    uint64_t file_size;
    std::string file_hash;
    int total_chunks;
    std::vector<FileChunk> chunks;
};

// --------------------------------------------
// Function declarations
// --------------------------------------------

// Hash utilities
std::string calculate_sha1(const std::string& data);
std::string calculate_file_hash(const std::string& file_path);
bool verify_chunk(const std::string& chunk_path, const std::string& expected_hash);

// File splitting and combining
FileMetadata split_file_into_chunks(const std::string& file_path);  // fixed 512KB chunk size
bool combine_chunks(const std::string& output_path, const std::string& temp_dir, int total_chunks);

// File integrity
bool verify_file_integrity(const std::string& file_path, const std::string& expected_hash);

// Metadata operations
bool save_metadata(const FileMetadata& metadata, const std::string& output_path);
FileMetadata load_metadata(const std::string& metadata_path);

// Chunk path generator
std::string get_chunk_path(const std::string& base_dir, const std::string& file_name, int chunk_number);

// File path utilities
std::string get_file_name_from_path(const std::string& file_path);

#endif // FILE_OPERATIONS_H