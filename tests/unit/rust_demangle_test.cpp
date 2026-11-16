#include "../test.hpp"

#include "demangle.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

int vector_count = 0;

void expect_demangled(std::string_view mangled, std::string_view expected) {
  ++vector_count;
  const std::optional<std::string> actual = nix_sym::demangle_rust(mangled);
  if (actual && *actual == expected) {
    std::cout << "PASS  " << mangled << "  ->  " << *actual << "\n";
    return;
  }

  std::cout << "FAIL  " << mangled << "  ->  " << (actual ? *actual : std::string("<nullopt>"))
            << "  (expected " << expected << ")\n";
  nix_sym_test::expect(actual.has_value(), "demangle_rust(...).has_value()", __FILE__, __LINE__);
  if (actual) {
    nix_sym_test::expect_eq(*actual, std::string(expected), "demangle_rust(...)", __FILE__,
                            __LINE__);
  }
}

void expect_invalid(std::string_view mangled) {
  ++vector_count;
  const std::optional<std::string> actual = nix_sym::demangle_rust(mangled);
  if (!actual) {
    std::cout << "PASS  " << mangled << "  ->  <nullopt>\n";
    return;
  }

  std::cout << "FAIL  " << mangled << "  ->  " << *actual << "  (expected <nullopt>)\n";
  nix_sym_test::expect(!actual.has_value(), "!demangle_rust(...).has_value()", __FILE__, __LINE__);
}

} // namespace

int main() {
  std::cout << "rust_demangle_test\n";

  // Canonical examples from the task.
  expect_demangled("_RNvNtCs1234_7mycrate3foo3bar", "mycrate::foo::bar");
  expect_demangled("_RINvNtC3std3mem8align_ofjE", "std::mem::align_of::<usize>");
  expect_demangled("_RNvNtNtCs8meDrTSUyFO_3std3sys3pal4unix14abort_internal",
                   "std::sys::pal::unix::abort_internal");
  expect_demangled("_RNvNtNtNtCs8meDrTSUyFO_3std3sys3pal4unix14abort_internal",
                   "std::sys::pal::unix::abort_internal");

  // v0 paths, nested paths, namespaces, and the double-underscore prefix.
  expect_demangled("_RNvC3foo3bar", "foo::bar");
  expect_demangled("_RNsC3foo3bar", "foo::bar");
  expect_demangled("__RNvC3foo3bar", "foo::bar");
  expect_demangled("_RC3foo", "foo");
  expect_demangled("_RC4f128", "f128");
  expect_demangled("_RNvNtC3foo3bar3baz", "foo::bar::baz");
  expect_demangled("_RNCC3foo3bar", "foo::{closure:bar#0}");
  expect_demangled("_RNCNCNgCs6DXkGYLi8lr_2cc5spawn00B5_", "cc::spawn::{closure#0}::{closure#0}");

  // v0 generic arguments, including nested types and backreferences.
  expect_demangled("_RINtC3foo3BarjE", "foo::Bar::<usize>");
  expect_demangled("_RINvC3foo3barINtC3foo3BazjEE", "foo::bar::<foo::Baz<usize>>");
  expect_demangled("_RIC0TTuB4_EEE", "::<(((), ()),)>");
  expect_demangled("_RINbNbCskIICzLVDPPb_5alloc5alloc8box_freeDINbNiB4_5boxed5FnBoxuEp6OutputuEL_"
                   "ECs1iopQbuBiw2_3std",
                   "alloc::alloc::box_free::<dyn alloc::boxed::FnBox<(), Output = ()>>");

  // v0 basic types and simple const data.
  expect_demangled("_RIC0jE", "::<usize>");
  expect_demangled("_RIC0hE", "::<u8>");
  expect_demangled("_RIC0bE", "::<bool>");
  expect_demangled("_RIC0uE", "::<()>");
  expect_demangled("_RIC0fE", "::<f32>");
  expect_demangled("_RIC0dE", "::<f64>");
  expect_demangled("_RIC0eE", "::<str>");
  expect_demangled("_RIC0pE", "::<_>");
  expect_demangled("_RIC0RjE", "::<&usize>");
  expect_demangled("_RIC0PjE", "::<*const usize>");
  expect_demangled("_RIC0ShE", "::<[u8]>");
  expect_demangled("_RIC0Ahj7_E", "::<[u8; 7]>");
  expect_demangled("_RIC0ThbEE", "::<(u8, bool)>");
  expect_demangled("_RIC0FhEuE", "::<fn(u8)>");
  expect_demangled("_RIC0L_E", "::<'_>");
  expect_demangled("_RIC0Kj7b_E", "::<123>");
  expect_demangled("_RIC0Kb1_E", "::<true>");
  expect_demangled("_RIC0Kc76_E", "::<'v'>");
  expect_demangled("_RIC0Kc0_E", "::<'\\0'>");
  expect_demangled("_RIC0Kc2202_E", "::<'\u2202'>");
  expect_demangled("_RIC0Kca0_E", "::<'\\u{a0}'>");
  expect_demangled("_RIC0Kce282_E", "::<'\\u{e282}'>");
  expect_demangled("_RIC0Ke616263_E", "::<{*\"abc\"}>");
  expect_demangled("_RIC0Ke00_E", "::<{*\"\\0\"}>");
  expect_demangled("_RIC0Ke090a_E", "::<{*\"\\t\\n\"}>");
  expect_demangled("_RIC0KRe616263_E", "::<\"abc\">");
  expect_demangled("_RIC0KRh7b_E", "::<{&123}>");
  expect_demangled("_RIC0KQh1_E", "::<{&mut 1}>");
  expect_demangled("_RIC0KAh1_h2_h3_EE", "::<{[1, 2, 3]}>");
  expect_demangled("_RIC0KTh1_b0_EE", "::<{(1, false)}>");

  // v0 punycode identifiers.
  expect_demangled("_RNqCs4fqI2P2rA04_11utf8_identsu30____7hkackfecea1cbdathfdh9hlq6y",
                   "utf8_idents::\u10e1\u10d0\u10ed\u10db\u10d4\u10da\u10d0\u10d3_"
                   "\u10d2\u10d4\u10db\u10e0\u10d8\u10d4\u10da\u10d8_"
                   "\u10e1\u10d0\u10d3\u10d8\u10da\u10d8");

  // Vendor/ThinLTO suffixes.
  expect_demangled("_RC3foo.llvm.9D1C9369", "foo");
  expect_demangled("_RC3foo.llvm.9D1C9369@@16", "foo");
  expect_demangled("_RNvC9backtrace3foo.llvm.A5310EB9", "backtrace::foo");
  expect_demangled("_RNvC3foo3bar.0.0", "foo::bar.0.0");
  expect_demangled("_RC3foo.0.0", "foo.0.0");
  expect_demangled("_RIC0Kj7b_E.llvm.A1B2", "::<123>");
  expect_demangled("_RIC0Kj7b_E.0", "::<123>.0");
  expect_demangled("_ZN3foo17h0123456789abcdefE.llvm.A5310EB9", "foo");
  expect_demangled("_ZN3foo17h0123456789abcdefE.exit.i.i", "foo.exit.i.i");

  // Legacy (Itanium-style) Rust symbols with a trailing hash.
  expect_demangled("_ZN4core3fmt9Formatter3pad17h0123456789abcdefE", "core::fmt::Formatter::pad");
  expect_demangled("_ZN3foo17h05af221e174051e9E", "foo");
  expect_demangled("__ZN5alloc9allocator6Layout9for_value17h02a996811f781011E",
                   "alloc::allocator::Layout::for_value");
  expect_demangled("_ZN35Bar$LT$$u5b$u32$u3b$$u20$4$u5d$$GT$17h0123456789abcdefE", "Bar<[u32; 4]>");
  expect_demangled("_ZN13_$LT$test$GT$17h0123456789abcdefE", "<test>");
  expect_demangled("_ZN28_$u7b$$u7b$closure$u7d$$u7d$17h0123456789abcdefE", "{{closure}}");
  expect_demangled("_ZN3foo5h05afE", "foo");
  expect_demangled(
      "_ZN11issue_609253foo37Foo$LT$issue_60925..llv$u6d$..Foo$GT$3foo17h059a991a004536adE",
      "issue_60925::foo::Foo<issue_60925::llvm::Foo>::foo");

  // Invalid and non-Rust names must be rejected, never partially formatted.
  expect_invalid("");
  expect_invalid("hello");
  expect_invalid("_Z3foov");
  expect_invalid("_ZN3foo3barE");
  expect_invalid("_ZN4testE");
  expect_invalid("_ZN17h0123456789abcdef3fooE");
  expect_invalid("_ZN3foo17hg5af221e174051e9E");
  expect_invalid("_ZN3fooE.llvm.A5310EB9");
  expect_invalid("_R");
  expect_invalid("_RN");
  expect_invalid("_RNvC3foo");
  expect_invalid("_RNvNtC3foo3bar");
  expect_invalid("_RNvC3foo3barE");
  expect_invalid("_RIC0j");
  expect_invalid("_RIC0Kj7b");
  expect_invalid("_RIC0B2_E");
  expect_invalid("_RC3foo3bar");
  expect_invalid("_RNvC3foo3bar3baz");
  expect_invalid("_RNvNtNtC3std3sys3pal4unix14abort_internal");
  expect_invalid("_RIC0Ke80_E");
  expect_invalid("_RIC0Kec080_E");
  expect_invalid("_RC3foo.0.0 ");
  expect_invalid("_RC3f\xc3\xb6");

  std::string too_deep = "_RIC0";
  too_deep.append(600, 'R');
  too_deep += 'p';
  too_deep += 'E';
  expect_invalid(too_deep);

  std::cout << "\n" << vector_count << " vectors, " << nix_sym_test::failures << " failure(s)\n";
  return nix_sym_test::failures == 0 ? 0 : 1;
}
