#include "cache.hpp"
#include <fstream>
#include <string>
#include <sys/stat.h>

namespace cache {

namespace {

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

} // namespace

CacheFileData parse_cache_file(const std::string& path) {
    CacheFileData data;

    std::ifstream f(path);
    if (!f.is_open()) return data;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        std::string ip;
        auto space_pos = line.find(' ');
        if (space_pos == std::string::npos) {
            ip = line;
        } else {
            ip = trim(line.substr(space_pos + 1));
        }
        if (!ip.empty()) data.last_ip = ip;
    }

    return data;
}

std::string read_last_ip(const std::string& path) {
    return parse_cache_file(path).last_ip;
}

bool write_last_ip(const std::string& path, const std::string& ip) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;

    f << ip << '\n';
    if (!f.good()) return false;
    f.close();

    if (chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        return false;
    }
    return true;
}

} // namespace cache
