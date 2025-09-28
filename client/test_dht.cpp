#include "dht.h"
#include <thread>
#include <chrono>
#include <iomanip>
#include <iostream>

using namespace p2p;
using namespace std::chrono_literals;

int main() {
    try {
        // Create a DHT node on port 6881
        Dht dht("", 6881);
        
        // Set up callbacks
        dht.set_node_discovered_callback([](const DhtNode& node) {
            std::cout << "Discovered node: " << node.address << ":" << node.port << std::endl;
        });
        
        dht.set_peers_found_callback([](const std::vector<DhtPeer>& peers) {
            std::cout << "Found " << peers.size() << " peers:" << std::endl;
            for (const auto& peer : peers) {
                std::cout << "  - " << peer.address << ":" << peer.port << std::endl;
            }
        });
        
        dht.set_error_callback([](const std::string& error) {
            std::cerr << "DHT Error: " << error << std::endl;
        });
        
        // Start the DHT
        dht.start();
        
        // Add some bootstrap nodes (public DHT nodes)
        dht.add_bootstrap_node("router.bittorrent.com", 6881);
        dht.add_bootstrap_node("dht.libtorrent.org", 25401);
        dht.add_bootstrap_node("router.utorrent.com", 6881);
        
        std::cout << "DHT node started with ID: ";
        auto node_id = dht.get_node_id();
        for (auto b : node_id) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)b;
        }
        std::cout << std::dec << std::endl;
        
        // Keep the DHT running for a while
        std::cout << "DHT running for 60 seconds..." << std::endl;
        std::this_thread::sleep_for(10s);
        
        // Try to find peers for a test info hash
        std::vector<uint8_t> info_hash(20, 0); // Replace with a real info hash
        std::cout << "Searching for peers..." << std::endl;
        dht.find_peers(info_hash, [](const std::vector<DhtPeer>& peers) {
            if (!peers.empty()) {
                std::cout << "Found " << peers.size() << " peers!" << std::endl;
            } else {
                std::cout << "No peers found." << std::endl;
            }
        });
        
        // Wait a bit more
        std::this_thread::sleep_for(50s);
        
        // Stop the DHT
        dht.stop();
        std::cout << "DHT stopped." << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
