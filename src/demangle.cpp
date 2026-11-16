#include "demangle.hpp"

#include <cxxabi.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nix_sym {
namespace {

bool starts_with(std::string_view value, std::string_view prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool ends_with(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool looks_like_rust_legacy(std::string_view name) {
  if (!starts_with(name, "_ZN") || !ends_with(name, "E")) {
    return false;
  }
  size_t h = name.rfind("17h");
  if (h == std::string_view::npos || h + 19 != name.size() - 1) {
    return false;
  }
  for (size_t i = h + 3; i < name.size() - 1; ++i) {
    char c = name[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

std::string cxx_demangle_base(std::string_view base) {
  if (!starts_with(base, "_Z") && !starts_with(base, "__Z")) {
    return {};
  }
  int status = 0;
  char* demangled = abi::__cxa_demangle(std::string(base).c_str(), nullptr, nullptr, &status);
  if (status != 0 || demangled == nullptr) {
    std::free(demangled);
    return {};
  }
  std::string result(demangled);
  std::free(demangled);
  return result;
}

bool is_symbol_suffix(std::string_view suffix) {
  return starts_with(suffix, ".cold") || starts_with(suffix, ".isra.") ||
         starts_with(suffix, ".constprop.") || starts_with(suffix, ".llvm.") ||
         starts_with(suffix, ".part.") || starts_with(suffix, ".clone.") ||
         starts_with(suffix, ".lto_priv.") || starts_with(suffix, ".localalias");
}

} // namespace

Language language_from_dwarf(unsigned value) {
  switch (value) {
  case 0x0002:
  case 0x000c:
  case 0x001d:
  case 0x002c:
    return Language::C;
  case 0x0004:
  case 0x0019:
  case 0x001a:
  case 0x0021:
  case 0x002a:
  case 0x002b:
  case 0x003a:
    return Language::Cxx;
  case 0x0018:
    return Language::Haskell;
  case 0x001c:
    return Language::Rust;
  default:
    return Language::Unknown;
  }
}

Language language_from_symbol(std::string_view name) {
  if (starts_with(name, "_R") || starts_with(name, "__R") || looks_like_rust_legacy(name)) {
    return Language::Rust;
  }
  if (starts_with(name, "_Z") || starts_with(name, "__Z")) {
    return Language::Cxx;
  }
  if (ends_with(name, "_info") || ends_with(name, "_closure") || ends_with(name, "_con_info") ||
      ends_with(name, "_static_info")) {
    if (name.find('z') != std::string_view::npos || name.find('Z') != std::string_view::npos) {
      return Language::Haskell;
    }
  }
  return Language::C;
}

const char* language_name(Language language) {
  switch (language) {
  case Language::C:
    return "c";
  case Language::Cxx:
    return "c++";
  case Language::Rust:
    return "rust";
  case Language::Haskell:
    return "haskell";
  case Language::Unknown:
    return "unknown";
  }
  return "unknown";
}

std::string demangle_cxx(std::string_view name) {
  std::string raw(name);
  std::string suffix;
  size_t dot = raw.find('.');
  size_t at = raw.find('@');
  size_t cut = std::min(dot == std::string::npos ? raw.size() : dot,
                        at == std::string::npos ? raw.size() : at);

  std::string base = raw.substr(0, cut);
  std::string tail = raw.substr(cut);
  std::string decoded = cxx_demangle_base(base);
  if (decoded.empty()) {
    return raw;
  }
  if (dot != std::string::npos && at == std::string::npos) {
    std::string possible_suffix = raw.substr(dot);
    if (is_symbol_suffix(possible_suffix)) {
      return decoded + possible_suffix;
    }
  }
  return decoded;
}

std::string demangle_symbol(std::string_view name, Language language) {
  if (name.empty()) {
    return std::string(name);
  }
  if (language == Language::Rust) {
    if (auto rust = demangle_rust(name)) {
      return *rust;
    }
  }
  if (language == Language::Haskell) {
    if (auto haskell = demangle_haskell(name)) {
      return *haskell;
    }
  }
  if (language == Language::Cxx || starts_with(name, "_Z") || starts_with(name, "__Z")) {
    std::string demangled = demangle_cxx(name);
    if (demangled != name) {
      return demangled;
    }
  }
  if (language == Language::Unknown || language == Language::C) {
    if (auto rust = demangle_rust(name)) {
      return *rust;
    }
    if (auto haskell = demangle_haskell(name)) {
      return *haskell;
    }
  }
  return std::string(name);
}

} // namespace nix_sym
