#ifndef FILE_UTILS_H
#define FILE_UTILS_H

#include <string>
using namespace std;

// File utility functions
string calculate_sha1(const string& input);
string get_file_name_from_path(const string& file_path);

#endif