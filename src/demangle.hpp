#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace nix_sym {

enum class Language { Unknown, C, Cxx, Rust, Haskell };

Language language_from_dwarf(unsigned value);
Language language_from_symbol(std::string_view name);
const char* language_name(Language language);

std::optional<std::string> demangle_rust(std::string_view name);
std::optional<std::string> demangle_haskell(std::string_view name);
std::string demangle_cxx(std::string_view name);
std::string demangle_symbol(std::string_view name, Language language);

} // namespace nix_sym
