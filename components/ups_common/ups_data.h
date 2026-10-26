#pragma once

// Shared contract between UPS drivers (e.g. ups_hid) and consumers (e.g. nut_server).
// Kept free of ESPHome core includes so it can be used in host-side unit tests.

#include <map>
#include <string>

namespace esphome::ups_common {

/// Point-in-time view of a UPS, using NUT variable names (battery.charge, ups.status, ...).
struct UpsSnapshot {
  /// A UPS is attached and has been identified.
  bool connected{false};
  /// Connected, but no fresh data has been read recently.
  bool stale{false};
  /// NUT variables, name -> value. Always contains ups.status when connected.
  std::map<std::string, std::string> vars;
};

/// Anything that can provide UPS data to a consumer.
class UpsSource {
 public:
  virtual ~UpsSource() = default;
  virtual const UpsSnapshot &get_ups_snapshot() const = 0;
};

}  // namespace esphome::ups_common
