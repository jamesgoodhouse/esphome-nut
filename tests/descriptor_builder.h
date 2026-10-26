#pragma once

// Builds HID report descriptors item by item, for tests.

#include <cstdint>
#include <vector>

namespace test {

class DescriptorBuilder {
 public:
  DescriptorBuilder &item(uint8_t type, uint8_t tag, int64_t value, int size = -1) {
    if (size < 0) {
      if (value >= -128 && value <= 255)
        size = 1;
      else if (value >= -32768 && value <= 65535)
        size = 2;
      else
        size = 4;
    }
    const uint8_t size_code = size == 4 ? 3 : static_cast<uint8_t>(size);
    bytes.push_back(static_cast<uint8_t>((tag << 4) | (type << 2) | size_code));
    for (int i = 0; i < size; i++)
      bytes.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
    return *this;
  }

  // Global items
  DescriptorBuilder &usage_page(uint16_t page) { return item(1, 0x0, page); }
  DescriptorBuilder &logical_min(int32_t v) { return item(1, 0x1, v, v >= -128 && v <= 127 ? 1 : 2); }
  DescriptorBuilder &logical_max(int32_t v) { return item(1, 0x2, v); }
  DescriptorBuilder &unit_exponent(int8_t v) { return item(1, 0x5, v & 0x0F, 1); }
  DescriptorBuilder &unit(uint32_t v) { return item(1, 0x6, v, v > 0xFFFF ? 4 : (v > 0xFF ? 2 : 1)); }
  DescriptorBuilder &report_size(uint8_t v) { return item(1, 0x7, v); }
  DescriptorBuilder &report_id(uint8_t v) { return item(1, 0x8, v); }
  DescriptorBuilder &report_count(uint8_t v) { return item(1, 0x9, v); }

  // Local items
  DescriptorBuilder &usage(uint16_t id) { return item(2, 0x0, id, id > 0xFF ? 2 : 1); }

  // Main items
  DescriptorBuilder &collection(uint8_t kind) { return item(0, 0xA, kind); }
  DescriptorBuilder &end_collection() {
    bytes.push_back(0xC0);
    return *this;
  }
  DescriptorBuilder &input(uint8_t flags = 0x02) { return item(0, 0x8, flags); }
  DescriptorBuilder &feature(uint8_t flags = 0x02) { return item(0, 0xB, flags); }

  /// Shorthand: a usage followed by a single feature field
  DescriptorBuilder &feature_field(uint16_t id) { return usage(id).feature(); }

  std::vector<uint8_t> bytes;
};

constexpr uint32_t UNIT_VOLT = 0x00F0D121;
constexpr uint32_t UNIT_SECOND = 0x1001;
constexpr uint32_t UNIT_HERTZ = 0xF001;

}  // namespace test
