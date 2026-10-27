#include "nut_mapping.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "hid_usages.h"
#include "quirks.h"

namespace esphome::ups_hid {

namespace {

using namespace usage;

using Pattern = std::initializer_list<uint32_t>;

enum class ValueKind : uint8_t { NUMBER, BEEPER };

struct ValueDef {
  const char *nut_name;
  ValueKind kind;
  std::initializer_list<Pattern> patterns;
};

// Candidate usage paths per NUT variable, best first. A pattern matches the end of a field's path.
// Mirrors the paths NUT's usbhid-ups subdrivers use for the standard HID Power Device usages.
const ValueDef VALUE_DEFS[] = {
    {"battery.charge", ValueKind::NUMBER,
     {{POWER_SUMMARY, REMAINING_CAPACITY}, {BATTERY, REMAINING_CAPACITY}, {REMAINING_CAPACITY}}},
    {"battery.charge.low", ValueKind::NUMBER,
     {{POWER_SUMMARY, REMAINING_CAPACITY_LIMIT}, {BATTERY, REMAINING_CAPACITY_LIMIT}, {REMAINING_CAPACITY_LIMIT}}},
    {"battery.runtime", ValueKind::NUMBER,
     {{POWER_SUMMARY, RUN_TIME_TO_EMPTY}, {BATTERY, RUN_TIME_TO_EMPTY}, {RUN_TIME_TO_EMPTY}}},
    {"battery.runtime.low", ValueKind::NUMBER,
     {{POWER_SUMMARY, REMAINING_TIME_LIMIT}, {BATTERY, REMAINING_TIME_LIMIT}, {REMAINING_TIME_LIMIT}}},
    {"battery.voltage", ValueKind::NUMBER, {{BATTERY, VOLTAGE}, {POWER_SUMMARY, VOLTAGE}}},
    {"battery.voltage.nominal", ValueKind::NUMBER, {{BATTERY, CONFIG_VOLTAGE}, {POWER_SUMMARY, CONFIG_VOLTAGE}}},
    {"battery.temperature", ValueKind::NUMBER, {{BATTERY, TEMPERATURE}}},
    {"input.voltage", ValueKind::NUMBER, {{INPUT, VOLTAGE}}},
    {"input.voltage.nominal", ValueKind::NUMBER, {{INPUT, CONFIG_VOLTAGE}}},
    {"input.frequency", ValueKind::NUMBER, {{INPUT, FREQUENCY}}},
    {"output.voltage", ValueKind::NUMBER, {{OUTPUT, VOLTAGE}}},
    {"output.voltage.nominal", ValueKind::NUMBER, {{OUTPUT, CONFIG_VOLTAGE}}},
    {"output.frequency", ValueKind::NUMBER, {{OUTPUT, FREQUENCY}}},
    {"output.current", ValueKind::NUMBER, {{OUTPUT, CURRENT}}},
    {"ups.load", ValueKind::NUMBER, {{OUTPUT, PERCENT_LOAD}, {POWER_SUMMARY, PERCENT_LOAD}, {PERCENT_LOAD}}},
    {"ups.power", ValueKind::NUMBER, {{OUTPUT, APPARENT_POWER}}},
    {"ups.realpower", ValueKind::NUMBER, {{OUTPUT, ACTIVE_POWER}}},
    {"ups.power.nominal", ValueKind::NUMBER, {{CONFIG_APPARENT_POWER}}},
    {"ups.realpower.nominal", ValueKind::NUMBER, {{CONFIG_ACTIVE_POWER}}},
    {"ups.beeper.status", ValueKind::BEEPER, {{POWER_SUMMARY, AUDIBLE_ALARM_CONTROL}, {AUDIBLE_ALARM_CONTROL}}},
};

struct FlagDef {
  StatusFlag flag;
  uint32_t usage;
};

const FlagDef FLAG_DEFS[] = {
    {FLAG_AC_PRESENT, AC_PRESENT},
    {FLAG_CHARGING, CHARGING},
    {FLAG_DISCHARGING, DISCHARGING},
    {FLAG_BELOW_CAPACITY_LIMIT, BELOW_REMAINING_CAPACITY_LIMIT},
    {FLAG_TIME_LIMIT_EXPIRED, REMAINING_TIME_LIMIT_EXPIRED},
    {FLAG_SHUTDOWN_IMMINENT, SHUTDOWN_IMMINENT},
    {FLAG_NEED_REPLACEMENT, NEED_REPLACEMENT},
    {FLAG_OVERLOAD, OVERLOAD},
    {FLAG_BOOST, BOOST},
    {FLAG_BUCK, BUCK},
};

bool path_ends_with(const std::vector<uint32_t> &path, const Pattern &pattern) {
  if (pattern.size() > path.size())
    return false;
  auto it = path.end() - static_cast<long>(pattern.size());
  for (uint32_t usage : pattern) {
    if (*it++ != usage)
      return false;
  }
  return true;
}

/// Pick the best field for a list of patterns. Among equal matches, prefer the given report type
/// (input reports for flags, since the device pushes them on change; feature reports for values).
const HidField *find_field(const HidDescriptor &descriptor, std::initializer_list<Pattern> patterns,
                           ReportType preferred) {
  for (const auto &pattern : patterns) {
    const HidField *fallback = nullptr;
    for (const auto &field : descriptor.fields()) {
      if (field.type == ReportType::OUTPUT || !path_ends_with(field.path, pattern))
        continue;
      if (field.type == preferred)
        return &field;
      if (fallback == nullptr)
        fallback = &field;
    }
    if (fallback != nullptr)
      return fallback;
  }
  return nullptr;
}

uint16_t report_key(ReportType type, uint8_t id) { return (static_cast<uint16_t>(type) << 8) | id; }

}  // namespace

void NutMapper::bind(const HidDescriptor &descriptor, uint16_t vendor_id, uint16_t product_id) {
  this->values_.clear();
  this->payloads_.clear();
  this->vendor_id_ = vendor_id;
  this->product_id_ = product_id;
  for (const auto &def : VALUE_DEFS) {
    const HidField *field = find_field(descriptor, def.patterns, ReportType::FEATURE);
    if (field == nullptr)
      continue;
    this->values_.push_back(Binding{def.nut_name, def.kind == ValueKind::BEEPER, *field,
                                    quirk_scale(vendor_id, product_id, def.nut_name)});
  }
  for (const auto &def : FLAG_DEFS) {
    const HidField *field =
        find_field(descriptor, {{PRESENT_STATUS, def.usage}, {def.usage}}, ReportType::INPUT);
    this->flags_[def.flag].bound = field != nullptr;
    if (field != nullptr)
      this->flags_[def.flag].field = *field;
  }
}

bool NutMapper::can_report_status() const {
  return this->flags_[FLAG_AC_PRESENT].bound || this->flags_[FLAG_DISCHARGING].bound;
}

size_t NutMapper::bound_count() const {
  size_t count = this->values_.size();
  for (const auto &flag : this->flags_)
    count += flag.bound ? 1 : 0;
  return count;
}

std::vector<HidReport> NutMapper::needed_reports() const {
  std::vector<HidReport> reports;
  auto add = [&reports](const HidField &field) {
    for (const auto &report : reports) {
      if (report.type == field.type && report.id == field.report_id)
        return;
    }
    reports.push_back(HidReport{field.type, field.report_id, 0});
  };
  for (const auto &binding : this->values_)
    add(binding.field);
  for (const auto &flag : this->flags_) {
    if (flag.bound)
      add(flag.field);
  }
  return reports;
}

void NutMapper::update_report(ReportType type, uint8_t id, const uint8_t *payload, size_t len) {
  this->payloads_[report_key(type, id)].assign(payload, payload + len);
}

bool NutMapper::read_field_(const HidField &field, int32_t *out) const {
  auto it = this->payloads_.find(report_key(field.type, field.report_id));
  if (it == this->payloads_.end())
    return false;
  return hid_extract(field, it->second.data(), it->second.size(), out);
}

void NutMapper::build(std::map<std::string, std::string> *vars) const {
  for (const auto &binding : this->values_) {
    int32_t raw;
    if (!this->read_field_(binding.field, &raw))
      continue;
    if (binding.beeper) {
      // AudibleAlarmControl: 1 = disabled, 2 = enabled, 3 = muted
      const char *state = raw == 1 ? "disabled" : raw == 2 ? "enabled" : raw == 3 ? "muted" : nullptr;
      if (state != nullptr)
        (*vars)[binding.nut_name] = state;
      continue;
    }
    (*vars)[binding.nut_name] = format_nut_number(hid_to_physical(binding.field, raw) * binding.scale);
  }

  bool have[FLAG_COUNT] = {};
  bool on[FLAG_COUNT] = {};
  for (uint8_t i = 0; i < FLAG_COUNT; i++) {
    int32_t raw;
    if (this->flags_[i].bound && this->read_field_(this->flags_[i].field, &raw)) {
      have[i] = true;
      on[i] = raw != 0;
    }
  }

  std::string status;
  auto add = [&status](const char *token) {
    if (!status.empty())
      status += ' ';
    status += token;
  };
  if (have[FLAG_AC_PRESENT]) {
    add(on[FLAG_AC_PRESENT] ? "OL" : "OB");
  } else if (have[FLAG_DISCHARGING]) {
    add(on[FLAG_DISCHARGING] ? "OB" : "OL");
  } else {
    // Without a power source flag there is no meaningful status yet.
    vars->erase("ups.status");
    apply_value_quirks(this->vendor_id_, this->product_id_, vars);
    return;
  }
  if (on[FLAG_CHARGING])
    add("CHRG");
  if (on[FLAG_DISCHARGING])
    add("DISCHRG");
  if (on[FLAG_BELOW_CAPACITY_LIMIT] || on[FLAG_TIME_LIMIT_EXPIRED] || on[FLAG_SHUTDOWN_IMMINENT])
    add("LB");
  if (on[FLAG_NEED_REPLACEMENT])
    add("RB");
  if (on[FLAG_OVERLOAD])
    add("OVER");
  if (on[FLAG_BOOST])
    add("BOOST");
  if (on[FLAG_BUCK])
    add("TRIM");
  (*vars)["ups.status"] = status;

  apply_value_quirks(this->vendor_id_, this->product_id_, vars);
}

std::vector<std::string> NutMapper::describe() const {
  std::vector<std::string> lines;
  for (const auto &binding : this->values_) {
    const HidField &f = binding.field;
    char line[160];
    int32_t raw;
    const bool have = this->read_field_(f, &raw);
    std::snprintf(line, sizeof(line), "%s: %s report %u, bits %u+%u, unit 0x%08X, exponent %d, raw %s -> %s",
                  binding.nut_name, f.type == ReportType::INPUT ? "input" : "feature", f.report_id, f.bit_offset,
                  f.bit_size, static_cast<unsigned>(f.unit), f.unit_exponent,
                  have ? std::to_string(raw).c_str() : "?",
                  have ? format_nut_number(hid_to_physical(f, raw) * binding.scale).c_str() : "?");
    lines.emplace_back(line);
  }
  return lines;
}

std::string format_nut_number(double value) {
  if (std::isnan(value) || std::isinf(value))
    return "0";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", value);
  std::string text = buf;
  // Trim trailing zeros, then a dangling decimal point
  while (!text.empty() && text.back() == '0')
    text.pop_back();
  if (!text.empty() && text.back() == '.')
    text.pop_back();
  if (text == "-0")
    text = "0";
  return text;
}

std::string describe_status(const std::string &status) {
  struct Word {
    const char *token;
    const char *text;
  };
  static const Word WORDS[] = {
      {"FSD", "forced shutdown in progress"},
      {"OL", "on wall power"},
      {"OB", "on battery"},
      {"CHRG", "charging"},
      {"DISCHRG", "discharging"},
      {"LB", "battery low"},
      {"RB", "battery needs replacement"},
      {"OVER", "overloaded"},
      {"BOOST", "boosting low wall voltage"},
      {"TRIM", "trimming high wall voltage"},
  };
  std::string result;
  size_t pos = 0;
  while (pos < status.size()) {
    size_t end = status.find(' ', pos);
    if (end == std::string::npos)
      end = status.size();
    std::string token = status.substr(pos, end - pos);
    pos = end + 1;
    if (token.empty())
      continue;
    const char *text = nullptr;
    for (const auto &word : WORDS) {
      if (token == word.token) {
        text = word.text;
        break;
      }
    }
    if (!result.empty())
      result += ", ";
    result += text != nullptr ? text : token;
  }
  if (result.empty())
    return "Unknown";
  if (result[0] >= 'a' && result[0] <= 'z')
    result[0] = static_cast<char>(result[0] - 'a' + 'A');
  return result;
}

}  // namespace esphome::ups_hid
