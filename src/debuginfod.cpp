#include "debuginfod.hpp"

#include <elfutils/debuginfod.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace nix_sym {

struct DebuginfodClient::Impl {
  debuginfod_client* client = nullptr;
  std::mutex client_mutex;
  std::mutex install_mutex;
  std::mutex source_mutex;
  unsigned long long temp_sequence = 0;
  std::string disabled_reason;
};

namespace {

namespace fs = std::filesystem;

std::vector<std::string> split_whitespace(const std::string& text) {
  std::vector<std::string> parts;
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) {
      ++index;
    }
    const std::size_t begin = index;
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) == 0) {
      ++index;
    }
    if (index > begin) {
      parts.push_back(text.substr(begin, index - begin));
    }
  }
  return parts;
}

std::string join(const std::vector<std::string>& parts) {
  std::string result;
  for (const std::string& part : parts) {
    if (!result.empty()) {
      result.push_back(' ');
    }
    result += part;
  }
  return result;
}

std::string describe_byte(unsigned char byte) {
  if (byte >= 0x20 && byte <= 0x7e) {
    return std::string("'") + static_cast<char>(byte) + "'";
  }
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "0x%02x", static_cast<unsigned int>(byte));
  return buffer;
}

// Lowercases and validates a build id. libdebuginfod accepts a lowercase hex
// string when build_id_len is zero.
bool normalize_build_id(const std::string& build_id_hex, std::string* normalized,
                        std::string* error) {
  normalized->clear();
  if (build_id_hex.empty()) {
    *error = "invalid build id: empty";
    return false;
  }

  normalized->reserve(build_id_hex.size());
  for (std::size_t index = 0; index < build_id_hex.size(); ++index) {
    unsigned char byte = static_cast<unsigned char>(build_id_hex[index]);
    if (byte >= 'A' && byte <= 'F') {
      byte = static_cast<unsigned char>(byte - 'A' + 'a');
    }
    const bool is_hex_digit = (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
    if (!is_hex_digit) {
      *error = "invalid build id: non-hex character " + describe_byte(byte) + " at offset " +
               std::to_string(index);
      normalized->clear();
      return false;
    }
    normalized->push_back(static_cast<char>(byte));
  }

  if (normalized->size() % 2 != 0) {
    *error =
        "invalid build id: odd number of hex digits (" + std::to_string(normalized->size()) + ")";
    normalized->clear();
    return false;
  }
  return true;
}

bool has_nul(const std::string& text) {
  return text.find('\0') != std::string::npos;
}

bool validate_fetched_path(const std::string& fetched_path, std::string* error) {
  if (fetched_path.empty()) {
    *error = "fetched path is empty";
    return false;
  }
  if (has_nul(fetched_path)) {
    *error = "fetched path contains a NUL byte";
    return false;
  }

  std::error_code ec;
  if (!fs::is_regular_file(fs::path(fetched_path), ec)) {
    std::string message = "fetched path is not a regular file: '" + fetched_path + "'";
    if (ec) {
      message += ": " + ec.message() + " (errno " + std::to_string(ec.value()) + ")";
    }
    *error = std::move(message);
    return false;
  }
  return true;
}

std::string path_error(const std::string& action, const fs::path& path, const std::error_code& ec) {
  return action + " '" + path.string() + "': " + ec.message() + " (errno " +
         std::to_string(ec.value()) + ")";
}

std::string install_failure(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return {};
}

// Turns a libdebuginfod return value into a result. On success the returned
// descriptor is closed, the path is copied, and the API-owned path is freed.
DebuginfodResult finish_find(int return_value, char* path, const char* operation,
                             const std::string& build_id, int saved_errno) {
  if (return_value < 0) {
    std::string message = std::string(operation) + " failed for build id " + build_id +
                          ": returned " + std::to_string(return_value) + " (" +
                          std::strerror(-return_value) + ")";
    if (saved_errno != 0) {
      message += "; errno " + std::to_string(saved_errno) + " (" + std::strerror(saved_errno) + ")";
    }
    if (path != nullptr) {
      std::free(path);
    }
    return {false, {}, std::move(message)};
  }

  (void)::close(return_value);

  if (path == nullptr) {
    return {false, {}, std::string(operation) + " returned a file descriptor but no path"};
  }

  std::string result_path(path);
  std::free(path);
  if (result_path.empty()) {
    return {false, {}, std::string(operation) + " returned an empty path"};
  }
  return {true, std::move(result_path), {}};
}

} // namespace

DebuginfodClient::DebuginfodClient(const std::string& cache_root, std::vector<std::string> urls,
                                   bool offline)
    : impl_(new Impl()), cache_root_(cache_root), offline_(offline) {
  std::vector<std::string> requested;
  for (const std::string& url : urls) {
    const std::vector<std::string> parts = split_whitespace(url);
    requested.insert(requested.end(), parts.begin(), parts.end());
  }

  bool environment_ok = true;
  if (!urls.empty()) {
    const std::string environment_value = join(requested);
    if (!environment_value.empty() &&
        ::setenv(DEBUGINFOD_URLS_ENV_VAR, environment_value.c_str(), 1) != 0) {
      environment_ok = false;
    }
    urls_ = requested;
  } else {
    const char* environment_value = std::getenv(DEBUGINFOD_URLS_ENV_VAR);
    if (environment_value != nullptr) {
      urls_ = split_whitespace(environment_value);
    }
  }

  if (offline_) {
    impl_->disabled_reason = "debuginfod is disabled: offline mode";
    return;
  }
  if (urls_.empty()) {
    impl_->disabled_reason = "debuginfod is disabled: no DEBUGINFOD_URLS configured";
    return;
  }
  if (!environment_ok) {
    impl_->disabled_reason = "debuginfod is disabled: could not set DEBUGINFOD_URLS";
    return;
  }

  // Keep libdebuginfod's download cache inside nix-sym's cache. The default
  // ~/.cache/debuginfod_client may be read-only in sandboxes and CI.
  std::error_code cache_ec;
  fs::path debuginfod_cache = fs::path(cache_root_) / "debuginfod-client";
  fs::create_directories(debuginfod_cache, cache_ec);
  if (!cache_ec) {
    ::setenv("DEBUGINFOD_CACHE_PATH", debuginfod_cache.string().c_str(), 1);
  }

  {
    std::lock_guard<std::mutex> lock(impl_->client_mutex);
    impl_->client = debuginfod_begin();
  }
  if (impl_->client == nullptr) {
    impl_->disabled_reason = "debuginfod_begin() failed";
  }
}

DebuginfodClient::~DebuginfodClient() {
  if (impl_ == nullptr) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->client_mutex);
    if (impl_->client != nullptr) {
      debuginfod_end(impl_->client);
      impl_->client = nullptr;
    }
  }
  delete impl_;
  impl_ = nullptr;
}

bool DebuginfodClient::enabled() const {
  if (impl_ == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->client_mutex);
  return impl_->client != nullptr;
}

const std::vector<std::string>& DebuginfodClient::urls() const {
  return urls_;
}

DebuginfodResult DebuginfodClient::find_debuginfo(const std::string& build_id_hex) {
  std::string build_id;
  std::string error;
  if (!normalize_build_id(build_id_hex, &build_id, &error)) {
    return {false, {}, std::move(error)};
  }

  debuginfod_client* client = nullptr;
  std::string disabled_reason;
  {
    std::lock_guard<std::mutex> lock(impl_->client_mutex);
    client = impl_->client;
    if (client == nullptr) {
      disabled_reason =
          impl_->disabled_reason.empty() ? "debuginfod client is disabled" : impl_->disabled_reason;
    }
  }
  if (client == nullptr) {
    return {false, {}, std::move(disabled_reason)};
  }

  char* path = nullptr;
  errno = 0;
  const int return_value = debuginfod_find_debuginfo(
      client, reinterpret_cast<const unsigned char*>(build_id.c_str()), 0, &path);
  const int saved_errno = errno;
  return finish_find(return_value, path, "debuginfod_find_debuginfo", build_id, saved_errno);
}

DebuginfodResult DebuginfodClient::find_source(const std::string& build_id_hex,
                                               const std::string& source_path) {
  std::string build_id;
  std::string error;
  if (!normalize_build_id(build_id_hex, &build_id, &error)) {
    return {false, {}, std::move(error)};
  }

  debuginfod_client* client = nullptr;
  std::string disabled_reason;
  {
    std::lock_guard<std::mutex> lock(impl_->client_mutex);
    client = impl_->client;
    if (client == nullptr) {
      disabled_reason =
          impl_->disabled_reason.empty() ? "debuginfod client is disabled" : impl_->disabled_reason;
    }
  }
  if (client == nullptr) {
    return {false, {}, std::move(disabled_reason)};
  }

  // The source path is deliberately passed through unchanged.
  char* path = nullptr;
  errno = 0;
  const int return_value =
      debuginfod_find_source(client, reinterpret_cast<const unsigned char*>(build_id.c_str()), 0,
                             source_path.c_str(), &path);
  const int saved_errno = errno;
  return finish_find(return_value, path, "debuginfod_find_source", build_id, saved_errno);
}

std::string DebuginfodClient::install_debuginfo(const std::string& build_id_hex,
                                                const std::string& fetched_path,
                                                std::string* error) {
  if (error != nullptr) {
    error->clear();
  }

  std::string build_id;
  std::string validation_error;
  if (!normalize_build_id(build_id_hex, &build_id, &validation_error)) {
    return install_failure(error, validation_error);
  }
  if (!validate_fetched_path(fetched_path, &validation_error)) {
    return install_failure(error, validation_error);
  }

  const fs::path output_directory = fs::path(cache_root_) / "debuginfod" / build_id;
  const fs::path destination = output_directory / "lib" / "debug" / ".build-id" /
                               build_id.substr(0, 2) / (build_id.substr(2) + ".debug");
  const fs::path fetched(fetched_path);

  std::lock_guard<std::mutex> lock(impl_->install_mutex);

  std::error_code ec;
  const bool destination_exists = fs::exists(destination, ec);
  if (ec) {
    return install_failure(error, path_error("cannot inspect destination", destination, ec));
  }
  if (destination_exists) {
    return output_directory.string();
  }

  fs::create_directories(destination.parent_path(), ec);
  if (ec) {
    return install_failure(error,
                           path_error("cannot create directory", destination.parent_path(), ec));
  }

  const std::string suffix = ".tmp." + std::to_string(static_cast<long long>(::getpid())) + "." +
                             std::to_string(impl_->temp_sequence++);
  fs::path temporary = destination;
  temporary += fs::path(suffix);

  fs::copy_file(fetched, temporary, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return install_failure(error, "cannot copy fetched debug info '" + fetched_path + "' to '" +
                                      temporary.string() + "': " + ec.message() + " (errno " +
                                      std::to_string(ec.value()) + ")");
  }

  fs::rename(temporary, destination, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    std::error_code exists_error;
    const bool appeared = fs::exists(destination, exists_error);
    if (!exists_error && appeared) {
      return output_directory.string();
    }
    return install_failure(error,
                           path_error("cannot move temporary file into place", temporary, ec));
  }

  return output_directory.string();
}

std::string DebuginfodClient::install_source(const std::string& build_id_hex,
                                             const std::string& source_path,
                                             const std::string& fetched_path) {
  (void)build_id_hex;

  if (source_path.empty() || fetched_path.empty()) {
    return {};
  }
  if (has_nul(source_path) || has_nul(fetched_path)) {
    return {};
  }

  // The mirror only accepts a genuinely absolute source path. Removing one
  // leading slash must leave a relative path, otherwise a path such as
  // "//etc/passwd" would escape the mirror root when appended.
  if (source_path.size() < 2 || source_path.front() != '/') {
    return {};
  }
  const std::string relative_text = source_path.substr(1);
  if (relative_text.empty()) {
    return {};
  }
  const fs::path relative(relative_text);
  if (relative.is_absolute()) {
    return {};
  }
  for (const fs::path& component : relative) {
    if (component.empty() || component == "." || component == "..") {
      return {};
    }
  }

  std::string validation_error;
  if (!validate_fetched_path(fetched_path, &validation_error)) {
    return {};
  }

  const fs::path fetched(fetched_path);
  const fs::path destination = fs::path(cache_root_) / "debuginfod-sources" / relative;

  std::lock_guard<std::mutex> lock(impl_->source_mutex);

  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  if (ec) {
    return {};
  }

  const bool destination_exists = fs::exists(destination, ec);
  if (ec) {
    return {};
  }
  if (destination_exists) {
    if (!fs::remove(destination, ec) || ec) {
      return {};
    }
  }

  fs::copy_file(fetched, destination, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    return {};
  }
  return destination.string();
}

std::string DebuginfodClient::source_root() const {
  return (fs::path(cache_root_) / "debuginfod-sources").string();
}

} // namespace nix_sym
