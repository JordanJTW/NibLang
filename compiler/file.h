/*
 * Copyright (C) 2026, Jordan Werthman <jordanwerthman@gmail.com>.
 *
 * This software is dual-licensed. You may use this file under the terms of
 * the GNU General Public License v3.0 (GPL-3.0) OR under a commercial
 * license obtained directly from the copyright holder.
 *
 * See the LICENSE.md file in the project root for full licensing details.
 *
 * SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-Commercial
 */

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