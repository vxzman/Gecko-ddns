#include "cache.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fstream>
#include <ranges>
#include <string>
#include <sys/stat.h>

namespace cache {

std::string read_last_ip(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string ip;
    std::getline(f, ip);
    auto is_space = [](unsigned char c) { return std::isspace(c); };
    auto trimmed = ip | std::views::reverse | std::views::drop_while(is_space) | std::views::reverse;
    return std::string(trimmed.begin(), trimmed.end());
}

bool write_last_ip(const std::string& path, const std::string& ip) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << ip;
    if (!f.good()) return false;
    f.close();
    
    // Set file permissions to 0600 (owner read/write only)
    if (chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        return false;
    }
    return true;
}

} // namespace cache
