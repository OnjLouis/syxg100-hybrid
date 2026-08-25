#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace hybrid {

std::vector<std::uint8_t> loadLeImage(const std::filesystem::path& path,
                                      std::uint32_t loadBase);

} // namespace hybrid
