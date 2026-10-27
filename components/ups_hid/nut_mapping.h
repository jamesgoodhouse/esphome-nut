#pragma once

// Maps HID Power Device fields to NUT variables. Pure C++ (no ESPHome / ESP-IDF dependencies).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "hid_parser.h"

namespace esphome::ups_hid {

/// Status flags that make up NUT's ups.status.
enum StatusFlag : uint8_t {
  FLAG_AC_PRESENT,
  FLAG_CHARGING,
  FLAG_DISCHARGING,
  FLAG_BELOW_CAPACITY_LIMIT,
  FLAG_TIME_LIMIT_EXPIRED,
  FLAG_SHUTDOWN_IMMINENT,
  FLAG_NEED_REPLACEMENT,
  FLAG_OVERLOAD,
  FLAG_BOOST,
  FLAG_BUCK,
  FLAG_COUNT,
};

/// Turns raw HID reports into NUT variables for one connected device.
class NutMapper {
 public:
  /// Resolve which descriptor fields feed which NUT variables. Call once per connected device.
  void bind(const HidDescriptor &descriptor, uint16_t vendor_id, uint16_t product_id);

  /// True if the device exposes enough to build ups.status (AC present or discharging flag).
  bool can_report_status() const;
  /// Number of NUT values/flags that were found in the descriptor.
  size_t bound_count() const;

  /// Reports that must be read (via GET_REPORT or the interrupt endpoint) to fill all values.
  std::vector<HidReport> needed_reports() const;

  /// Store the latest payload of a report (payload excludes the report ID byte).
  void update_report(ReportType type, uint8_t id, const uint8_t *payload, size_t len);
  /// Forget all stored report payloads.
  void clear_reports() { this->payloads_.clear(); }

  /// Write all NUT variables that currently have data into *vars (including ups.status).
  void build(std::map<std::string, std::string> *vars) const;

  /// One line per bound value, showing where it comes from and how it is scaled. For troubleshooting.
  std::vector<std::string> describe() const;

 protected:
  struct Binding {
    const char *nut_name;
    /// AudibleAlarmControl enum rather than a number
    bool beeper;
    HidField field;
    double scale;
  };
  struct FlagBinding {
    bool bound{false};
    HidField field;
  };

  bool read_field_(const HidField &field, int32_t *out) const;
  double physical_(const Binding &binding, int32_t raw) const;

  std::vector<Binding> values_;
  FlagBinding flags_[FLAG_COUNT];
  std::map<uint16_t, std::vector<uint8_t>> payloads_;
  uint16_t vendor_id_{0};
  uint16_t product_id_{0};
};

/// Format a number the way NUT drivers do: no trailing zeros, at most 2 decimals.
std::string format_nut_number(double value);

/// Turn a NUT status string ("OB DISCHRG LB") into plain English ("On battery, discharging, battery low").
std::string describe_status(const std::string &status);

}  // namespace esphome::ups_hid
