#include "file_operations.h"
#include <iostream>
#include <cassert>

int main() {
    try {
        // Test file path - change this to an existing file on your system
        std::string test_file = "test_file.txt";
        
        // Create a test file with some content
        {
            std::ofstream out_file(test_file);
            out_file << "This is a test file for P2P file sharing system.\n";
            out_file << "This file will be split into chunks and then recombined.\n";
            out_file.close();
        }
        
        // Test file hashing
        std::string file_hash = calculate_file_hash(test_file);
        std::cout << "File hash: " << file_hash << std::endl;
        
        // Test file splitting
        std::cout << "Splitting file into chunks..." << std::endl;
        FileMetadata metadata = split_file_into_chunks(test_file, 32); // Small chunk size for testing
        
        std::cout << "File: " << metadata.file_name << std::endl;
        std::cout << "Size: " << metadata.file_size << " bytes" << std::endl;
        std::cout << "Total chunks: " << metadata.total_chunks << std::endl;
        std::cout << "Chunk hashes:" << std::endl;
        
        for (const auto& chunk : metadata.chunks) {
            std::cout << "  Chunk " << chunk.chunk_number << ": " << chunk.hash << std::endl;
        }
        
        // Save metadata
        save_metadata(metadata, "file_metadata.txt");
        
        // Load metadata
        FileMetadata loaded_metadata = load_metadata("file_metadata.txt");
        
        // Verify loaded metadata
        assert(loaded_metadata.file_name == metadata.file_name);
        assert(loaded_metadata.file_size == metadata.file_size);
        assert(loaded_metadata.total_chunks == metadata.total_chunks);
        
        // Test combining chunks
        std::string output_file = "recombined_" + test_file;
        std::string temp_dir = "./chunks/" + metadata.file_name + "_chunks";
        
        std::cout << "Recombining chunks into " << output_file << "..." << std::endl;
        bool success = combine_chunks(output_file, temp_dir, metadata.total_chunks);
        
        if (success) {
            std::cout << "Successfully recombined file." << std::endl;
            
            // Verify file integrity
            bool integrity_ok = verify_file_integrity(output_file, metadata.file_hash);
            std::cout << "File integrity check: " << (integrity_ok ? "PASSED" : "FAILED") << std::endl;
            
            if (integrity_ok) {
                std::cout << "Test completed successfully!" << std::endl;
                return 0;
            }
        } else {
            std::cerr << "Failed to recombine file." << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    
    return 1;
}
