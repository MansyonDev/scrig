#pragma once
#include <string>
#include <cstdint>

namespace scrig::config {

struct Settings {
  std::string node_address;
  std::string miner_public_base36;
  int32_t threads_count;
  bool full_dataset;
};

Settings load_or_create(const std::string& path);

}