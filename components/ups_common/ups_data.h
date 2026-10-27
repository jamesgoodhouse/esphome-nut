#pragma once

// Shared contract between UPS drivers (e.g. ups_hid) and consumers (e.g. nut_server).
// Kept free of ESPHome core includes so it can be used in host-side unit tests.

#include <map>
#include <string>
#include <vector>

namespace esphome::ups_common {

/// Point-in-time view of a UPS, using NUT variable names (battery.charge, ups.status, ...).
struct UpsSnapshot {
  /// A UPS is attached and has been identified.
  bool connected{false};
  /// Connected, but no fresh data has been read recently.
  bool stale{false};
  /// NUT variables, name -> value. Always contains ups.status when connected.
  std::map<std::string, std::string> vars;
  /// NUT instant commands that can be run right now (e.g. "beeper.disable"). Empty unless control is allowed.
  std::vector<std::string> commands;
};

/// Anything that can provide UPS data to a consumer.
class UpsSource {
 public:
  virtual ~UpsSource() = default;
  virtual const UpsSnapshot &get_ups_snapshot() const = 0;
  /// Start a NUT instant command listed in the snapshot. Returns false if it could not be started.
  virtual bool run_command(const std::string &name) {
    (void) name;
    return false;
  }
};

}  // namespace esphome::ups_common
