#include "demangle.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "../test.hpp"

namespace {

int vector_count = 0;

void fail_vector(std::string_view input, const std::string& message, const char* file, int line) {
  nix_sym_test::fail(file, line, "demangle_haskell(\"" + std::string(input) + "\"): " + message);
}

void check_demangled(std::string_view input, std::string_view expected, const char* file,
                     int line) {
  ++vector_count;
  const std::optional<std::string> actual = nix_sym::demangle_haskell(input);
  if (!actual) {
    fail_vector(input, "returned nullopt, expected \"" + std::string(expected) + "\"", file, line);
    return;
  }
  if (*actual != expected) {
    fail_vector(input, "got \"" + *actual + "\", expected \"" + std::string(expected) + "\"", file,
                line);
  }
}

void check_rejected(std::string_view input, const char* file, int line) {
  ++vector_count;
  const std::optional<std::string> actual = nix_sym::demangle_haskell(input);
  if (actual) {
    fail_vector(input, "expected nullopt, got \"" + *actual + "\"", file, line);
  }
}

} // namespace

#define CHECK_DEMANGLED(input, expected) check_demangled((input), (expected), __FILE__, __LINE__)
#define CHECK_REJECTED(input) check_rejected((input), __FILE__, __LINE__)

int main() {
  // Info-table suffixes.
  CHECK_DEMANGLED("Main_main_info", "Main.main");
  CHECK_DEMANGLED("Main_main_closure", "Main.main");
  CHECK_DEMANGLED("Main_main_con_info", "Main.main");
  CHECK_DEMANGLED("Main_main_static_info", "Main.main");
  CHECK_DEMANGLED("Main_main_caf", "Main.main");
  CHECK_DEMANGLED("Main_main_info_123", "Main.main");
  CHECK_DEMANGLED("Main_main_closure.cold", "Main.main");
  CHECK_DEMANGLED("Main_main_info$deadbeef", "Main.main");
  CHECK_DEMANGLED("Main_main_info@plt", "Main.main");
  CHECK_DEMANGLED("Main_main_closure_tbl", "Main.main");

  // Module paths, package qualifiers, and nested modules.
  CHECK_DEMANGLED("DataziList_map_info", "Data.List.map");
  CHECK_DEMANGLED("DatazdList_map_info", "Data.List.map");
  CHECK_DEMANGLED("DataziMapziInternal_lookup_info", "Data.Map.Internal.lookup");
  CHECK_DEMANGLED("DataziListziInternal_lookup_info", "Data.List.Internal.lookup");
  CHECK_DEMANGLED("base_GHCziIO_putStrLn_info", "GHC.IO.putStrLn");
  CHECK_DEMANGLED("base_GHCziIOziHandleziFD_stdout_closure", "GHC.IO.Handle.FD.stdout");
  CHECK_DEMANGLED("base_Main_main_caf", "Main.main");
  CHECK_DEMANGLED("ghczminternal_GHCziInternalziBase_zpzp_info", "GHC.Internal.Base.++");
  CHECK_DEMANGLED(
      "ghczmlibzmparserzm9zi12zi3zi20251228zm5kHqaukBkKG7ikputCRKLu_GHCziUtilsziEncoding_"
      "zzDecodeString_info",
      "GHC.Utils.Encoding.zDecodeString");

  // Suffixless symbols are accepted when the module path is z-encoded.
  CHECK_DEMANGLED("DataziList_map", "Data.List.map");
  CHECK_DEMANGLED("DatazdList_map", "Data.List.map");
  CHECK_DEMANGLED("GHCziIO_putStrLn", "GHC.IO.putStrLn");
  CHECK_DEMANGLED("base_GHCziIO_putStrLn", "GHC.IO.putStrLn");

  // Lower-case z escapes.
  CHECK_DEMANGLED("Foo_barzd_info", "Foo.bar.");
  CHECK_DEMANGLED("Foo_barzc_info", "Foo.bar:");
  CHECK_DEMANGLED("Foo_barzi_info", "Foo.bar#");
  CHECK_DEMANGLED("Foo_barzm_info", "Foo.bar-");
  CHECK_DEMANGLED("Foo_barzp_info", "Foo.bar+");
  CHECK_DEMANGLED("Foo_barzq_info", "Foo.bar?");
  CHECK_DEMANGLED("Foo_barzb_info", "Foo.bar\\");
  CHECK_DEMANGLED("Foo_barzl_info", "Foo.bar/");
  CHECK_DEMANGLED("Foo_barzs_info", "Foo.bar ");
  CHECK_DEMANGLED("Foo_barzg_info", "Foo.bar>");
  CHECK_DEMANGLED("Foo_barze_info", "Foo.bar=");

  // Upper-case Z escapes.
  CHECK_DEMANGLED("Foo_barZL_info", "Foo.bar(");
  CHECK_DEMANGLED("Foo_barZR_info", "Foo.bar)");
  CHECK_DEMANGLED("Foo_barZM_info", "Foo.bar[");
  CHECK_DEMANGLED("Foo_barZN_info", "Foo.bar]");
  CHECK_DEMANGLED("Foo_barZP_info", "Foo.bar@");
  CHECK_DEMANGLED("Foo_barZA_info", "Foo.bar&");
  CHECK_DEMANGLED("Foo_barZB_info", "Foo.bar|");
  CHECK_DEMANGLED("Foo_barZC_info", "Foo.bar^");
  CHECK_DEMANGLED("Foo_barZD_info", "Foo.bar$");
  CHECK_DEMANGLED("Foo_barZE_info", "Foo.bar%");
  CHECK_DEMANGLED("Foo_barZF_info", "Foo.bar!");
  CHECK_DEMANGLED("Foo_barZG_info", "Foo.bar\"");
  CHECK_DEMANGLED("Foo_barZH_info", "Foo.bar'");
  CHECK_DEMANGLED("Foo_barZK_info", "Foo.bar;");
  CHECK_DEMANGLED("Foo_barZO_info", "Foo.bar~");

  // Escape doubling and unknown safe printable escapes.
  CHECK_DEMANGLED("Foo_aZZb_info", "Foo.aZb");
  CHECK_DEMANGLED("Foo_barzz_info", "Foo.barz");
  CHECK_DEMANGLED("Foo_barz{_info", "Foo.bar{");
  CHECK_DEMANGLED("Foo_barZ@_info", "Foo.bar@");

  // Tuple encodings.
  CHECK_DEMANGLED("GHCziTypes_Z4T_con_info", "GHC.Types.(,,,)");
  CHECK_DEMANGLED("GHCziTypes_Z4H_con_info", "GHC.Types.(#,,,#)");
  CHECK_DEMANGLED("GHCziTypes_Z3H_con_info", "GHC.Types.(#,,#)");
  CHECK_DEMANGLED("GHCziTypes_Z1H_info", "GHC.Types.(# #)");
  CHECK_DEMANGLED("GHCziTypes_Z0T_info", "GHC.Types.()");

  // Unicode code point escapes.
  CHECK_DEMANGLED("Foo_barz955U_info", "Foo.bar\xCE\xBB");
  CHECK_DEMANGLED("Foo_z1234U_info", "Foo.\xD3\x92");
  CHECK_DEMANGLED("Foo_z4660U_info", "Foo.\xE1\x88\xB4");
  CHECK_DEMANGLED("Foo_barz128512U_info", "Foo.bar\xF0\x9F\x98\x80");

  // Ordinary C and C++ names must not be classified as Haskell.
  CHECK_REJECTED("main");
  CHECK_REJECTED("abort");
  CHECK_REJECTED("_ZN3foo3barE");
  CHECK_REJECTED("_c1zz_info");
  CHECK_REJECTED("_info");
  CHECK_REJECTED("main_info");
  CHECK_REJECTED("foo_bar_info");
  CHECK_REJECTED("size_info");
  CHECK_REJECTED("Foo_bar");
  CHECK_REJECTED("Data_List_map");
  CHECK_REJECTED("Main_main");
  CHECK_REJECTED("DataziList");
  CHECK_REJECTED("Foo_barzi");

  // Malformed escapes and malformed symbol shapes.
  CHECK_REJECTED("");
  CHECK_REJECTED("Main_main_");
  CHECK_REJECTED("Main_main_info_");
  CHECK_REJECTED("Foo_123_info");
  CHECK_REJECTED("Foo_barz_info");
  CHECK_REJECTED("Foo_barZ_info");
  CHECK_REJECTED("Foo_barz123_info");
  CHECK_REJECTED("Foo_barZ9X_info");
  CHECK_REJECTED("Foo_barz0U_info");
  CHECK_REJECTED("Foo_barz55296U_info");
  CHECK_REJECTED("Foo_barz1114112U_info");
  CHECK_REJECTED("Foo_bar$baz_info");
  CHECK_REJECTED("Foo_barZY_info");
  CHECK_REJECTED("Foo_barzU_info");

  std::cout << "haskell_demangle_test: " << vector_count << " vectors, " << nix_sym_test::failures
            << " failure(s)\n";
  return nix_sym_test::failures == 0 ? 0 : 1;
}
