#pragma once

#include <string>

#include "model.hpp"

namespace nix_sym {

std::string render_json(const Backtrace& backtrace);
std::string render_text(const Backtrace& backtrace, bool color, bool demangle);

std::string json_escape(const std::string& value);

} // namespace nix_sym
