#pragma once

#include <string>

#include "core_reader.hpp"
#include "model.hpp"
#include "nix.hpp"
#include "source.hpp"

namespace nix_sym {

struct Options;

Backtrace symbolize_core(const Options& options, CoreFile& core, NixResolver& resolver,
                         SourceResolver& sources);

} // namespace nix_sym
