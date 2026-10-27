#pragma once

// HID usage IDs from the USB HID "Usage Tables for HID Power Devices" (pages 0x84 and 0x85).
// Values are full 32-bit usages: (usage page << 16) | usage id.

#include <cstdint>

namespace esphome::ups_hid::usage {

constexpr uint16_t PAGE_POWER_DEVICE = 0x84;
constexpr uint16_t PAGE_BATTERY_SYSTEM = 0x85;

constexpr uint32_t pd(uint16_t id) { return (uint32_t(PAGE_POWER_DEVICE) << 16) | id; }
constexpr uint32_t bs(uint16_t id) { return (uint32_t(PAGE_BATTERY_SYSTEM) << 16) | id; }

// Power Device page (0x84): collections
constexpr uint32_t PRESENT_STATUS = pd(0x02);
constexpr uint32_t UPS = pd(0x04);
constexpr uint32_t BATTERY_SYSTEM = pd(0x10);
constexpr uint32_t BATTERY = pd(0x12);
constexpr uint32_t POWER_CONVERTER = pd(0x16);
constexpr uint32_t INPUT = pd(0x1A);
constexpr uint32_t OUTPUT = pd(0x1C);
constexpr uint32_t POWER_SUMMARY = pd(0x24);

// Power Device page (0x84): measurements and configuration
constexpr uint32_t VOLTAGE = pd(0x30);
constexpr uint32_t CURRENT = pd(0x31);
constexpr uint32_t FREQUENCY = pd(0x32);
constexpr uint32_t APPARENT_POWER = pd(0x33);
constexpr uint32_t ACTIVE_POWER = pd(0x34);
constexpr uint32_t PERCENT_LOAD = pd(0x35);
constexpr uint32_t TEMPERATURE = pd(0x36);
constexpr uint32_t CONFIG_VOLTAGE = pd(0x40);
constexpr uint32_t CONFIG_FREQUENCY = pd(0x42);
constexpr uint32_t CONFIG_APPARENT_POWER = pd(0x43);
constexpr uint32_t CONFIG_ACTIVE_POWER = pd(0x44);
constexpr uint32_t TEST = pd(0x58);
constexpr uint32_t AUDIBLE_ALARM_CONTROL = pd(0x5A);

// Power Device page (0x84): status flags
constexpr uint32_t OVERLOAD = pd(0x65);
constexpr uint32_t SHUTDOWN_IMMINENT = pd(0x69);
constexpr uint32_t BOOST = pd(0x6E);
constexpr uint32_t BUCK = pd(0x6F);

// Battery System page (0x85)
constexpr uint32_t REMAINING_CAPACITY_LIMIT = bs(0x29);
constexpr uint32_t REMAINING_TIME_LIMIT = bs(0x2A);
constexpr uint32_t BELOW_REMAINING_CAPACITY_LIMIT = bs(0x42);
constexpr uint32_t REMAINING_TIME_LIMIT_EXPIRED = bs(0x43);
constexpr uint32_t CHARGING = bs(0x44);
constexpr uint32_t DISCHARGING = bs(0x45);
constexpr uint32_t NEED_REPLACEMENT = bs(0x4B);
constexpr uint32_t REMAINING_CAPACITY = bs(0x66);
constexpr uint32_t RUN_TIME_TO_EMPTY = bs(0x68);
constexpr uint32_t AC_PRESENT = bs(0xD0);

}  // namespace esphome::ups_hid::usage
