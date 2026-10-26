#include "hid_parser.h"

#include <algorithm>
#include <cmath>

namespace esphome::ups_hid {

namespace {

constexpr size_t MAX_FIELDS = 1024;
constexpr size_t MAX_COLLECTION_DEPTH = 16;
constexpr size_t MAX_GLOBAL_STACK = 8;
constexpr size_t MAX_USAGES_PER_ITEM = 256;

// Item types
constexpr uint8_t TYPE_MAIN = 0;
constexpr uint8_t TYPE_GLOBAL = 1;
constexpr uint8_t TYPE_LOCAL = 2;

// Main item tags
constexpr uint8_t MAIN_INPUT = 0x8;
constexpr uint8_t MAIN_OUTPUT = 0x9;
constexpr uint8_t MAIN_COLLECTION = 0xA;
constexpr uint8_t MAIN_FEATURE = 0xB;
constexpr uint8_t MAIN_END_COLLECTION = 0xC;

// Global item tags
constexpr uint8_t GLOBAL_USAGE_PAGE = 0x0;
constexpr uint8_t GLOBAL_LOGICAL_MIN = 0x1;
constexpr uint8_t GLOBAL_LOGICAL_MAX = 0x2;
constexpr uint8_t GLOBAL_UNIT_EXPONENT = 0x5;
constexpr uint8_t GLOBAL_UNIT = 0x6;
constexpr uint8_t GLOBAL_REPORT_SIZE = 0x7;
constexpr uint8_t GLOBAL_REPORT_ID = 0x8;
constexpr uint8_t GLOBAL_REPORT_COUNT = 0x9;
constexpr uint8_t GLOBAL_PUSH = 0xA;
constexpr uint8_t GLOBAL_POP = 0xB;

// Local item tags
constexpr uint8_t LOCAL_USAGE = 0x0;
constexpr uint8_t LOCAL_USAGE_MIN = 0x1;
constexpr uint8_t LOCAL_USAGE_MAX = 0x2;

// Main item data flags
constexpr uint32_t FLAG_CONSTANT = 0x01;

// HID unit codes, as used by NUT's libhid to fix up exponents
constexpr uint32_t UNIT_VOLT = 0x00F0D121;
constexpr uint32_t UNIT_WATT = 0x0000D121;
constexpr uint32_t UNIT_KELVIN = 0x00010001;

struct GlobalState {
  uint16_t usage_page{0};
  int32_t logical_min{0};
  int32_t logical_max{0};
  uint32_t logical_max_raw{0};
  uint8_t logical_max_size{0};
  int8_t unit_exponent{0};
  uint32_t unit{0};
  uint32_t report_size{0};
  uint8_t report_id{0};
  uint32_t report_count{0};
};

int32_t sign_extend(uint32_t value, uint8_t size_bytes) {
  switch (size_bytes) {
    case 1:
      return static_cast<int8_t>(value);
    case 2:
      return static_cast<int16_t>(value);
    default:
      return static_cast<int32_t>(value);
  }
}

uint16_t report_key(ReportType type, uint8_t id) { return (static_cast<uint16_t>(type) << 8) | id; }

}  // namespace

bool HidDescriptor::parse(const uint8_t *data, size_t len, std::string *error) {
  auto fail = [error](const char *msg) {
    if (error != nullptr)
      *error = msg;
    return false;
  };

  this->fields_.clear();
  this->reports_.clear();
  this->pages_seen_.clear();
  this->uses_report_ids_ = false;

  GlobalState global;
  std::vector<GlobalState> global_stack;
  std::vector<uint32_t> collection_path;
  std::vector<uint32_t> usages;
  bool have_usage_min = false;
  uint32_t usage_min = 0;
  // Bit length accumulated per (type, report id)
  std::vector<std::pair<uint16_t, uint32_t>> bit_lengths;

  auto note_page = [this](uint16_t page) {
    if (std::find(this->pages_seen_.begin(), this->pages_seen_.end(), page) == this->pages_seen_.end())
      this->pages_seen_.push_back(page);
  };
  auto full_usage = [&global](uint32_t value, uint8_t size) {
    return size == 4 ? value : (static_cast<uint32_t>(global.usage_page) << 16) | (value & 0xFFFF);
  };
  auto bits_for = [&bit_lengths](uint16_t key) -> uint32_t & {
    for (auto &entry : bit_lengths) {
      if (entry.first == key)
        return entry.second;
    }
    bit_lengths.emplace_back(key, 0);
    return bit_lengths.back().second;
  };

  size_t pos = 0;
  while (pos < len) {
    const uint8_t prefix = data[pos++];
    if (prefix == 0xFE) {
      // Long item: bDataSize, bLongItemTag, data
      if (pos + 2 > len)
        return fail("truncated long item");
      pos += 2 + data[pos];
      continue;
    }
    uint8_t size = prefix & 0x03;
    if (size == 3)
      size = 4;
    const uint8_t type = (prefix >> 2) & 0x03;
    const uint8_t tag = prefix >> 4;
    if (pos + size > len)
      return fail("truncated item");
    uint32_t value = 0;
    for (uint8_t i = 0; i < size; i++)
      value |= static_cast<uint32_t>(data[pos + i]) << (8 * i);
    pos += size;

    if (type == TYPE_GLOBAL) {
      switch (tag) {
        case GLOBAL_USAGE_PAGE:
          global.usage_page = static_cast<uint16_t>(value);
          break;
        case GLOBAL_LOGICAL_MIN:
          global.logical_min = sign_extend(value, size);
          break;
        case GLOBAL_LOGICAL_MAX:
          global.logical_max_raw = value;
          global.logical_max_size = size;
          break;
        case GLOBAL_UNIT_EXPONENT: {
          // Spec says a 4-bit two's complement nibble; some devices send a sign-extended byte.
          int32_t exp = sign_extend(value, size);
          if (exp >= 8 && exp <= 15)
            exp -= 16;
          global.unit_exponent = static_cast<int8_t>(exp);
          break;
        }
        case GLOBAL_UNIT:
          global.unit = value;
          break;
        case GLOBAL_REPORT_SIZE:
          global.report_size = value;
          break;
        case GLOBAL_REPORT_ID:
          if (value == 0 || value > 255)
            return fail("invalid report ID");
          global.report_id = static_cast<uint8_t>(value);
          this->uses_report_ids_ = true;
          break;
        case GLOBAL_REPORT_COUNT:
          global.report_count = value;
          break;
        case GLOBAL_PUSH:
          if (global_stack.size() >= MAX_GLOBAL_STACK)
            return fail("global stack overflow");
          global_stack.push_back(global);
          break;
        case GLOBAL_POP:
          if (global_stack.empty())
            return fail("global stack underflow");
          global = global_stack.back();
          global_stack.pop_back();
          break;
        default:
          break;  // physical min/max and reserved items are not needed
      }
      continue;
    }

    if (type == TYPE_LOCAL) {
      switch (tag) {
        case LOCAL_USAGE:
          if (usages.size() < MAX_USAGES_PER_ITEM)
            usages.push_back(full_usage(value, size));
          break;
        case LOCAL_USAGE_MIN:
          usage_min = full_usage(value, size);
          have_usage_min = true;
          break;
        case LOCAL_USAGE_MAX: {
          uint32_t usage_max = full_usage(value, size);
          if (have_usage_min && usage_max >= usage_min) {
            for (uint32_t u = usage_min; u <= usage_max && usages.size() < MAX_USAGES_PER_ITEM; u++)
              usages.push_back(u);
          }
          have_usage_min = false;
          break;
        }
        default:
          break;
      }
      continue;
    }

    if (type != TYPE_MAIN)
      continue;  // reserved item type

    switch (tag) {
      case MAIN_COLLECTION: {
        if (collection_path.size() >= MAX_COLLECTION_DEPTH)
          return fail("collections nested too deeply");
        uint32_t usage = usages.empty() ? 0 : usages.front();
        if (usage != 0)
          note_page(static_cast<uint16_t>(usage >> 16));
        collection_path.push_back(usage);
        break;
      }
      case MAIN_END_COLLECTION:
        if (collection_path.empty())
          return fail("unbalanced end collection");
        collection_path.pop_back();
        break;
      case MAIN_INPUT:
      case MAIN_OUTPUT:
      case MAIN_FEATURE: {
        const ReportType rtype = tag == MAIN_INPUT    ? ReportType::INPUT
                                 : tag == MAIN_OUTPUT ? ReportType::OUTPUT
                                                      : ReportType::FEATURE;
        if (global.report_size == 0 || global.report_size > 32) {
          // Fields wider than 32 bits are not used by power devices; skip but keep offsets right.
          if (global.report_size > 32) {
            bits_for(report_key(rtype, global.report_id)) += global.report_size * global.report_count;
          }
          break;
        }
        // Logical max is unsigned when logical min is non-negative (a common descriptor quirk).
        int32_t logical_max = sign_extend(global.logical_max_raw, global.logical_max_size);
        if (logical_max < global.logical_min)
          logical_max = static_cast<int32_t>(global.logical_max_raw);

        uint32_t &bits = bits_for(report_key(rtype, global.report_id));
        const bool constant = (value & FLAG_CONSTANT) != 0;
        for (uint32_t i = 0; i < global.report_count; i++) {
          uint32_t usage = 0;
          if (!usages.empty())
            usage = i < usages.size() ? usages[i] : usages.back();
          if (!constant && usage != 0 && this->fields_.size() < MAX_FIELDS) {
            HidField field{};
            field.type = rtype;
            field.report_id = global.report_id;
            field.bit_offset = static_cast<uint16_t>(bits);
            field.bit_size = static_cast<uint8_t>(global.report_size);
            field.logical_min = global.logical_min;
            field.logical_max = logical_max;
            field.unit_exponent = global.unit_exponent;
            field.unit = global.unit;
            field.path = collection_path;
            field.path.push_back(usage);
            note_page(static_cast<uint16_t>(usage >> 16));
            this->fields_.push_back(std::move(field));
          }
          bits += global.report_size;
        }
        break;
      }
      default:
        break;
    }
    // Local items only apply to the next main item
    usages.clear();
    have_usage_min = false;
  }

  if (!collection_path.empty())
    return fail("unterminated collection");

  for (const auto &entry : bit_lengths) {
    this->reports_.push_back(HidReport{static_cast<ReportType>(entry.first >> 8),
                                       static_cast<uint8_t>(entry.first & 0xFF),
                                       static_cast<uint16_t>(entry.second)});
  }
  return true;
}

bool HidDescriptor::has_usage_page(uint16_t page) const {
  return std::find(this->pages_seen_.begin(), this->pages_seen_.end(), page) != this->pages_seen_.end();
}

const HidReport *HidDescriptor::find_report(ReportType type, uint8_t id) const {
  for (const auto &report : this->reports_) {
    if (report.type == type && report.id == id)
      return &report;
  }
  return nullptr;
}

bool hid_extract(const HidField &field, const uint8_t *payload, size_t len, int32_t *out) {
  const uint32_t end_bit = static_cast<uint32_t>(field.bit_offset) + field.bit_size;
  if (field.bit_size == 0 || (end_bit + 7) / 8 > len)
    return false;
  uint32_t raw = 0;
  for (uint8_t i = 0; i < field.bit_size; i++) {
    const uint32_t bit = field.bit_offset + i;
    if (payload[bit / 8] & (1u << (bit % 8)))
      raw |= 1u << i;
  }
  if (field.logical_min < 0 && field.bit_size < 32 && (raw & (1u << (field.bit_size - 1))))
    raw |= ~((1u << field.bit_size) - 1);  // sign-extend
  *out = static_cast<int32_t>(raw);
  return true;
}

double hid_to_physical(const HidField &field, int32_t logical) {
  int exponent = field.unit_exponent;
  switch (field.unit) {
    case UNIT_VOLT:
    case UNIT_WATT:
      // SI units in HID are built from cm and grams, so volts and watts carry an extra 10^7.
      exponent -= 7;
      break;
    case 0:
      // Unitless values (percentages, flags) ignore the exponent, as NUT does.
      exponent = 0;
      break;
    default:
      break;
  }
  double value = static_cast<double>(logical) * std::pow(10.0, exponent);
  if (field.unit == UNIT_KELVIN)
    value -= 273.15;
  return value;
}

}  // namespace esphome::ups_hid
