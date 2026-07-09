#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "compiler/types.h"

struct File {
  std::string resolved_path;
  std::vector<std::string> import_paths;
  Block root_block;
  std::string file_contents;
  size_t file_id;
};