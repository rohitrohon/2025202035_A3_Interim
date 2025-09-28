cmake_minimum_required(VERSION 3.10)
project(p2p_file_sharing)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Add main client executable
add_executable(p2p_client
    client.cpp
    file_operations.cpp
    network_utils.cpp
)

target_include_directories(p2p_client PRIVATE 
    ${OPENSSL_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(p2p_client PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    ${CMAKE_THREAD_LIBS_INIT}
)
