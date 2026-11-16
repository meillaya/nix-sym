#pragma once

#include <string>
#include <vector>

namespace nix_sym {

struct ProcessResult {
  bool started = false;
  bool finished = false;
  bool timed_out = false;
  int exit_code = -1;
  int signal = 0;
  std::string out;
  std::string err;
};

ProcessResult run_process(const std::vector<std::string>& argv, int timeout_seconds = 120,
                          const std::string& stdin_data = {});

} // namespace nix_sym
