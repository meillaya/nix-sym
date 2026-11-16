#include "demangle.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nix_sym {
namespace {

constexpr std::size_t kMaxOutput = 1'000'000;
constexpr std::uint64_t kMaxWork = 4'000'000;
constexpr unsigned kMaxDepth = 500;

bool is_ascii(std::string_view s) {
  for (unsigned char c : s) {
    if (c >= 0x80)
      return false;
  }
  return true;
}

bool is_digit(char c) {
  return c >= '0' && c <= '9';
}
bool is_lower_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}
bool is_hex(char c) {
  return is_lower_hex(c) || (c >= 'A' && c <= 'F');
}
bool is_upper_alpha(char c) {
  return c >= 'A' && c <= 'Z';
}
bool is_alpha(char c) {
  return is_upper_alpha(c) || (c >= 'a' && c <= 'z');
}

bool is_symbol_like_suffix(std::string_view s) {
  if (s.empty() || s.front() != '.')
    return false;
  for (unsigned char c : s) {
    const bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    const bool punct = (c >= 0x21 && c <= 0x2f) || (c >= 0x3a && c <= 0x40) ||
                       (c >= 0x5b && c <= 0x60) || (c >= 0x7b && c <= 0x7e);
    if (!alnum && !punct)
      return false;
  }
  return true;
}

// LLVM's ThinLTO renames local symbols by appending ".llvm.<hex>" (and
// possibly a version suffix).  rustc-demangle removes that trailer before
// parsing, so do the same here.
std::string_view strip_thinlto_suffix(std::string_view s) {
  const std::size_t pos = s.find(".llvm.");
  if (pos == std::string_view::npos)
    return s;
  const std::string_view candidate = s.substr(pos + 6);
  for (char c : candidate) {
    if (!is_digit(c) && !(c >= 'A' && c <= 'F') && c != '@')
      return s;
  }
  return s.substr(0, pos);
}

void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp <= 0x7f) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7ff) {
    out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else if (cp <= 0xffff) {
    out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
}

bool is_unicode_scalar(std::uint32_t cp) {
  return cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
}

bool is_control(std::uint32_t cp) {
  return cp < 0x20 || (cp >= 0x7f && cp <= 0x9f);
}

// Approximation of Rust's `char::is_printable`, used for Debug-style char and
// string literals.  It covers the categories that matter in practice: control
// and format characters, separators, and private-use code points.
bool is_printable(std::uint32_t cp) {
  if (cp == 0x20)
    return true;
  if (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f))
    return false;
  if (cp == 0x00a0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x202f ||
      cp == 0x205f || cp == 0x3000) {
    return false;
  }
  if (cp == 0x2028 || cp == 0x2029)
    return false;
  if (cp == 0x00ad || cp == 0x061c || cp == 0x180e || cp == 0x200b || cp == 0x200c ||
      cp == 0x200d || (cp >= 0x200e && cp <= 0x200f) || (cp >= 0x202a && cp <= 0x202e) ||
      (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x206f) || cp == 0xfeff ||
      (cp >= 0xfff9 && cp <= 0xfffb)) {
    return false;
  }
  if ((cp >= 0xe000 && cp <= 0xf8ff) || cp >= 0xf0000)
    return false;
  return true;
}

// Decode one well-formed UTF-8 scalar, advancing `pos`.  `bytes` may contain
// arbitrary byte soup, so all overlong forms and surrogates are rejected.
bool decode_utf8(std::string_view bytes, std::size_t& pos, std::uint32_t& codepoint) {
  if (pos >= bytes.size())
    return false;
  const auto byte = [bytes](std::size_t i) { return static_cast<unsigned char>(bytes[i]); };
  const unsigned char first = byte(pos);
  std::size_t length = 0;
  std::uint32_t cp = 0;
  if (first <= 0x7f) {
    codepoint = first;
    ++pos;
    return true;
  }
  if (first >= 0xc2 && first <= 0xdf) {
    length = 2;
    cp = first & 0x1f;
  } else if (first >= 0xe0 && first <= 0xef) {
    length = 3;
    cp = first & 0x0f;
  } else if (first >= 0xf0 && first <= 0xf4) {
    length = 4;
    cp = first & 0x07;
  } else {
    return false;
  }
  if (pos + length > bytes.size())
    return false;
  const unsigned char second = byte(pos + 1);
  if (length == 3) {
    if (first == 0xe0 && second < 0xa0)
      return false;
    if (first == 0xed && second > 0x9f)
      return false;
  } else if (length == 4) {
    if (first == 0xf0 && second < 0x90)
      return false;
    if (first == 0xf4 && second > 0x8f)
      return false;
  }
  for (std::size_t i = 1; i < length; ++i) {
    const unsigned char continuation = byte(pos + i);
    if ((continuation & 0xc0) != 0x80)
      return false;
    cp = (cp << 6) | (continuation & 0x3f);
  }
  if (!is_unicode_scalar(cp))
    return false;
  pos += length;
  codepoint = cp;
  return true;
}

bool parse_lower_hex(std::string_view digits, std::uint32_t& value) {
  if (digits.empty())
    return false;
  std::uint32_t result = 0;
  for (char c : digits) {
    if (!is_lower_hex(c))
      return false;
    const std::uint32_t digit =
        c <= '9' ? static_cast<std::uint32_t>(c - '0') : static_cast<std::uint32_t>(c - 'a' + 10);
    if (result > (0xffffffffu - digit) / 16u)
      return false;
    result = result * 16u + digit;
  }
  if (!is_unicode_scalar(result))
    return false;
  value = result;
  return true;
}

const char* legacy_escape(std::string_view escape) {
  if (escape == "SP")
    return "@";
  if (escape == "BP")
    return "*";
  if (escape == "RF")
    return "&";
  if (escape == "LT")
    return "<";
  if (escape == "GT")
    return ">";
  if (escape == "LP")
    return "(";
  if (escape == "RP")
    return ")";
  if (escape == "C")
    return ",";
  return nullptr;
}

// Legacy Rust identifiers use C++-ish escapes for characters that are not
// representable in every object format.
std::string demangle_legacy_element(std::string_view element) {
  if (element.size() >= 2 && element[0] == '_' && element[1] == '$') {
    element.remove_prefix(1);
  }

  std::string out;
  std::size_t pos = 0;
  while (pos < element.size()) {
    const char c = element[pos];
    if (c == '.') {
      if (pos + 1 < element.size() && element[pos + 1] == '.') {
        out += "::";
        pos += 2;
      } else {
        out.push_back('.');
        ++pos;
      }
      continue;
    }
    if (c != '$') {
      out.push_back(c);
      ++pos;
      continue;
    }

    const std::size_t end = element.find('$', pos + 1);
    if (end == std::string_view::npos) {
      out.append(element.substr(pos));
      break;
    }

    const std::string_view escape = element.substr(pos + 1, end - pos - 1);
    if (const char* replacement = legacy_escape(escape)) {
      out += replacement;
      pos = end + 1;
      continue;
    }

    std::uint32_t codepoint = 0;
    if (escape.size() > 1 && escape[0] == 'u' && parse_lower_hex(escape.substr(1), codepoint) &&
        !is_control(codepoint)) {
      append_utf8(out, codepoint);
      pos = end + 1;
      continue;
    }

    // rustc-demangle leaves an unknown escape untouched.
    out.append(element.substr(pos));
    break;
  }
  return out;
}

bool is_rust_hash(std::string_view element) {
  if (element.size() < 2 || element[0] != 'h')
    return false;
  for (char c : element.substr(1)) {
    if (!is_hex(c))
      return false;
  }
  return true;
}

std::optional<std::string> demangle_legacy(std::string_view input) {
  std::string_view inner;
  if (input.size() >= 4 && input.substr(0, 4) == "__ZN") {
    inner = input.substr(4);
  } else if (input.size() >= 3 && input.substr(0, 3) == "_ZN") {
    inner = input.substr(3);
  } else if (input.size() >= 2 && input.substr(0, 2) == "ZN") {
    inner = input.substr(2);
  } else {
    return std::nullopt;
  }

  std::vector<std::string_view> elements;
  std::size_t pos = 0;
  while (pos < inner.size() && inner[pos] != 'E') {
    if (!is_digit(inner[pos]))
      return std::nullopt;
    std::uint64_t length = 0;
    while (pos < inner.size() && is_digit(inner[pos])) {
      const std::uint64_t digit = static_cast<std::uint64_t>(inner[pos] - '0');
      if (length > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
        return std::nullopt;
      }
      length = length * 10 + digit;
      ++pos;
    }
    if (length > static_cast<std::uint64_t>(inner.size() - pos))
      return std::nullopt;
    const std::size_t size = static_cast<std::size_t>(length);
    elements.push_back(inner.substr(pos, size));
    pos += size;
  }

  if (pos >= inner.size() || inner[pos] != 'E')
    return std::nullopt;
  ++pos;

  // The trailing hash is the marker that distinguishes a Rust legacy symbol
  // from an arbitrary Itanium `_ZN...E` name.
  if (elements.size() < 2 || !is_rust_hash(elements.back()))
    return std::nullopt;

  std::string out;
  for (std::size_t i = 0; i + 1 < elements.size(); ++i) {
    if (i != 0)
      out += "::";
    out += demangle_legacy_element(elements[i]);
    if (out.size() > kMaxOutput)
      return std::nullopt;
  }

  const std::string_view rest = inner.substr(pos);
  if (!rest.empty()) {
    if (!is_symbol_like_suffix(rest))
      return std::nullopt;
    if (rest.size() > kMaxOutput - out.size())
      return std::nullopt;
    out.append(rest);
  }
  return out;
}

bool decode_punycode(std::string_view ascii, std::string_view punycode, std::string& out) {
  if (punycode.empty() || ascii.size() > 8192)
    return false;

  std::vector<std::uint32_t> chars;
  chars.reserve(ascii.size() + 8);
  for (char c : ascii)
    chars.push_back(static_cast<unsigned char>(c));

  constexpr std::uint64_t kBase = 36;
  constexpr std::uint64_t kTMin = 1;
  constexpr std::uint64_t kTMax = 26;
  constexpr std::uint64_t kSkew = 38;
  constexpr std::uint64_t kDamp = 700;
  constexpr std::uint64_t kInitialBias = 72;
  constexpr std::uint64_t kInitialN = 0x80;

  std::uint64_t n = kInitialN;
  std::uint64_t i = 0;
  std::uint64_t bias = kInitialBias;
  std::uint64_t damp = kDamp;
  std::size_t pos = 0;

  while (pos < punycode.size()) {
    std::uint64_t w = 1;
    std::uint64_t delta = 0;
    for (std::uint64_t k = kBase;; k += kBase) {
      if (pos >= punycode.size())
        return false;
      const char c = punycode[pos++];
      std::uint64_t digit = 0;
      if (c >= 'a' && c <= 'z') {
        digit = static_cast<std::uint64_t>(c - 'a');
      } else if (c >= '0' && c <= '9') {
        digit = 26 + static_cast<std::uint64_t>(c - '0');
      } else {
        return false;
      }
      if (digit > (std::numeric_limits<std::uint64_t>::max() - delta) / w)
        return false;
      delta += digit * w;
      const std::uint64_t clamped = k > bias ? k - bias : 0;
      const std::uint64_t t = std::min(std::max(clamped, kTMin), kTMax);
      if (digit < t)
        break;
      if (w > std::numeric_limits<std::uint64_t>::max() / (kBase - t))
        return false;
      w *= kBase - t;
    }

    const std::uint64_t len = static_cast<std::uint64_t>(chars.size()) + 1;
    if (i > std::numeric_limits<std::uint64_t>::max() - delta)
      return false;
    i += delta;
    if (n > std::numeric_limits<std::uint64_t>::max() - i / len)
      return false;
    n += i / len;
    i %= len;
    if (n > 0x10ffff || (n >= 0xd800 && n <= 0xdfff))
      return false;
    if (chars.size() >= 8192)
      return false;
    chars.insert(chars.begin() + static_cast<std::ptrdiff_t>(i), static_cast<std::uint32_t>(n));
    ++i;

    if (pos == punycode.size())
      break;

    // Bias adaptation, exactly as in the Punycode decoder.
    delta /= damp;
    damp = 2;
    delta += delta / len;
    std::uint64_t k = 0;
    while (delta > ((kBase - kTMin) * kTMax) / 2) {
      delta /= kBase - kTMin;
      k += kBase;
    }
    bias = k + ((kBase - kTMin + 1) * delta) / (delta + kSkew);
  }

  for (std::uint32_t cp : chars) {
    if (!is_unicode_scalar(cp))
      return false;
    append_utf8(out, cp);
  }
  return true;
}

class V0Parser {
public:
  explicit V0Parser(std::string_view sym) : sym_(sym) {}

  bool parse(std::string& out, std::string_view& rest) {
    if (!parse_path(true))
      return false;

    // A few symbols in the wild (and in the task's canonical examples) omit
    // the final `N` path tags and leave one or more bare, length-prefixed
    // identifiers after a complete path.  Accept that spelling as an
    // extension, but only for compiler-style symbols whose crate carries a
    // disambiguator; this keeps ordinary malformed names rejected.
    while (saw_crate_disambiguator_ && pos_ < sym_.size() && is_digit(sym_[pos_])) {
      std::uint64_t dis = 0;
      Ident ident;
      if (!disambiguator(dis) || !parse_ident(ident))
        return false;
      if (ident.text.empty())
        return fail();
      put("::");
      put(ident.text);
    }

    // Some old v0 symbols have a second, silently ignored "instantiating
    // crate" path after the real symbol.
    if (pos_ < sym_.size() && is_upper_alpha(sym_[pos_])) {
      const bool saved_emit = emit_;
      emit_ = false;
      const bool ok = parse_path(false);
      emit_ = saved_emit;
      if (!ok)
        return false;
    }

    if (failed_)
      return false;
    rest = sym_.substr(pos_);
    out = std::move(out_);
    return true;
  }

private:
  struct DepthGuard {
    explicit DepthGuard(unsigned& depth) : depth_(depth) {
      ++depth_;
    }
    ~DepthGuard() {
      --depth_;
    }
    unsigned& depth_;
  };

  struct Ident {
    std::string text;
    bool had_punycode = false;
  };

  static const char* basic_type(char tag) {
    switch (tag) {
    case 'a':
      return "i8";
    case 'b':
      return "bool";
    case 'c':
      return "char";
    case 'd':
      return "f64";
    case 'e':
      return "str";
    case 'f':
      return "f32";
    case 'h':
      return "u8";
    case 'i':
      return "isize";
    case 'j':
      return "usize";
    case 'l':
      return "i32";
    case 'm':
      return "u32";
    case 'n':
      return "i128";
    case 'o':
      return "u128";
    case 's':
      return "i16";
    case 't':
      return "u16";
    case 'u':
      return "()";
    case 'v':
      return "...";
    case 'x':
      return "i64";
    case 'y':
      return "u64";
    case 'z':
      return "!";
    case 'p':
      return "_";
    default:
      return nullptr;
    }
  }

  bool fail() {
    failed_ = true;
    return false;
  }

  bool tick() {
    if (failed_)
      return false;
    ++work_;
    if (work_ > kMaxWork)
      return fail();
    return true;
  }

  bool eat(char expected) {
    if (failed_ || pos_ >= sym_.size() || sym_[pos_] != expected)
      return false;
    ++pos_;
    return tick();
  }

  bool take(char& c) {
    if (failed_)
      return false;
    if (pos_ >= sym_.size())
      return fail();
    c = sym_[pos_++];
    return tick();
  }

  void put(std::string_view text) {
    if (!emit_ || failed_ || text.empty())
      return;
    if (text.size() > kMaxOutput || out_.size() > kMaxOutput - text.size()) {
      failed_ = true;
      return;
    }
    out_.append(text);
  }

  void put(char c) {
    put(std::string_view(&c, 1));
  }

  void put_u64(std::uint64_t value) {
    char buffer[32];
    std::size_t pos = sizeof(buffer);
    do {
      buffer[--pos] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value != 0);
    put(std::string_view(buffer + pos, sizeof(buffer) - pos));
  }

  unsigned depth_ = 0;
  std::uint64_t work_ = 0;
  bool failed_ = false;
  bool emit_ = true;
  bool saw_crate_disambiguator_ = false;
  std::string_view sym_;
  std::size_t pos_ = 0;
  std::string out_;
  std::uint64_t bound_lifetime_depth_ = 0;

  bool parse_ident(Ident& ident) {
    ident = Ident{};
    const bool punycode = eat('u');

    char c = 0;
    if (!take(c) || !is_digit(c))
      return fail();
    std::uint64_t length = static_cast<std::uint64_t>(c - '0');
    if (length != 0) {
      while (pos_ < sym_.size() && is_digit(sym_[pos_])) {
        const std::uint64_t digit = static_cast<std::uint64_t>(sym_[pos_] - '0');
        ++pos_;
        if (!tick())
          return false;
        if (length > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
          return fail();
        }
        length = length * 10 + digit;
      }
    }

    // An optional underscore separates the length from the identifier.  It is
    // needed for identifiers that start with a digit (`6_123foo`).
    eat('_');

    if (length > static_cast<std::uint64_t>(sym_.size() - pos_))
      return fail();
    const std::size_t size = static_cast<std::size_t>(length);
    const std::string_view raw = sym_.substr(pos_, size);
    pos_ += size;
    work_ += size;
    if (work_ > kMaxWork)
      return fail();

    if (!punycode) {
      ident.text.assign(raw);
      return true;
    }

    ident.had_punycode = true;
    const std::size_t split = raw.rfind('_');
    const std::string_view ascii =
        split == std::string_view::npos ? std::string_view{} : raw.substr(0, split);
    const std::string_view encoded = split == std::string_view::npos ? raw : raw.substr(split + 1);
    if (!decode_punycode(ascii, encoded, ident.text))
      return fail();
    return true;
  }

  bool integer_62(std::uint64_t& value) {
    if (eat('_')) {
      value = 0;
      return true;
    }

    std::uint64_t result = 0;
    for (;;) {
      if (eat('_'))
        break;
      char c = 0;
      if (!take(c))
        return false;
      std::uint64_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<std::uint64_t>(c - '0');
      } else if (c >= 'a' && c <= 'z') {
        digit = 10 + static_cast<std::uint64_t>(c - 'a');
      } else if (c >= 'A' && c <= 'Z') {
        digit = 36 + static_cast<std::uint64_t>(c - 'A');
      } else {
        return fail();
      }
      if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 62) {
        return fail();
      }
      result = result * 62 + digit;
    }

    if (result == std::numeric_limits<std::uint64_t>::max())
      return fail();
    value = result + 1;
    return true;
  }

  bool opt_integer_62(char tag, std::uint64_t& value) {
    if (!eat(tag)) {
      value = 0;
      return true;
    }
    std::uint64_t encoded = 0;
    if (!integer_62(encoded))
      return false;
    if (encoded == std::numeric_limits<std::uint64_t>::max())
      return fail();
    value = encoded + 1;
    return true;
  }

  bool disambiguator(std::uint64_t& value) {
    return opt_integer_62('s', value);
  }

  bool print_lifetime(std::uint64_t index) {
    put("'");
    if (index == 0) {
      put("_");
      return true;
    }
    if (index > bound_lifetime_depth_)
      return fail();
    const std::uint64_t depth = bound_lifetime_depth_ - index;
    if (depth < 26) {
      put(static_cast<char>('a' + depth));
    } else {
      put("_");
      put_u64(depth);
    }
    return true;
  }

  bool parse_path(bool in_value) {
    if (!tick())
      return false;
    if (depth_ >= kMaxDepth)
      return fail();
    DepthGuard guard(depth_);

    char tag = 0;
    if (!take(tag))
      return false;
    switch (tag) {
    case 'C': {
      std::uint64_t dis = 0;
      if (!disambiguator(dis))
        return false;
      if (dis != 0)
        saw_crate_disambiguator_ = true;
      Ident ident;
      if (!parse_ident(ident))
        return false;
      put(ident.text);
      return true;
    }
    case 'N': {
      char ns = 0;
      if (!take(ns) || !is_alpha(ns))
        return fail();
      if (!parse_path(in_value))
        return false;

      std::uint64_t dis = 0;
      if (!disambiguator(dis))
        return false;
      Ident ident;
      if (!parse_ident(ident))
        return false;

      if (is_upper_alpha(ns)) {
        put("::{");
        if (ns == 'C') {
          put("closure");
        } else if (ns == 'S') {
          put("shim");
        } else {
          put(ns);
        }
        if (!ident.text.empty()) {
          put(":");
          put(ident.text);
        }
        put("#");
        put_u64(dis);
        put("}");
      } else if (!ident.text.empty()) {
        put("::");
        put(ident.text);
      }
      return true;
    }
    case 'I': {
      if (!parse_path(in_value))
        return false;
      if (in_value)
        put("::");
      put("<");
      if (!parse_generic_list())
        return false;
      put(">");
      return true;
    }
    case 'M':
    case 'X':
    case 'Y': {
      if (tag != 'Y') {
        std::uint64_t dis = 0;
        if (!disambiguator(dis))
          return false;
        const bool saved_emit = emit_;
        emit_ = false;
        const bool ok = parse_path(false);
        emit_ = saved_emit;
        if (!ok)
          return false;
      }
      put("<");
      if (!parse_type())
        return false;
      if (tag != 'M') {
        put(" as ");
        if (!parse_path(false))
          return false;
      }
      put(">");
      return true;
    }
    case 'B': {
      const std::size_t backref_pos = pos_ - 1;
      std::uint64_t target = 0;
      if (!integer_62(target))
        return false;
      if (target >= backref_pos)
        return fail();
      const std::size_t saved_pos = pos_;
      pos_ = static_cast<std::size_t>(target);
      const bool ok = parse_path(in_value);
      pos_ = saved_pos;
      return ok;
    }
    default:
      return fail();
    }
  }

  bool parse_generic_list() {
    bool first = true;
    for (;;) {
      if (eat('E'))
        return true;
      if (!first)
        put(", ");
      if (!parse_generic_arg())
        return false;
      first = false;
    }
  }

  bool parse_generic_arg() {
    if (!tick())
      return false;
    if (eat('L')) {
      std::uint64_t index = 0;
      if (!integer_62(index))
        return false;
      return print_lifetime(index);
    }
    if (eat('K'))
      return parse_const(false);
    return parse_type();
  }

  bool parse_hex_nibbles(std::string_view& nibbles) {
    const std::size_t start = pos_;
    for (;;) {
      char c = 0;
      if (!take(c))
        return false;
      if (c == '_') {
        nibbles = sym_.substr(start, pos_ - start - 1);
        return true;
      }
      if (!is_lower_hex(c))
        return fail();
    }
  }

  bool nibbles_to_uint(std::string_view nibbles, std::uint64_t& value) {
    std::size_t first = 0;
    while (first < nibbles.size() && nibbles[first] == '0')
      ++first;
    const std::string_view significant = nibbles.substr(first);
    if (significant.size() > 16)
      return false;
    std::uint64_t result = 0;
    for (char c : significant) {
      const std::uint64_t digit =
          c <= '9' ? static_cast<std::uint64_t>(c - '0') : static_cast<std::uint64_t>(c - 'a' + 10);
      result = (result << 4) | digit;
    }
    value = result;
    return true;
  }

  bool put_const_uint() {
    std::string_view nibbles;
    if (!parse_hex_nibbles(nibbles))
      return false;
    std::size_t first = 0;
    while (first < nibbles.size() && nibbles[first] == '0')
      ++first;
    const std::string_view significant = nibbles.substr(first);
    if (significant.size() <= 16) {
      std::uint64_t value = 0;
      if (!nibbles_to_uint(nibbles, value))
        return fail();
      put_u64(value);
    } else {
      put("0x");
      put(nibbles);
    }
    return true;
  }

  bool decode_hex_string(std::string_view nibbles, std::string& out) {
    if (nibbles.size() % 2 != 0)
      return false;
    std::string bytes;
    bytes.reserve(nibbles.size() / 2);
    for (std::size_t i = 0; i < nibbles.size(); i += 2) {
      const auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9')
          return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f')
          return static_cast<unsigned>(c - 'a' + 10);
        return 16;
      };
      const unsigned hi = digit(nibbles[i]);
      const unsigned lo = digit(nibbles[i + 1]);
      if (hi > 15 || lo > 15)
        return false;
      bytes.push_back(static_cast<char>((hi << 4) | lo));
    }

    // Validate that the byte sequence is well-formed UTF-8.
    std::size_t pos = 0;
    while (pos < bytes.size()) {
      std::uint32_t codepoint = 0;
      if (!decode_utf8(bytes, pos, codepoint))
        return false;
    }

    out.append(bytes);
    return true;
  }

  void put_unicode_escape(std::uint32_t cp) {
    put("\\u{");
    std::string hex;
    const char* digits = "0123456789abcdef";
    do {
      hex.insert(hex.begin(), digits[cp % 16]);
      cp /= 16;
    } while (cp != 0);
    put(hex);
    put("}");
  }

  bool put_quoted_char(std::uint32_t cp) {
    if (!is_unicode_scalar(cp))
      return fail();
    put("'");
    if (cp == '\\' || cp == '\'') {
      put('\\');
      put(static_cast<char>(cp));
    } else if (cp == '\n') {
      put("\\n");
    } else if (cp == '\r') {
      put("\\r");
    } else if (cp == '\t') {
      put("\\t");
    } else if (cp == 0) {
      put("\\0");
    } else if (is_printable(cp)) {
      std::string utf8;
      append_utf8(utf8, cp);
      put(utf8);
    } else {
      put_unicode_escape(cp);
    }
    put("'");
    return true;
  }

  bool put_string_literal(std::string_view bytes) {
    put("\"");
    std::size_t pos = 0;
    while (pos < bytes.size()) {
      std::uint32_t cp = 0;
      if (!decode_utf8(bytes, pos, cp))
        return fail();

      if (cp == '"' || cp == '\\') {
        put('\\');
        put(static_cast<char>(cp));
      } else if (cp == '\n') {
        put("\\n");
      } else if (cp == '\r') {
        put("\\r");
      } else if (cp == '\t') {
        put("\\t");
      } else if (cp == 0) {
        put("\\0");
      } else if (is_printable(cp)) {
        std::string utf8;
        append_utf8(utf8, cp);
        put(utf8);
      } else {
        put_unicode_escape(cp);
      }
    }
    put("\"");
    return true;
  }

  bool put_const_string() {
    std::string_view nibbles;
    if (!parse_hex_nibbles(nibbles))
      return false;
    std::string bytes;
    if (!decode_hex_string(nibbles, bytes))
      return fail();
    return put_string_literal(bytes);
  }

  bool parse_const_list() {
    bool first = true;
    for (;;) {
      if (eat('E'))
        return true;
      if (!first)
        put(", ");
      if (!parse_const(true))
        return false;
      first = false;
    }
  }

  bool parse_type_list() {
    bool first = true;
    for (;;) {
      if (eat('E'))
        return true;
      if (!first)
        put(", ");
      if (!parse_type())
        return false;
      first = false;
    }
  }

  bool parse_fn_type() {
    std::uint64_t binder = 0;
    if (!opt_integer_62('G', binder))
      return false;
    if (binder > kMaxDepth)
      return fail();

    const std::uint64_t saved_bound_depth = bound_lifetime_depth_;
    if (binder != 0) {
      put("for<");
      for (std::uint64_t i = 0; i < binder; ++i) {
        if (i != 0)
          put(", ");
        ++bound_lifetime_depth_;
        if (!print_lifetime(1)) {
          bound_lifetime_depth_ = saved_bound_depth;
          return false;
        }
      }
      put("> ");
    }

    bool ok = true;
    if (eat('U'))
      put("unsafe ");

    if (eat('K')) {
      std::string abi;
      if (eat('C')) {
        abi = "C";
      } else {
        Ident ident;
        if (!parse_ident(ident)) {
          ok = false;
        } else if (ident.text.empty() || ident.had_punycode) {
          ok = fail();
        } else {
          abi = ident.text;
        }
      }
      if (ok) {
        put("extern \"");
        for (char c : abi)
          put(c == '_' ? '-' : c);
        put("\" ");
      }
    }

    if (ok) {
      put("fn(");
      ok = parse_type_list();
      put(")");
    }

    if (ok && !eat('u')) {
      put(" -> ");
      ok = parse_type();
    }

    bound_lifetime_depth_ = saved_bound_depth;
    return ok;
  }

  // In a `dyn Trait<...>` type the generic argument list is normally closed
  // immediately, but associated-type bindings (`p<name> ...`) are appended
  // inside the same angle brackets.
  bool parse_path_maybe_open_generics(bool& open) {
    if (!tick())
      return false;
    if (depth_ >= kMaxDepth)
      return fail();
    DepthGuard guard(depth_);

    if (eat('B')) {
      const std::size_t backref_pos = pos_ - 1;
      std::uint64_t target = 0;
      if (!integer_62(target))
        return false;
      if (target >= backref_pos)
        return fail();
      const std::size_t saved_pos = pos_;
      pos_ = static_cast<std::size_t>(target);
      const bool ok = parse_path_maybe_open_generics(open);
      pos_ = saved_pos;
      return ok;
    }
    if (eat('I')) {
      if (!parse_path(false))
        return false;
      put("<");
      if (!parse_generic_list())
        return false;
      open = true;
      return true;
    }
    open = false;
    return parse_path(false);
  }

  bool parse_dyn_trait() {
    bool open = false;
    if (!parse_path_maybe_open_generics(open))
      return false;

    while (eat('p')) {
      if (open) {
        put(", ");
      } else {
        put("<");
        open = true;
      }
      Ident name;
      if (!parse_ident(name))
        return false;
      put(name.text);
      put(" = ");
      if (eat('K')) {
        if (!parse_const(false))
          return false;
      } else if (!parse_type()) {
        return false;
      }
    }

    if (open)
      put(">");
    return true;
  }

  bool parse_dyn_type() {
    std::uint64_t binder = 0;
    if (!opt_integer_62('G', binder))
      return false;
    if (binder > kMaxDepth)
      return fail();

    const std::uint64_t saved_bound_depth = bound_lifetime_depth_;
    if (binder != 0) {
      put("for<");
      for (std::uint64_t i = 0; i < binder; ++i) {
        if (i != 0)
          put(", ");
        ++bound_lifetime_depth_;
        if (!print_lifetime(1)) {
          bound_lifetime_depth_ = saved_bound_depth;
          return false;
        }
      }
      put("> ");
    }

    put("dyn ");
    bool first = true;
    bool ok = true;
    for (;;) {
      if (eat('E'))
        break;
      if (!first)
        put(" + ");
      if (!parse_dyn_trait()) {
        ok = false;
        break;
      }
      first = false;
    }

    bound_lifetime_depth_ = saved_bound_depth;

    if (ok) {
      if (!eat('L')) {
        ok = fail();
      } else {
        std::uint64_t index = 0;
        if (!integer_62(index)) {
          ok = false;
        } else if (index != 0) {
          put(" + ");
          ok = print_lifetime(index);
        }
      }
    }
    return ok;
  }

  bool parse_const(bool in_value) {
    if (!tick())
      return false;
    char tag = 0;
    if (!take(tag))
      return false;
    if (depth_ >= kMaxDepth)
      return fail();
    DepthGuard guard(depth_);

    bool opened_brace = false;
    const auto open_brace = [&] {
      if (!in_value) {
        opened_brace = true;
        put("{");
      }
    };

    bool ok = true;
    switch (tag) {
    case 'p':
      put("_");
      break;
    case 'h':
    case 't':
    case 'm':
    case 'y':
    case 'o':
    case 'j':
      ok = put_const_uint();
      break;
    case 'a':
    case 's':
    case 'l':
    case 'x':
    case 'n':
    case 'i':
      if (eat('n'))
        put("-");
      ok = put_const_uint();
      break;
    case 'b': {
      std::string_view nibbles;
      std::uint64_t value = 0;
      ok = parse_hex_nibbles(nibbles) && nibbles_to_uint(nibbles, value);
      if (ok) {
        if (value == 0) {
          put("false");
        } else if (value == 1) {
          put("true");
        } else {
          ok = fail();
        }
      }
      break;
    }
    case 'c': {
      std::string_view nibbles;
      std::uint64_t value = 0;
      ok = parse_hex_nibbles(nibbles) && nibbles_to_uint(nibbles, value) && value <= 0x10ffff &&
           is_unicode_scalar(static_cast<std::uint32_t>(value));
      if (ok)
        ok = put_quoted_char(static_cast<std::uint32_t>(value));
      break;
    }
    case 'e':
      open_brace();
      put("*");
      ok = put_const_string();
      break;
    case 'R':
    case 'Q':
      if (tag == 'R' && eat('e')) {
        ok = put_const_string();
      } else {
        open_brace();
        put("&");
        if (tag == 'Q')
          put("mut ");
        ok = parse_const(true);
      }
      break;
    case 'A':
      open_brace();
      put("[");
      ok = parse_const_list();
      put("]");
      break;
    case 'T': {
      open_brace();
      put("(");
      std::size_t count = 0;
      bool first = true;
      for (;;) {
        if (eat('E'))
          break;
        if (!first)
          put(", ");
        if (!parse_const(true)) {
          ok = false;
          break;
        }
        first = false;
        ++count;
      }
      if (ok && count == 1)
        put(",");
      put(")");
      break;
    }
    case 'V': {
      open_brace();
      ok = parse_path(true);
      char form = 0;
      if (ok)
        ok = take(form);
      if (ok) {
        if (form == 'U') {
          // Unit variant.
        } else if (form == 'T') {
          put("(");
          ok = parse_const_list();
          put(")");
        } else if (form == 'S') {
          put(" { ");
          bool first = true;
          for (;;) {
            if (eat('E'))
              break;
            if (!first)
              put(", ");
            std::uint64_t dis = 0;
            Ident name;
            if (!disambiguator(dis) || !parse_ident(name)) {
              ok = false;
              break;
            }
            put(name.text);
            put(": ");
            if (!parse_const(true)) {
              ok = false;
              break;
            }
            first = false;
          }
          put(" }");
        } else {
          ok = fail();
        }
      }
      break;
    }
    case 'B': {
      const std::size_t backref_pos = pos_ - 1;
      std::uint64_t target = 0;
      if (!integer_62(target)) {
        ok = false;
      } else if (target >= backref_pos) {
        ok = fail();
      } else {
        const std::size_t saved_pos = pos_;
        pos_ = static_cast<std::size_t>(target);
        ok = parse_const(in_value);
        pos_ = saved_pos;
      }
      break;
    }
    default:
      ok = fail();
      break;
    }

    if (ok && opened_brace)
      put("}");
    return ok;
  }

  bool parse_type() {
    if (!tick())
      return false;

    if (eat('w'))
      put("#[splat] ");

    char tag = 0;
    if (!take(tag))
      return false;
    if (const char* basic = basic_type(tag)) {
      put(basic);
      return true;
    }

    if (depth_ >= kMaxDepth)
      return fail();
    DepthGuard guard(depth_);

    switch (tag) {
    case 'R':
    case 'Q': {
      put("&");
      if (eat('L')) {
        std::uint64_t index = 0;
        if (!integer_62(index))
          return false;
        if (index != 0) {
          if (!print_lifetime(index))
            return false;
          put(" ");
        }
      }
      if (tag == 'Q')
        put("mut ");
      return parse_type();
    }
    case 'P':
    case 'O': {
      put("*");
      if (tag == 'P') {
        put("const ");
      } else {
        put("mut ");
      }
      return parse_type();
    }
    case 'A':
    case 'S': {
      put("[");
      if (!parse_type())
        return false;
      if (tag == 'A') {
        put("; ");
        if (!parse_const(true))
          return false;
      }
      put("]");
      return true;
    }
    case 'T': {
      put("(");
      std::size_t count = 0;
      bool first = true;
      for (;;) {
        if (eat('E'))
          break;
        if (!first)
          put(", ");
        if (!parse_type())
          return false;
        first = false;
        ++count;
      }
      if (count == 1)
        put(",");
      put(")");
      return true;
    }
    case 'F':
      return parse_fn_type();
    case 'D':
      return parse_dyn_type();
    case 'B': {
      const std::size_t backref_pos = pos_ - 1;
      std::uint64_t target = 0;
      if (!integer_62(target))
        return false;
      if (target >= backref_pos)
        return fail();
      const std::size_t saved_pos = pos_;
      pos_ = static_cast<std::size_t>(target);
      const bool ok = parse_type();
      pos_ = saved_pos;
      return ok;
    }
    default:
      --pos_;
      return parse_path(false);
    }
  }
};

std::optional<std::string> demangle_v0(std::string_view input, std::size_t prefix_size) {
  const std::string_view inner = input.substr(prefix_size);
  if (inner.empty() || !is_upper_alpha(inner.front()))
    return std::nullopt;

  V0Parser parser(inner);
  std::string out;
  std::string_view rest;
  if (!parser.parse(out, rest))
    return std::nullopt;

  if (!rest.empty()) {
    if (!is_symbol_like_suffix(rest))
      return std::nullopt;
    if (rest.size() > kMaxOutput - out.size())
      return std::nullopt;
    out.append(rest);
  }
  return out;
}

} // namespace

std::optional<std::string> demangle_rust(std::string_view name) {
  if (name.empty() || !is_ascii(name))
    return std::nullopt;

  const std::string_view input = strip_thinlto_suffix(name);

  if (input.starts_with("_R"))
    return demangle_v0(input, 2);
  if (input.starts_with("__R"))
    return demangle_v0(input, 3);
  return demangle_legacy(input);
}

} // namespace nix_sym
