#include "model.hpp"

namespace nix_sym {

const char* module_status_name(ModuleStatus status) {
  switch (status) {
  case ModuleStatus::Resolved:
    return "resolved";
  case ModuleStatus::MissingDebug:
    return "missing-debug";
  case ModuleStatus::MissingFile:
    return "missing-file";
  case ModuleStatus::NoBuildId:
    return "no-build-id";
  case ModuleStatus::NonNix:
    return "non-nix";
  case ModuleStatus::Unknown:
    return "unknown";
  }
  return "unknown";
}

} // namespace nix_sym
