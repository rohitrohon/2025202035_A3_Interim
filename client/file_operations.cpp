#include "file_operations.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <openssl/sha.h>
#include <vector>
#include <cstring>
#include <sys/stat.h>   // mkdir
#include <sys/types.h>
#include <unistd.h>

using namespace std;

const size_t CHUNK_SIZE = 512 * 1024; // 512 KB fixed chunk size

// --------------------------------------------
// SHA1 calculation helpers
// --------------------------------------------
string calculate_sha1(const string& data) {
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(data.c_str()), data.size(), hash);

    stringstream ss;
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
        ss << hex << setw(2) << setfill('0') 
           << static_cast<int>(hash[i]);
    }

    return ss.str();
}

string calculate_file_hash(const string& file_path) {
    ifstream file(file_path, ios::binary);
    if (!file) {
        throw runtime_error("Could not open file: " + file_path);
    }

    const size_t buffer_size = 8192;
    char buffer[buffer_size];
    unsigned char hash[SHA_DIGEST_LENGTH] = {0};
    SHA_CTX sha1_context;

    SHA1_Init(&sha1_context);

    while (file.good()) {
        file.read(buffer, buffer_size);
        SHA1_Update(&sha1_context, buffer, file.gcount());
    }

    SHA1_Final(hash, &sha1_context);

    stringstream ss;
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
        ss << hex << setw(2) << setfill('0') 
           << static_cast<int>(hash[i]);
    }

    return ss.str();
}

// --------------------------------------------
// Verify a chunk hash immediately after writing
// --------------------------------------------
bool verify_chunk(const string& chunk_path, const string& expected_hash) {
    ifstream file(chunk_path, ios::binary);
    if (!file) return false;

    ostringstream ss;
    ss << file.rdbuf();
    string actual_hash = calculate_sha1(ss.str());
    return (actual_hash == expected_hash);
}

// --------------------------------------------
// Split file into fixed 512KB chunks
// --------------------------------------------
FileMetadata split_file_into_chunks(const string& file_path) {
    FileMetadata metadata;
    metadata.file_path = file_path;
    metadata.file_name = file_path.substr(file_path.find_last_of("/\\") + 1);

    // Open the input file
    ifstream input_file(file_path, ios::binary | ios::ate);
    if (!input_file) {
        throw runtime_error("Could not open file: " + file_path);
    }

    // Get file size
    metadata.file_size = input_file.tellg();
    input_file.seekg(0, ios::beg);

    // Calculate number of chunks
    metadata.total_chunks = (metadata.file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;

    // Create a temporary directory for chunks
    string temp_dir = "./chunks/" + metadata.file_name + "_chunks";
    mkdir("./chunks", 0777);   // ensure parent dir exists
    mkdir(temp_dir.c_str(), 0777);

    // Buffer for reading
    vector<char> buffer(CHUNK_SIZE);
    int chunk_number = 0;

    // Read and split the file
    while (input_file) {
        input_file.read(buffer.data(), CHUNK_SIZE);
        streamsize bytes_read = input_file.gcount();

        if (bytes_read > 0) {
            string chunk_filename = get_chunk_path(temp_dir, metadata.file_name, chunk_number);

            // Write the chunk to disk
            ofstream chunk_file(chunk_filename, ios::binary);
            if (!chunk_file) {
                throw runtime_error("Could not create chunk file: " + chunk_filename);
            }

            chunk_file.write(buffer.data(), bytes_read);
            chunk_file.close();

            // Calculate chunk hash
            string chunk_hash = calculate_sha1(string(buffer.data(), bytes_read));

            // Verify chunk immediately
            bool verified = verify_chunk(chunk_filename, chunk_hash);

            // Store chunk metadata
            FileChunk chunk_info;
            chunk_info.chunk_number = chunk_number;
            chunk_info.hash = chunk_hash;
            chunk_info.is_available = true;
            chunk_info.is_verified = verified;
            metadata.chunks.push_back(chunk_info);

            chunk_number++;
        }
    }

    // Calculate the overall file hash
    metadata.file_hash = calculate_file_hash(file_path);

    return metadata;
}

// --------------------------------------------
// Combine chunks back into a single file
// --------------------------------------------
bool combine_chunks(const string& output_path, const string& temp_dir, int total_chunks) {
    ofstream output_file(output_path, ios::binary);
    if (!output_file) {
        cerr << "Could not create output file: " << output_path << endl;
        return false;
    }

    for (int i = 0; i < total_chunks; i++) {
        string chunk_path = temp_dir + "/chunk_" + to_string(i);
        ifstream chunk_file(chunk_path, ios::binary);

        if (!chunk_file) {
            cerr << "Could not open chunk file: " << chunk_path << endl;
            return false;
        }

        // Read and write the chunk
        output_file << chunk_file.rdbuf();
        chunk_file.close();
    }

    output_file.close();
    return true;
}

// --------------------------------------------
// Verify full file integrity
// --------------------------------------------
bool verify_file_integrity(const string& file_path, const string& expected_hash) {
    try {
        string actual_hash = calculate_file_hash(file_path);
        return (actual_hash == expected_hash);
    } catch (const exception& e) {
        cerr << "Error verifying file integrity: " << e.what() << endl;
        return false;
    }
}

// --------------------------------------------
// Save metadata to file
// --------------------------------------------
bool save_metadata(const FileMetadata& metadata, const string& output_path) {
    ofstream out_file(output_path);
    if (!out_file) {
        return false;
    }

    out_file << "file_name:" << metadata.file_name << "\n";
    out_file << "file_path:" << metadata.file_path << "\n";
    out_file << "file_size:" << metadata.file_size << "\n";
    out_file << "file_hash:" << metadata.file_hash << "\n";
    out_file << "total_chunks:" << metadata.total_chunks << "\n";

    out_file << "chunks:\n";
    for (const auto& chunk : metadata.chunks) {
        out_file << chunk.chunk_number << ","
                 << chunk.hash << ","
                 << chunk.is_available << ","
                 << chunk.is_verified << "\n";
    }

    out_file.close();
    return true;
}

// --------------------------------------------
// Load metadata back
// --------------------------------------------
FileMetadata load_metadata(const string& metadata_path) {
    FileMetadata metadata;
    ifstream in_file(metadata_path);

    if (!in_file) {
        throw runtime_error("Could not open metadata file: " + metadata_path);
    }

    string line;
    while (getline(in_file, line)) {
        if (line == "chunks:") {
            // Start reading chunks
            while (getline(in_file, line)) {
                size_t c1 = line.find(',');
                size_t c2 = line.find(',', c1 + 1);
                size_t c3 = line.find(',', c2 + 1);

                if (c1 != string::npos && c2 != string::npos && c3 != string::npos) {
                    FileChunk chunk;
                    chunk.chunk_number = stoi(line.substr(0, c1));
                    chunk.hash = line.substr(c1 + 1, c2 - c1 - 1);
                    chunk.is_available = (line.substr(c2 + 1, c3 - c2 - 1) == "1");
                    chunk.is_verified = (line.substr(c3 + 1) == "1");
                    metadata.chunks.push_back(chunk);
                }
            }
        } else {
            size_t colon = line.find(':');
            if (colon != string::npos) {
                string key = line.substr(0, colon);
                string value = line.substr(colon + 1);

                if (key == "file_name") metadata.file_name = value;
                else if (key == "file_path") metadata.file_path = value;
                else if (key == "file_size") metadata.file_size = stoull(value);
                else if (key == "file_hash") metadata.file_hash = value;
                else if (key == "total_chunks") metadata.total_chunks = stoi(value);
            }
        }
    }

    return metadata;
}

// --------------------------------------------
// Chunk path generator
// --------------------------------------------
string get_chunk_path(const string& base_dir, const string& file_name, int chunk_number) {
    return base_dir + "/chunk_" + to_string(chunk_number);
}

// --------------------------------------------
// File path utilities
// --------------------------------------------
string get_file_name_from_path(const string& file_path) {
    size_t pos = file_path.find_last_of("/");
    if (pos != string::npos) {
        return file_path.substr(pos + 1);
    }
    return file_path;
}

// --------------------------------------------
// Networking placeholders (to be integrated)
// --------------------------------------------
// send_chunk(socket_fd, chunk_path)
// recv_chunk(socket_fd, expected_hash, save_path)
