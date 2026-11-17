#pragma once

#include <string>

#include <vector>

#include "core_reader.hpp"
#include "model.hpp"
#include "nix.hpp"
#include "source.hpp"
#include "stack.hpp"

namespace nix_sym {

struct Options;
class DebuginfodClient;

Backtrace symbolize_core(const Options& options, CoreFile& core, NixResolver& resolver,
                         SourceResolver& sources, DebuginfodClient* debuginfod);

Backtrace symbolize_pid(const Options& options, int pid, NixResolver& resolver,
                        SourceResolver& sources, DebuginfodClient* debuginfod);

Backtrace symbolize_stack(const Options& options, const std::vector<StackRecord>& records,
                          NixResolver& resolver, SourceResolver& sources,
                          DebuginfodClient* debuginfod);

} // namespace nix_sym
