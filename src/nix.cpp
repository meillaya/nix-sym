#include "nix.hpp"

#include <glob.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "process.hpp"

namespace nix_sym {
namespace {

bool starts_with(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string trim(const std::string& value) {
  size_t begin = value.find_first_not_of(" \t\r\n\"'");
  if (begin == std::string::npos) {
    return {};
  }
  size_t end = value.find_last_not_of(" \t\r\n\"'");
  return value.substr(begin, end - begin + 1);
}

std::vector<std::string> split_ws(const std::string& value) {
  std::vector<std::string> result;
  std::istringstream stream(value);
  std::string item;
  while (stream >> item) {
    result.push_back(trim(item));
  }
  return result;
}

std::vector<std::string> split_lines(const std::string& value) {
  std::vector<std::string> result;
  std::istringstream stream(value);
  std::string line;
  while (std::getline(stream, line)) {
    line = trim(line);
    if (!line.empty()) {
      result.push_back(line);
    }
  }
  return result;
}

bool is_store_path(const std::string& path) {
  return starts_with(path, "/nix/store/");
}

std::string store_root(const std::string& path) {
  if (!is_store_path(path)) {
    return {};
  }
  size_t slash = path.find('/', 11);
  if (slash == std::string::npos) {
    return path;
  }
  return path.substr(0, slash);
}

std::string basename(const std::string& path) {
  size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string read_symlink_target(const std::string& path) {
  std::error_code ec;
  std::filesystem::path link = std::filesystem::read_symlink(path, ec);
  if (ec || link.empty()) {
    return {};
  }
  std::filesystem::path resolved;
  if (link.is_absolute()) {
    resolved = link;
  } else {
    resolved = std::filesystem::path(path).parent_path() / link;
  }
  return resolved.lexically_normal().string();
}

std::string fnv_hex(const std::string& text) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << std::hex << hash;
  return out.str();
}

std::string build_id_hex(const std::string& id) {
  return id;
}

bool read_file(const std::string& path, std::string* content) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  std::ostringstream stream;
  stream << input.rdbuf();
  *content = stream.str();
  return true;
}

void write_file_atomic(const std::string& path, const std::string& content) {
  std::filesystem::path target(path);
  std::error_code ec;
  std::filesystem::create_directories(target.parent_path(), ec);
  std::filesystem::path temporary = target;
  temporary += ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      return;
    }
    output << content;
  }
  std::filesystem::rename(temporary, target, ec);
  if (ec) {
    std::filesystem::remove(temporary, ec);
  }
}

std::vector<std::string> glob_paths(const std::string& pattern) {
  std::vector<std::string> results;
  glob_t matches;
  std::memset(&matches, 0, sizeof(matches));
  int status = glob(pattern.c_str(), GLOB_NOSORT, nullptr, &matches);
  if (status == 0) {
    for (size_t i = 0; i < matches.gl_pathc; ++i) {
      results.emplace_back(matches.gl_pathv[i]);
    }
  }
  globfree(&matches);
  return results;
}

std::string strip_output_suffix(std::string name) {
  static const std::vector<std::string> suffixes = {"-bin", "-dev",    "-lib",   "-man",    "-doc",
                                                    "-out", "-static", "-debug", "-headers"};
  for (const auto& suffix : suffixes) {
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
      name.erase(name.size() - suffix.size());
      break;
    }
  }
  return name;
}

} // namespace

NixResolver::NixResolver(const std::string& nix_bin, const std::string& cache_root, bool fetch,
                         bool offline, bool build_missing, bool refresh, bool no_cache)
    : nix_bin_(nix_bin), cache_root_(cache_root), fetch_(fetch), offline_(offline),
      build_missing_(build_missing), refresh_(refresh), no_cache_(no_cache) {
  std::filesystem::path binary(nix_bin_);
  if (binary.has_parent_path()) {
    nix_store_bin_ = (binary.parent_path() / "nix-store").string();
  } else {
    nix_store_bin_ = "nix-store";
  }
}

std::string NixResolver::run_nix(const std::vector<std::string>& args, bool* ok) {
  std::vector<std::string> argv;
  argv.push_back(nix_bin_);
  argv.insert(argv.end(), args.begin(), args.end());
  ProcessResult result = run_process(argv, 180);
  *ok = result.started && !result.timed_out && result.exit_code == 0;
  return *ok ? result.out : result.err;
}

std::string NixResolver::cache_key(const std::string& module_path,
                                   const std::string& build_id) const {
  if (!build_id.empty()) {
    return "buildid-" + build_id_hex(build_id);
  }
  return "path-" + fnv_hex(module_path);
}

bool NixResolver::read_cache(const std::string& key, DebugResolution* out) {
  if (no_cache_ || refresh_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(cache_mutex_);
  std::string content;
  if (!read_file(cache_root_ + "/resolved/" + key + ".txt", &content)) {
    return false;
  }
  bool any = false;
  std::vector<std::string> lines = split_lines(content);
  for (const auto& line : lines) {
    auto equals = line.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    std::string name = line.substr(0, equals);
    std::string value = line.substr(equals + 1);
    if (name == "status") {
      if (value == "resolved") {
        out->status = ModuleStatus::Resolved;
      } else if (value == "missing-debug") {
        out->status = ModuleStatus::MissingDebug;
      } else if (value == "no-build-id") {
        out->status = ModuleStatus::NoBuildId;
      } else if (value == "missing-file") {
        out->status = ModuleStatus::MissingFile;
      } else {
        out->status = ModuleStatus::Unknown;
      }
      any = true;
    } else if (name == "drv") {
      out->drv_path = value;
    } else if (name == "debug") {
      out->debug_output = value;
    } else if (name == "debug_file") {
      out->debug_file = value;
    } else if (name == "source") {
      out->source_path = value;
    } else if (name == "overlay") {
      out->overlay_path = value;
    } else if (name == "source_hint") {
      out->source_hint = value;
    }
  }
  if (!any) {
    return false;
  }
  if (out->status == ModuleStatus::Resolved && !std::filesystem::exists(out->debug_output)) {
    return false;
  }
  return true;
}

void NixResolver::write_cache(const std::string& key, const DebugResolution& out) {
  if (no_cache_) {
    return;
  }
  std::lock_guard<std::mutex> lock(cache_mutex_);
  std::string status;
  if (out.status == ModuleStatus::Resolved) {
    status = "resolved";
  } else if (out.status == ModuleStatus::MissingDebug) {
    status = "missing-debug";
  } else if (out.status == ModuleStatus::NoBuildId) {
    status = "no-build-id";
  } else if (out.status == ModuleStatus::MissingFile) {
    status = "missing-file";
  } else {
    return;
  }
  std::ostringstream content;
  content << "version=1\n";
  content << "status=" << status << "\n";
  content << "drv=" << out.drv_path << "\n";
  content << "debug=" << out.debug_output << "\n";
  content << "debug_file=" << out.debug_file << "\n";
  content << "source=" << out.source_path << "\n";
  content << "overlay=" << out.overlay_path << "\n";
  content << "source_hint=" << out.source_hint << "\n";
  write_file_atomic(cache_root_ + "/resolved/" + key + ".txt", content.str());
}

const std::vector<std::string>& NixResolver::load_substituters() {
  std::lock_guard<std::mutex> lock(substituters_mutex_);
  if (substituters_loaded_) {
    return substituters_;
  }
  substituters_loaded_ = true;
  bool ok = false;
  std::string output = run_nix({"config", "show", "substituters"}, &ok);
  if (ok) {
    for (const auto& item : split_ws(output)) {
      if (item.empty() || item == "local:" || starts_with(item, "dummy:")) {
        continue;
      }
      if (std::find(substituters_.begin(), substituters_.end(), item) == substituters_.end()) {
        substituters_.push_back(item);
      }
    }
  }
  if (substituters_.empty()) {
    substituters_.push_back("https://cache.nixos.org");
  }
  return substituters_;
}

const std::vector<std::string>& NixResolver::substituters() {
  return load_substituters();
}

bool NixResolver::fetch_path(const std::string& path, std::vector<Warning>* warnings) {
  if (!fetch_ || offline_ || !is_store_path(path)) {
    return false;
  }
  if (std::filesystem::exists(path)) {
    return true;
  }
  std::string last_error;
  for (const auto& substituter : load_substituters()) {
    ProcessResult result = run_process({nix_bin_, "copy", "--from", substituter, path}, 600);
    if (result.started && !result.timed_out && result.exit_code == 0 &&
        std::filesystem::exists(path)) {
      return true;
    }
    last_error = result.err.empty() ? result.out : result.err;
  }
  if (warnings != nullptr) {
    warnings->push_back({"fetch-failed", path,
                         "could not fetch debug output" +
                             (last_error.empty() ? std::string() : ": " + last_error)});
  }
  return false;
}

std::string NixResolver::readlink_path(const std::string& path) {
  return read_symlink_target(path);
}

bool NixResolver::file_exists(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

std::string NixResolver::debug_file_for(const std::string& debug_output,
                                        const std::string& build_id,
                                        const std::string& module_path) {
  if (!build_id.empty() && build_id.size() >= 2) {
    std::string prefix = build_id.substr(0, 2);
    std::string rest = build_id.substr(2);
    std::string candidate = debug_output + "/lib/debug/.build-id/" + prefix + "/" + rest + ".debug";
    if (file_exists(candidate)) {
      return candidate;
    }
  }
  std::string legacy = debug_output + "/lib/debug/" + basename(module_path);
  if (file_exists(legacy)) {
    return legacy;
  }
  std::string debug_directory = debug_output + "/lib/debug";
  for (const auto& entry : glob_paths(debug_directory + "/*.debug")) {
    return entry;
  }
  return {};
}

DebugResolution NixResolver::resolve_one(const std::string& module_path,
                                         const std::string& build_id) {
  DebugResolution result;
  if (!is_store_path(module_path)) {
    result.status = ModuleStatus::NonNix;
    return result;
  }

  std::string key = cache_key(module_path, build_id);
  if (read_cache(key, &result)) {
    if (result.status == ModuleStatus::Resolved) {
      return result;
    }
    if (offline_ || !fetch_) {
      if (result.status != ModuleStatus::NonNix && result.status != ModuleStatus::Unknown) {
        result.warnings.push_back(
            {"debug-info-missing", module_path, "using cached resolution without fetching"});
      }
      return result;
    }
  }

  std::string root = store_root(module_path);
  bool ok = false;
  std::string drv_output = run_nix({"path-info", "--derivation", root}, &ok);
  std::string drv;
  if (ok) {
    std::vector<std::string> lines = split_lines(drv_output);
    if (!lines.empty() && starts_with(lines.front(), "/nix/store/") && lines.front().size() > 4 &&
        lines.front().compare(lines.front().size() - 4, 4, ".drv") == 0 &&
        file_exists(lines.front())) {
      drv = lines.front();
    }
  }
  result.drv_path = drv;

  std::string debug_output;
  if (!drv.empty()) {
    ProcessResult outputs = run_process({nix_store_bin_, "-q", "--outputs", drv}, 60);
    if (outputs.started && !outputs.timed_out && outputs.exit_code == 0) {
      for (const auto& line : split_lines(outputs.out)) {
        if (line.size() > 6 && line.compare(line.size() - 6, 6, "-debug") == 0) {
          debug_output = line;
          break;
        }
      }
    }
  }

  if (!debug_output.empty() && !file_exists(debug_output)) {
    if (fetch_) {
      std::vector<Warning> warnings;
      fetch_path(debug_output, &warnings);
      result.warnings.insert(result.warnings.end(), warnings.begin(), warnings.end());
    }
    if (!file_exists(debug_output) && build_missing_) {
      ProcessResult built = run_process({nix_store_bin_, "--realise", debug_output}, 1800);
      if (built.started && built.exit_code != 0) {
        result.warnings.push_back(
            {"fetch-failed", root, "nix-store --realise did not produce the debug output"});
      }
    }
  }

  if (debug_output.empty() || !file_exists(debug_output)) {
    if (!build_id.empty() && build_id.size() >= 2) {
      std::string pattern = "/nix/store/*-debug/lib/debug/.build-id/" + build_id.substr(0, 2) +
                            "/" + build_id.substr(2) + ".debug";
      for (const auto& candidate : glob_paths(pattern)) {
        size_t marker = candidate.find("/lib/debug/");
        if (marker != std::string::npos) {
          debug_output = candidate.substr(0, marker);
          break;
        }
      }
    }
    if (debug_output.empty()) {
      std::string name = strip_output_suffix(basename(root));
      if (!name.empty()) {
        for (const auto& candidate : glob_paths("/nix/store/*-" + name + "-debug")) {
          std::string executable = candidate + "/lib/debug/.build-id/";
          (void)executable;
          debug_output = candidate;
          break;
        }
      }
    }
    if (!debug_output.empty() && file_exists(debug_output) && !build_id.empty()) {
      std::string expected = debug_output + "/lib/debug/.build-id/" + build_id.substr(0, 2) + "/" +
                             build_id.substr(2) + ".debug";
      if (!file_exists(expected)) {
        debug_output.clear();
      }
    }
  }

  if (debug_output.empty() || !file_exists(debug_output)) {
    result.status = build_id.empty() ? ModuleStatus::NoBuildId : ModuleStatus::MissingDebug;
    result.warnings.push_back({"debug-info-missing", module_path,
                               build_id.empty()
                                   ? "module has no build-id and no debug output was found"
                                   : "no debug output was found for build-id " + build_id});
    write_cache(key, result);
    return result;
  }

  result.debug_output = debug_output;
  result.debug_file = debug_file_for(debug_output, build_id, module_path);
  if (result.debug_file.empty()) {
    result.status = ModuleStatus::MissingDebug;
    result.warnings.push_back({"debug-info-missing", module_path,
                               "debug output exists but contains no matching debug file"});
    write_cache(key, result);
    return result;
  }

  if (!build_id.empty()) {
    std::string prefix =
        debug_output + "/lib/debug/.build-id/" + build_id.substr(0, 2) + "/" + build_id.substr(2);
    result.source_path = readlink_path(prefix + ".source");
    result.overlay_path = readlink_path(prefix + ".sourceoverlay");
    if (result.source_path.empty()) {
      result.overlay_path.clear();
    }
  }

  if (!result.source_path.empty() && !file_exists(result.source_path) && fetch_) {
    std::vector<Warning> warnings;
    fetch_path(result.source_path, &warnings);
    result.warnings.insert(result.warnings.end(), warnings.begin(), warnings.end());
  }

  if (result.source_path.empty() && !drv.empty()) {
    std::string drv_text;
    if (read_file(drv, &drv_text)) {
      std::regex source_pattern("\\(\"src\",\"(/nix/store/[^\"]+)\"\\)");
      std::smatch match;
      if (std::regex_search(drv_text, match, source_pattern)) {
        result.source_hint = match[1].str();
      }
    }
  }

  result.status = ModuleStatus::Resolved;
  write_cache(key, result);
  return result;
}

DebugResolution NixResolver::resolve(const std::string& module_path, const std::string& build_id) {
  return resolve_one(module_path, build_id);
}

std::vector<DebugResolution>
NixResolver::resolve_batch(const std::vector<ModuleRequest>& requests) {
  std::vector<DebugResolution> results(requests.size());
  if (requests.empty()) {
    return results;
  }
  if (requests.size() == 1) {
    results[0] = resolve_one(requests[0].path, requests[0].build_id);
    return results;
  }
  std::atomic<size_t> next{0};
  size_t worker_count = std::min<size_t>(4, requests.size());
  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (size_t worker = 0; worker < worker_count; ++worker) {
    workers.emplace_back([&]() {
      while (true) {
        size_t index = next.fetch_add(1);
        if (index >= requests.size()) {
          break;
        }
        results[index] = resolve_one(requests[index].path, requests[index].build_id);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  return results;
}

} // namespace nix_sym
