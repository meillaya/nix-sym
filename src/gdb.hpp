#pragma once

#include <string>
#include <vector>

#include "model.hpp"
#include "source.hpp"

namespace nix_sym {

std::string render_gdb_init(const Backtrace& backtrace, const std::string& core_path,
                            const std::vector<SourceMapping>& mappings, bool live = false);

} // namespace nix_sym
