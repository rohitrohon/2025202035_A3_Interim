#include "dht.h"
#include <iostream>
#include <thread>

using namespace p2p;

int main() {
    try {
        std::cout << "Starting DHT node on port 6881..." << std::endl;
        
        // Create and start DHT node
        Dht dht("", 6881);
        dht.start();
        
        // Print node ID
        auto node_id = dht.get_node_id();
        std::cout << "DHT node ID: ";
        for (auto b : node_id) {
            std::cout << std::hex << (int)b;
        }
        std::cout << std::dec << std::endl;
        
        // Add some bootstrap nodes
        std::cout << "Adding bootstrap nodes..." << std::endl;
        dht.add_bootstrap_node("router.bittorrent.com", 6881);
        dht.add_bootstrap_node("dht.libtorrent.org", 25401);
        
        // Keep running for 30 seconds
        std::cout << "Running for 30 seconds..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(30));
        
        // Stop the DHT
        dht.stop();
        std::cout << "DHT stopped." << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
