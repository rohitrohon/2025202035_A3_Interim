#include "file_utils.h"
#include <openssl/sha.h>
#include <string>
#include <sstream>
#include <iomanip>

using namespace std;

string calculate_sha1(const string& input) {
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(input.c_str()), input.size(), hash);
    
    stringstream ss;
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i) {
        ss << hex << setw(2) << setfill('0') << static_cast<int>(hash[i]);
    }
    
    return ss.str();
}

string get_file_name_from_path(const string& file_path) {
    size_t pos = file_path.find_last_of("/");
    if (pos != string::npos) {
        return file_path.substr(pos + 1);
    }
    return file_path;
}