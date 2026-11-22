#pragma once

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

namespace OtaPolicy {
inline bool allowedUrl(const std::string &url) {
  if (url.size() > 8192 || url.compare(0, 8, "https://") != 0 ||
      url.find_first_of("\r\n\\#") != std::string::npos) return false;
  const auto end = url.find_first_of("/?", 8);
  const auto host = url.substr(8, end == std::string::npos ? end : end - 8);
  return host == "github.com" || host == "api.github.com" ||
         host == "release-assets.githubusercontent.com" || host == "objects.githubusercontent.com";
}

inline bool checksum(const std::string &text, const std::string &filename, std::string &digest) {
  std::istringstream lines(text);
  std::string line;
  bool found = false;
  while (std::getline(lines, line)) {
    std::istringstream fields(line);
    std::string hash, name, extra;
    if (!(fields >> hash >> name)) continue;
    if (name == "*" + filename) name.erase(0, 1);
    if (name != filename) continue;
    if (found || (fields >> extra) || hash.size() != 64 ||
        !std::all_of(hash.begin(), hash.end(), [](unsigned char c) { return std::isxdigit(c); })) return false;
    std::transform(hash.begin(), hash.end(), hash.begin(), [](unsigned char c) { return std::tolower(c); });
    digest = hash;
    found = true;
  }
  return found;
}
}  // namespace OtaPolicy
