#pragma once

// Per-vendor fixups for UPSes that report values incorrectly. Pure C++.
//
// The generic HID mapping handles well-behaved devices. Add an entry here only for a
// confirmed device bug, with a note on where it was observed.

#include <cstdint>
#include <map>
#include <string>

namespace esphome::ups_hid {

constexpr uint16_t VENDOR_APC = 0x051D;
constexpr uint16_t VENDOR_CYBERPOWER = 0x0764;
constexpr uint16_t VENDOR_EATON = 0x0463;

/// Fixed multiplier for one NUT variable on a device (pid 0 matches every product of the vendor).
struct ScaleQuirk {
  uint16_t vendor_id;
  uint16_t product_id;
  const char *nut_name;
  double scale;
};

/// Multiplier to apply to a variable for this device; 1.0 when there is no quirk.
double quirk_scale(uint16_t vendor_id, uint16_t product_id, const char *nut_name);

/// Fixups that depend on more than one value. Runs after all variables are built.
void apply_value_quirks(uint16_t vendor_id, uint16_t product_id, std::map<std::string, std::string> *vars);

}  // namespace esphome::ups_hid
