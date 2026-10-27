#pragma once

// HID report descriptor parser. Pure C++ (no ESPHome / ESP-IDF dependencies) so it can be
// unit-tested on the host.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome::ups_hid {

enum class ReportType : uint8_t { INPUT = 1, OUTPUT = 2, FEATURE = 3 };

/// One value-carrying field of a report.
struct HidField {
  ReportType type;
  uint8_t report_id;
  /// Bit position within the report payload (the payload excludes the report ID byte).
  uint16_t bit_offset;
  uint8_t bit_size;
  int32_t logical_min;
  int32_t logical_max;
  int8_t unit_exponent;
  uint32_t unit;
  /// HID "constant" flag: a fixed value the host cannot change. Otherwise feature fields are writable.
  bool constant;
  /// Usages of the enclosing collections (outermost first), followed by the field's own usage.
  std::vector<uint32_t> path;

  uint32_t usage() const { return this->path.empty() ? 0 : this->path.back(); }
  /// The host may change this field with SET_REPORT.
  bool writable() const { return this->type == ReportType::FEATURE && !this->constant; }
};

/// A report the device can send or answer GET_REPORT for.
struct HidReport {
  ReportType type;
  uint8_t id;
  uint16_t bit_length;
  /// Payload size in bytes, excluding the report ID byte.
  size_t byte_length() const { return (this->bit_length + 7) / 8; }
};

class HidDescriptor {
 public:
  /// Parse a raw report descriptor. On failure returns false and sets *error (if given).
  bool parse(const uint8_t *data, size_t len, std::string *error = nullptr);

  const std::vector<HidField> &fields() const { return this->fields_; }
  const std::vector<HidReport> &reports() const { return this->reports_; }
  bool uses_report_ids() const { return this->uses_report_ids_; }
  /// True if any field or collection uses the given usage page.
  bool has_usage_page(uint16_t page) const;
  const HidReport *find_report(ReportType type, uint8_t id) const;

 protected:
  std::vector<HidField> fields_;
  std::vector<HidReport> reports_;
  std::vector<uint16_t> pages_seen_;
  bool uses_report_ids_{false};
};

/// Extract a field's raw logical value from a report payload (payload excludes the report ID).
/// Returns false if the payload is too short.
bool hid_extract(const HidField &field, const uint8_t *payload, size_t len, int32_t *out);

/// Write a field's raw logical value into a report payload, leaving the other bits alone.
/// Returns false if the payload is too short.
bool hid_insert(const HidField &field, uint8_t *payload, size_t len, int32_t value);

/// Convert a logical value to a physical value in base SI units (volts, seconds, hertz, ...),
/// applying the unit exponent the same way NUT's libhid does.
double hid_to_physical(const HidField &field, int32_t logical);

}  // namespace esphome::ups_hid
