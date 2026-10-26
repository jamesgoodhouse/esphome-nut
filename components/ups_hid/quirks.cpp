#include "quirks.h"

#include <cstdlib>
#include <cstring>

#include "nut_mapping.h"

namespace esphome::ups_hid {

// Fixed scale corrections. Empty for now: every entry must come from a confirmed device report
// (a descriptor dump plus the values the UPS front panel or vendor software shows).
static const ScaleQuirk SCALE_QUIRKS[] = {
    {0, 0, nullptr, 1.0},  // sentinel
};

double quirk_scale(uint16_t vendor_id, uint16_t product_id, const char *nut_name) {
  for (const auto &quirk : SCALE_QUIRKS) {
    if (quirk.nut_name == nullptr)
      break;
    if (quirk.vendor_id == vendor_id && (quirk.product_id == 0 || quirk.product_id == product_id) &&
        std::strcmp(quirk.nut_name, nut_name) == 0)
      return quirk.scale;
  }
  return 1.0;
}

static bool get_number(const std::map<std::string, std::string> &vars, const char *name, double *out) {
  auto it = vars.find(name);
  if (it == vars.end())
    return false;
  char *end = nullptr;
  *out = std::strtod(it->second.c_str(), &end);
  return end != it->second.c_str();
}

void apply_value_quirks(uint16_t vendor_id, uint16_t product_id, std::map<std::string, std::string> *vars) {
  (void) product_id;
  if (vendor_id == VENDOR_CYBERPOWER) {
    // Many CyberPower models report battery voltage scaled by 1.5. NUT's cps-hid driver detects
    // this by comparing against the nominal voltage and rescales by 2/3.
    double voltage, nominal;
    if (get_number(*vars, "battery.voltage", &voltage) && get_number(*vars, "battery.voltage.nominal", &nominal) &&
        nominal > 0 && voltage > nominal * 1.4) {
      (*vars)["battery.voltage"] = format_nut_number(voltage * 2.0 / 3.0);
    }
  }
}

}  // namespace esphome::ups_hid
