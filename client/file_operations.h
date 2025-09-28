#ifndef FILE_OPERATIONS_H
#define FILE_OPERATIONS_H

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <openssl/sha.h>
#include <filesystem>

namespace fs = std::filesystem;

// Structure to hold file chunk information
struct FileChunk {
    int chunk_number;
    std::string hash;
    bool is_available;
};

// Structure to hold file metadata
struct FileMetadata {
    std::string file_name;
    std::string file_path;
    uint64_t file_size;
    std::string file_hash;
    int total_chunks;
    std::vector<FileChunk> chunks;
};

// Function to calculate SHA1 hash of a file
std::string calculate_file_hash(const std::string& file_path);

// Function to split a file into chunks and calculate their hashes
FileMetadata split_file_into_chunks(const std::string& file_path, size_t chunk_size = 512 * 1024);

// Function to combine chunks back into a file
bool combine_chunks(const std::string& output_path, const std::string& temp_dir, int total_chunks);

// Function to verify the integrity of a downloaded file
bool verify_file_integrity(const std::string& file_path, const std::string& expected_hash);

// Function to save file metadata to disk
bool save_metadata(const FileMetadata& metadata, const std::string& output_path);

// Function to load file metadata from disk
FileMetadata load_metadata(const std::string& metadata_path);

// Function to get the chunk file path
std::string get_chunk_path(const std::string& base_dir, const std::string& file_name, int chunk_number);

#endif // FILE_OPERATIONS_H
