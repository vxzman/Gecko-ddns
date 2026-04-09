#pragma once
#include <string>

namespace cache {

std::string read_last_ip(const std::string& path);
bool        write_last_ip(const std::string& path, const std::string& ip);

} // namespace cache
