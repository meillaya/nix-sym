#include "demangle.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nix_sym {
namespace {

// GHC info-table suffix forms, longest first so that "_static_info" and
// "_con_info" are not mistaken for "_info".
constexpr std::array<std::string_view, 5> kInfoSuffixes = {"_static_info", "_con_info", "_closure",
                                                           "_info", "_caf"};

bool is_ascii_digit(char c) {
  return c >= '0' && c <= '9';
}

bool is_ascii_upper(char c) {
  return c >= 'A' && c <= 'Z';
}

bool is_ascii_lower(char c) {
  return c >= 'a' && c <= 'z';
}

bool is_ascii_alpha(char c) {
  return is_ascii_upper(c) || is_ascii_lower(c);
}

bool is_ascii_alnum(char c) {
  return is_ascii_alpha(c) || is_ascii_digit(c);
}

bool is_printable_ascii(char c) {
  const auto value = static_cast<unsigned char>(c);
  return value >= 0x20 && value <= 0x7e;
}

bool ends_with(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Decoding z-encoded module names and z-encoded occurrence names differ in
// one place in practice: real GHC symbol names use "zi" for the dots that
// separate module path components (DataziList), while the documented escape
// table reserves the pair "zi" for '#'.  A module path can never contain '#',
// so decode "zi" as '.' while decoding the module part and as '#' while
// decoding the function part.
enum class ZContext { Module, Name };

std::optional<char> decode_lower_escape(char escape, ZContext /*context*/) {
  switch (escape) {
  case 'd':
    return '$';
  case 'c':
    return ':';
  case 'i':
    return '.';
  case 'm':
    return '-';
  case 'p':
    return '+';
  case 'q':
    return '?';
  case 'b':
    return '\\';
  case 'l':
    return '/';
  case 's':
    return ' ';
  case 'g':
    return '>';
  case 'e':
    return '=';
  // Useful escapes not listed in the task but emitted by GHC/zenc.
  case 'u':
    return '_';
  case 'h':
    return '#';
  case 'r':
    return '\\';
  case 'a':
    return '&';
  case 'n':
    return '!';
  case 'v':
    return '%';
  case 't':
    return '*';
  default:
    return std::nullopt;
  }
}

std::optional<char> decode_upper_escape(char escape) {
  switch (escape) {
  case 'L':
    return '(';
  case 'R':
    return ')';
  case 'M':
    return '[';
  case 'N':
    return ']';
  case 'P':
    return '@';
  case 'A':
    return '&';
  case 'B':
    return '|';
  case 'C':
    return '^';
  case 'D':
    return '$';
  case 'E':
    return '%';
  case 'F':
    return '!';
  case 'G':
    return '"';
  case 'H':
    return '\'';
  case 'K':
    return ';';
  case 'O':
    return '~';
  default:
    return std::nullopt;
  }
}

void append_utf8(std::string& out, std::uint32_t code_point) {
  if (code_point < 0x80) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    out.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else if (code_point < 0x10000) {
    out.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  }
}

void append_tuple(std::string& out, std::uint64_t arity, bool unboxed) {
  if (!unboxed) {
    out.push_back('(');
    if (arity > 1) {
      out.append(static_cast<std::size_t>(arity - 1), ',');
    }
    out.push_back(')');
    return;
  }

  out += "(#";
  if (arity == 1) {
    out.push_back(' ');
  } else if (arity > 1) {
    out.append(static_cast<std::size_t>(arity - 1), ',');
  }
  out += "#)";
}

// Decode a z-encoded fragment.  allow_raw_underscore is used only for the
// module path, where a literal '_' can be either a package/module separator or
// (in simplified encodings) part of a module component.
std::optional<std::string> z_decode(std::string_view input, ZContext context,
                                    bool allow_raw_underscore) {
  std::string out;
  out.reserve(input.size());

  std::size_t i = 0;
  while (i < input.size()) {
    const char c = input[i];
    if (c != 'z' && c != 'Z' && is_ascii_alnum(c)) {
      out.push_back(c);
      ++i;
      continue;
    }

    if (c == '_' && allow_raw_underscore) {
      out.push_back(c);
      ++i;
      continue;
    }

    if (c == 'z') {
      if (i + 1 >= input.size()) {
        return std::nullopt;
      }
      const char escape = input[i + 1];

      if (escape == 'z') {
        out.push_back('z');
        i += 2;
        continue;
      }

      if (is_ascii_digit(escape)) {
        std::uint32_t code_point = 0;
        bool overflow = false;
        std::size_t j = i + 1;
        while (j < input.size() && is_ascii_digit(input[j])) {
          if (code_point > 0x10ffffU) {
            overflow = true;
          } else {
            code_point = code_point * 10U + static_cast<std::uint32_t>(input[j] - '0');
          }
          ++j;
        }
        if (overflow || j >= input.size() || input[j] != 'U') {
          return std::nullopt;
        }
        if (code_point == 0 || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
          return std::nullopt;
        }
        append_utf8(out, code_point);
        i = j + 1;
        continue;
      }

      if (const auto decoded = decode_lower_escape(escape, context)) {
        out.push_back(*decoded);
        i += 2;
        continue;
      }

      // Be conservative about escapes not in the table: punctuation can be
      // passed through, but an unknown letter escape is treated as malformed.
      if (is_printable_ascii(escape) && !is_ascii_alnum(escape)) {
        out.push_back(escape);
        i += 2;
        continue;
      }
      return std::nullopt;
    }

    if (c == 'Z') {
      std::size_t j = i + 1;
      const std::size_t digits_begin = j;
      std::uint64_t arity = 0;
      while (j < input.size() && is_ascii_digit(input[j])) {
        if (arity <= 64) {
          arity = arity * 10U + static_cast<std::uint64_t>(input[j] - '0');
        }
        ++j;
      }

      if (j > digits_begin) {
        if (j >= input.size() || (input[j] != 'T' && input[j] != 'H')) {
          return std::nullopt;
        }
        if (arity > 64) {
          return std::nullopt;
        }
        append_tuple(out, arity, input[j] == 'H');
        i = j + 1;
        continue;
      }

      if (i + 1 >= input.size()) {
        return std::nullopt;
      }
      const char escape = input[i + 1];
      if (escape == 'Z') {
        out.push_back('Z');
        i += 2;
        continue;
      }
      if (const auto decoded = decode_upper_escape(escape)) {
        out.push_back(*decoded);
        i += 2;
        continue;
      }
      if (is_printable_ascii(escape) && !is_ascii_alnum(escape)) {
        out.push_back(escape);
        i += 2;
        continue;
      }
      return std::nullopt;
    }

    return std::nullopt;
  }

  return out;
}

bool is_valid_module_component(std::string_view component) {
  if (component.empty() || !is_ascii_upper(component.front())) {
    return false;
  }
  for (const char c : component) {
    if (!(is_ascii_alnum(c) || c == '_' || c == '\'')) {
      return false;
    }
  }
  return true;
}

bool is_valid_module_path(std::string_view path) {
  if (path.empty()) {
    return false;
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t dot = path.find('.', start);
    const std::string_view component =
        dot == std::string_view::npos ? path.substr(start) : path.substr(start, dot - start);
    if (!is_valid_module_component(component)) {
      return false;
    }
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1;
  }
  return true;
}

bool is_valid_package_name(std::string_view package) {
  if (package.empty() || !is_ascii_lower(package.front())) {
    return false;
  }
  for (const char c : package) {
    if (!(is_ascii_alnum(c) || c == '-' || c == '+' || c == '.' || c == '_')) {
      return false;
    }
  }
  return true;
}

// A package-qualified module looks like "base_GHCziIO".  GHC never emits a
// literal underscore inside a z-encoded module component, so the first literal
// underscore separates the package from the module path.  Also accept the
// simpler encoding where a module component contains a raw underscore by
// trying the no-package interpretation first.
std::optional<std::string> decode_module_path(std::string_view encoded) {
  if (const auto whole = z_decode(encoded, ZContext::Module, true)) {
    if (is_valid_module_path(*whole)) {
      return whole;
    }
  }

  const std::size_t separator = encoded.find('_');
  if (separator == std::string_view::npos || separator == 0 || separator + 1 >= encoded.size()) {
    return std::nullopt;
  }
  if (encoded.find('_', separator + 1) != std::string_view::npos) {
    return std::nullopt;
  }

  const auto package = z_decode(encoded.substr(0, separator), ZContext::Module, false);
  const auto module = z_decode(encoded.substr(separator + 1), ZContext::Module, false);
  if (!package || !module || !is_valid_package_name(*package) || !is_valid_module_path(*module)) {
    return std::nullopt;
  }
  return module;
}

bool is_valid_function_name(std::string_view function) {
  if (function.empty() || function.front() == '_' || is_ascii_digit(function.front())) {
    return false;
  }
  for (const unsigned char c : function) {
    if (c < 0x20 || c == 0x7f) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> demangle_core(std::string_view core, bool had_known_suffix) {
  if (core.empty() || core.front() == '_') {
    return std::nullopt;
  }

  const std::size_t split = core.rfind('_');
  if (split == std::string_view::npos || split == 0 || split + 1 >= core.size()) {
    return std::nullopt;
  }

  const std::string_view encoded_module = core.substr(0, split);
  const std::string_view encoded_function = core.substr(split + 1);

  // Without an info-table suffix, only a z-encoded module path is strong
  // enough evidence to call the symbol Haskell.  This keeps plain C names out.
  if (!had_known_suffix && encoded_module.find_first_of("zZ") == std::string_view::npos) {
    return std::nullopt;
  }

  const auto module = decode_module_path(encoded_module);
  const auto function = z_decode(encoded_function, ZContext::Name, false);
  if (!module || !function || !is_valid_function_name(*function)) {
    return std::nullopt;
  }

  std::string result = *module;
  result.push_back('.');
  result += *function;
  return result;
}

bool is_valid_user_suffix(std::string_view suffix) {
  if (suffix.empty()) {
    return true;
  }
  const char first = suffix.front();
  if (first != '_' && first != '$' && first != '.' && first != '@') {
    return false;
  }
  if (suffix.size() < 2) {
    return false;
  }
  for (std::size_t i = 1; i < suffix.size(); ++i) {
    const char c = suffix[i];
    if (!(is_ascii_alnum(c) || c == '_' || c == '.' || c == '$' || c == '-' || c == '@')) {
      return false;
    }
  }
  return true;
}

struct SuffixHit {
  std::string_view core;
  std::size_t length = 0;
};

std::vector<SuffixHit> collect_suffix_hits(std::string_view name) {
  std::vector<SuffixHit> hits;
  for (const std::string_view suffix : kInfoSuffixes) {
    if (ends_with(name, suffix)) {
      hits.push_back({name.substr(0, name.size() - suffix.size()), suffix.size()});
    }

    std::size_t pos = name.rfind(suffix);
    while (pos != std::string_view::npos) {
      const std::size_t end = pos + suffix.size();
      if (end < name.size() && is_valid_user_suffix(name.substr(end)) && pos > 0) {
        hits.push_back({name.substr(0, pos), suffix.size()});
      }
      if (pos == 0) {
        break;
      }
      pos = name.rfind(suffix, pos - 1);
    }
  }
  return hits;
}

} // namespace

std::optional<std::string> demangle_haskell(std::string_view name) {
  if (name.size() < 3 || name.front() == '_') {
    return std::nullopt;
  }

  std::vector<SuffixHit> hits = collect_suffix_hits(name);
  std::sort(hits.begin(), hits.end(), [](const SuffixHit& left, const SuffixHit& right) {
    if (left.length != right.length) {
      return left.length > right.length;
    }
    return left.core.size() > right.core.size();
  });

  std::string_view previous_core;
  bool has_previous = false;
  for (const SuffixHit& hit : hits) {
    if (has_previous && hit.core == previous_core) {
      continue;
    }
    has_previous = true;
    previous_core = hit.core;
    if (auto demangled = demangle_core(hit.core, true)) {
      return demangled;
    }
  }

  // Suffixless symbols are accepted only when the module path itself is
  // z-encoded (for example DataziList_map or GHCziIO_putStrLn).
  return demangle_core(name, false);
}

} // namespace nix_sym
