// Tests for the HID report descriptor parser, the HID -> NUT mapping and vendor quirks.

#include <dirent.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "descriptor_builder.h"
#include "esphome/components/ups_hid/hid_parser.h"
#include "esphome/components/ups_hid/nut_mapping.h"
#include "esphome/components/ups_hid/quirks.h"
#include "test_framework.h"

using esphome::ups_hid::describe_status;
using esphome::ups_hid::format_nut_number;
using esphome::ups_hid::HidDescriptor;
using esphome::ups_hid::NutMapper;
using esphome::ups_hid::ReportType;
using test::DescriptorBuilder;

namespace {

/// Modelled on APC Back-UPS descriptors: values under UPS.PowerSummary, flags in an input report.
std::vector<uint8_t> apc_style_descriptor() {
  DescriptorBuilder d;
  d.usage_page(0x84).usage(0x04).collection(0x01);  // UPS, application
  {
    d.report_id(1).usage(0x24).collection(0x00);  // PowerSummary, physical
    {
      d.usage_page(0x85).logical_min(0).logical_max(100).report_size(8).report_count(1).unit(0).unit_exponent(0);
      d.feature_field(0x66);  // RemainingCapacity
      d.feature_field(0x29);  // RemainingCapacityLimit
      d.report_id(2).logical_max(65535).report_size(16).unit(test::UNIT_SECOND);
      d.feature_field(0x68);  // RunTimeToEmpty
      d.report_id(3).usage_page(0x84).unit(test::UNIT_VOLT).unit_exponent(5);
      d.feature_field(0x30);  // Voltage (centivolts)
      d.feature_field(0x40);  // ConfigVoltage
      d.report_id(4).usage(0x02).collection(0x02);  // PresentStatus, logical
      {
        d.usage_page(0x85).logical_min(0).logical_max(1).report_size(1).report_count(1).unit(0).unit_exponent(0);
        d.usage(0xD0).input();  // ACPresent
        d.usage(0x44).input();  // Charging
        d.usage(0x45).input();  // Discharging
        d.usage(0x42).input();  // BelowRemainingCapacityLimit
        d.usage_page(0x84).usage(0x65).input();  // Overload
        d.report_count(3).input(0x03);           // padding
      }
      d.end_collection();
    }
    d.end_collection();
    d.report_id(5).usage_page(0x84).usage(0x1A).collection(0x00);  // Input
    {
      d.unit(test::UNIT_VOLT).unit_exponent(7).logical_min(0).logical_max(255).report_size(8).report_count(1);
      d.feature_field(0x30);  // Voltage
      d.unit(test::UNIT_HERTZ).unit_exponent(0);
      d.feature_field(0x32);  // Frequency
    }
    d.end_collection();
    d.usage(0x1C).collection(0x00);  // Output
    {
      d.unit(0).feature_field(0x35);  // PercentLoad
    }
    d.end_collection();
  }
  d.end_collection();
  return d.bytes;
}

/// Modelled on CyberPower descriptors: values under UPS.Battery, no report-ID-less fields.
std::vector<uint8_t> cyberpower_style_descriptor() {
  DescriptorBuilder d;
  d.usage_page(0x84).usage(0x04).collection(0x01);
  {
    d.report_id(1).usage(0x12).collection(0x00);  // Battery
    {
      d.unit(test::UNIT_VOLT).unit_exponent(5).logical_min(0).logical_max(65535).report_size(16).report_count(1);
      d.feature_field(0x30);  // Voltage
      d.feature_field(0x40);  // ConfigVoltage
      d.usage_page(0x85).unit(0).unit_exponent(0).logical_max(100).report_size(8);
      d.feature_field(0x66);  // RemainingCapacity
    }
    d.end_collection();
    d.report_id(2).usage_page(0x84).usage(0x02).collection(0x02);  // PresentStatus
    {
      d.usage_page(0x85).logical_max(1).report_size(1).report_count(1);
      d.usage(0xD0).feature();  // ACPresent
      d.usage(0x45).feature();  // Discharging
      d.report_count(6).feature(0x03);
    }
    d.end_collection();
  }
  d.end_collection();
  return d.bytes;
}

void put16(std::vector<uint8_t> &buf, size_t offset, uint16_t value) {
  buf[offset] = value & 0xFF;
  buf[offset + 1] = value >> 8;
}

std::map<std::string, std::string> feed_apc(NutMapper &mapper, uint8_t status_bits) {
  mapper.update_report(ReportType::FEATURE, 1, std::vector<uint8_t>{87, 10}.data(), 2);
  std::vector<uint8_t> runtime(2);
  put16(runtime, 0, 1520);
  mapper.update_report(ReportType::FEATURE, 2, runtime.data(), runtime.size());
  std::vector<uint8_t> voltage(4);
  put16(voltage, 0, 1365);
  put16(voltage, 2, 1200);
  mapper.update_report(ReportType::FEATURE, 3, voltage.data(), voltage.size());
  mapper.update_report(ReportType::INPUT, 4, &status_bits, 1);
  mapper.update_report(ReportType::FEATURE, 5, std::vector<uint8_t>{120, 60, 23}.data(), 3);
  std::map<std::string, std::string> vars;
  mapper.build(&vars);
  return vars;
}

}  // namespace

TEST(parser_reads_apc_style_layout) {
  auto bytes = apc_style_descriptor();
  HidDescriptor desc;
  std::string error;
  CHECK(desc.parse(bytes.data(), bytes.size(), &error));
  CHECK_EQ(error, std::string());
  CHECK(desc.uses_report_ids());
  CHECK(desc.has_usage_page(0x84));
  CHECK(desc.has_usage_page(0x85));
  CHECK_EQ(desc.fields().size(), size_t(13));

  const auto *status = desc.find_report(ReportType::INPUT, 4);
  CHECK(status != nullptr);
  if (status != nullptr)
    CHECK_EQ(status->byte_length(), size_t(1));
  const auto *voltages = desc.find_report(ReportType::FEATURE, 3);
  CHECK(voltages != nullptr);
  if (voltages != nullptr)
    CHECK_EQ(voltages->byte_length(), size_t(4));
}

TEST(parser_treats_logical_max_as_unsigned_when_min_is_non_negative) {
  auto bytes = apc_style_descriptor();
  HidDescriptor desc;
  CHECK(desc.parse(bytes.data(), bytes.size()));
  bool found = false;
  for (const auto &field : desc.fields()) {
    if (field.report_id == 5 && field.bit_offset == 0) {
      CHECK_EQ(field.logical_max, 255);  // encoded as one byte 0xFF
      found = true;
    }
  }
  CHECK(found);
}

TEST(parser_rejects_truncated_and_unbalanced_descriptors) {
  auto bytes = apc_style_descriptor();
  HidDescriptor desc;
  std::string error;
  CHECK(!desc.parse(bytes.data(), bytes.size() - 1, &error));  // missing final end collection
  CHECK(!error.empty());
  const uint8_t truncated[] = {0x05};  // usage page with no data byte
  CHECK(!desc.parse(truncated, sizeof(truncated), &error));
}

TEST(parser_keeps_constant_fields_with_usages_but_not_padding) {
  DescriptorBuilder d;
  d.usage_page(0x84).usage(0x04).collection(0x01);
  d.report_id(1).logical_min(0).logical_max(255).report_size(8).report_count(1);
  d.usage(0x40).feature(0x03);       // ConfigVoltage, constant: a real read-only value
  d.report_count(2).feature(0x03);   // padding: constant, no usage
  d.report_count(1).usage(0x30).feature(0x02);  // Voltage, data
  d.end_collection();
  HidDescriptor desc;
  CHECK(desc.parse(d.bytes.data(), d.bytes.size()));
  CHECK_EQ(desc.fields().size(), size_t(2));
  if (desc.fields().size() == 2) {
    CHECK_EQ(desc.fields()[0].usage(), uint32_t(0x840040));
    CHECK_EQ(desc.fields()[0].bit_offset, uint16_t(0));
    CHECK_EQ(desc.fields()[1].usage(), uint32_t(0x840030));
    CHECK_EQ(desc.fields()[1].bit_offset, uint16_t(24));  // after 8 bits of value and 16 of padding
  }
}

TEST(extract_handles_bit_fields_and_signed_values) {
  esphome::ups_hid::HidField field{};
  field.bit_offset = 3;
  field.bit_size = 4;
  field.logical_min = 0;
  int32_t value = 0;
  const uint8_t payload[] = {0b01011000};
  CHECK(esphome::ups_hid::hid_extract(field, payload, 1, &value));
  CHECK_EQ(value, 11);

  field.bit_offset = 0;
  field.bit_size = 8;
  field.logical_min = -128;
  const uint8_t negative[] = {0xF6};
  CHECK(esphome::ups_hid::hid_extract(field, negative, 1, &value));
  CHECK_EQ(value, -10);

  field.bit_size = 16;
  CHECK(!esphome::ups_hid::hid_extract(field, negative, 1, &value));  // payload too short
}

TEST(mapping_builds_nut_variables_on_wall_power) {
  auto bytes = apc_style_descriptor();
  HidDescriptor desc;
  CHECK(desc.parse(bytes.data(), bytes.size()));
  NutMapper mapper;
  mapper.bind(desc, 0x051D, 0x0002);
  CHECK(mapper.can_report_status());
  CHECK_EQ(mapper.needed_reports().size(), size_t(5));

  auto vars = feed_apc(mapper, 0b00011);  // ACPresent + Charging
  CHECK_EQ(vars["battery.charge"], std::string("87"));
  CHECK_EQ(vars["battery.charge.low"], std::string("10"));
  CHECK_EQ(vars["battery.runtime"], std::string("1520"));
  CHECK_EQ(vars["battery.voltage"], std::string("13.65"));
  CHECK_EQ(vars["battery.voltage.nominal"], std::string("12"));
  CHECK_EQ(vars["input.voltage"], std::string("120"));
  CHECK_EQ(vars["input.frequency"], std::string("60"));
  CHECK_EQ(vars["ups.load"], std::string("23"));
  CHECK_EQ(vars["ups.status"], std::string("OL CHRG"));
}

TEST(mapping_reports_on_battery_low_and_overload) {
  auto bytes = apc_style_descriptor();
  HidDescriptor desc;
  CHECK(desc.parse(bytes.data(), bytes.size()));
  NutMapper mapper;
  mapper.bind(desc, 0x051D, 0x0002);
  auto vars = feed_apc(mapper, 0b11100);  // Discharging + BelowCapacityLimit + Overload, no AC
  CHECK_EQ(vars["ups.status"], std::string("OB DISCHRG LB OVER"));
}

TEST(mapping_has_no_status_until_flags_are_read) {
  auto bytes = apc_style_descriptor();
  HidDescriptor desc;
  CHECK(desc.parse(bytes.data(), bytes.size()));
  NutMapper mapper;
  mapper.bind(desc, 0x051D, 0x0002);
  mapper.update_report(ReportType::FEATURE, 1, std::vector<uint8_t>{50, 10}.data(), 2);
  std::map<std::string, std::string> vars;
  mapper.build(&vars);
  CHECK_EQ(vars.count("ups.status"), size_t(0));
  CHECK_EQ(vars["battery.charge"], std::string("50"));
}

TEST(non_ups_device_is_not_reportable) {
  // A keyboard: Generic Desktop page, Keyboard usage
  DescriptorBuilder d;
  d.usage_page(0x01).usage(0x06).collection(0x01);
  d.usage_page(0x07).logical_min(0).logical_max(1).report_size(1).report_count(8);
  d.item(2, 0x1, 0xE0).item(2, 0x2, 0xE7).input();
  d.end_collection();
  HidDescriptor desc;
  CHECK(desc.parse(d.bytes.data(), d.bytes.size()));
  CHECK(!desc.has_usage_page(0x84));
  NutMapper mapper;
  mapper.bind(desc, 0x046D, 0xC31C);
  CHECK(!mapper.can_report_status());
  CHECK_EQ(mapper.bound_count(), size_t(0));
}

TEST(cyberpower_battery_voltage_quirk_rescales_inflated_voltage) {
  auto bytes = cyberpower_style_descriptor();
  HidDescriptor desc;
  CHECK(desc.parse(bytes.data(), bytes.size()));
  NutMapper mapper;
  mapper.bind(desc, esphome::ups_hid::VENDOR_CYBERPOWER, 0x0501);
  std::vector<uint8_t> battery(5);
  put16(battery, 0, 2040);  // reports 20.4 V for a 13.6 V battery
  put16(battery, 2, 1200);
  battery[4] = 100;
  mapper.update_report(ReportType::FEATURE, 1, battery.data(), battery.size());
  const uint8_t status = 0b01;  // ACPresent
  mapper.update_report(ReportType::FEATURE, 2, &status, 1);
  std::map<std::string, std::string> vars;
  mapper.build(&vars);
  CHECK_EQ(vars["battery.voltage"], std::string("13.6"));
  CHECK_EQ(vars["battery.charge"], std::string("100"));
  CHECK_EQ(vars["ups.status"], std::string("OL"));

  // Same data from a different vendor is left alone
  mapper.bind(desc, 0x1234, 0x0001);
  mapper.update_report(ReportType::FEATURE, 1, battery.data(), battery.size());
  mapper.update_report(ReportType::FEATURE, 2, &status, 1);
  vars.clear();
  mapper.build(&vars);
  CHECK_EQ(vars["battery.voltage"], std::string("20.4"));
}

TEST(describe_lists_scaling_for_each_value) {
  auto bytes = apc_style_descriptor();
  HidDescriptor desc;
  CHECK(desc.parse(bytes.data(), bytes.size()));
  NutMapper mapper;
  mapper.bind(desc, 0x051D, 0x0002);
  feed_apc(mapper, 0b00011);
  bool found = false;
  for (const auto &line : mapper.describe()) {
    if (line.rfind("battery.voltage:", 0) == 0) {
      CHECK_EQ(line, std::string("battery.voltage: feature report 3, bits 0+16, unit 0x00F0D121, exponent 5, "
                                 "raw 1365 -> 13.65"));
      found = true;
    }
  }
  CHECK(found);
}

TEST(tripp_lite_eco850lcd_exponent_quirk) {
  // Real descriptor and raw values from an ECO850LCD whose display showed about 120 V in, 60 Hz
  std::ifstream file(std::string(FIXTURE_DIR) + "/tripplite_eco850lcd_09ae_3024.hex");
  std::vector<uint8_t> bytes;
  std::string token;
  while (file >> token)
    bytes.push_back(static_cast<uint8_t>(std::stoul(token, nullptr, 16)));
  CHECK_EQ(bytes.size(), size_t(885));
  HidDescriptor desc;
  CHECK(desc.parse(bytes.data(), bytes.size()));
  NutMapper mapper;
  mapper.bind(desc, esphome::ups_hid::VENDOR_TRIPP_LITE, 0x3024);
  CHECK(mapper.can_report_status());

  auto feed16 = [&mapper](uint8_t id, uint16_t value) {
    std::vector<uint8_t> payload(2);
    put16(payload, 0, value);
    mapper.update_report(ReportType::FEATURE, id, payload.data(), payload.size());
  };
  feed16(0x20, 136);   // battery voltage, exponent 1
  feed16(0x04, 12);    // nominal battery voltage, exponent 7
  feed16(0x31, 1195);  // input voltage
  feed16(0x19, 602);   // input frequency
  feed16(0x1B, 1184);  // output voltage
  feed16(0x47, 76);    // real power, exponent 7
  feed16(0x03, 850);   // VA rating, a "constant" feature field
  const uint8_t input_nominal = 120;  // input nominal voltage, a "constant" 8-bit feature field
  mapper.update_report(ReportType::FEATURE, 0x30, &input_nominal, 1);

  std::map<std::string, std::string> vars;
  mapper.build(&vars);
  CHECK_EQ(vars["ups.power.nominal"], std::string("850"));
  CHECK_EQ(vars["input.voltage.nominal"], std::string("120"));
  CHECK_EQ(vars["battery.voltage"], std::string("13.6"));
  CHECK_EQ(vars["battery.voltage.nominal"], std::string("12"));
  CHECK_EQ(vars["input.voltage"], std::string("119.5"));
  CHECK_EQ(vars["input.frequency"], std::string("60.2"));
  CHECK_EQ(vars["output.voltage"], std::string("118.4"));
  CHECK_EQ(vars["ups.realpower"], std::string("76"));
}

TEST(tripp_lite_commands_write_beeper_and_test_fields) {
  std::ifstream file(std::string(FIXTURE_DIR) + "/tripplite_eco850lcd_09ae_3024.hex");
  std::vector<uint8_t> bytes;
  std::string token;
  while (file >> token)
    bytes.push_back(static_cast<uint8_t>(std::stoul(token, nullptr, 16)));
  HidDescriptor desc;
  CHECK(desc.parse(bytes.data(), bytes.size()));
  NutMapper mapper;
  mapper.bind(desc, esphome::ups_hid::VENDOR_TRIPP_LITE, 0x3024);

  auto commands = mapper.commands();
  CHECK_EQ(commands.size(), size_t(5));
  CHECK(std::find(commands.begin(), commands.end(), "beeper.disable") != commands.end());
  CHECK(std::find(commands.begin(), commands.end(), "test.battery.start.quick") != commands.end());

  // Both control reports are polled, since writes start from the last read
  bool polls_beeper = false, polls_test = false;
  for (const auto &report : mapper.needed_reports()) {
    polls_beeper |= report.type == ReportType::FEATURE && report.id == 0x11;
    polls_test |= report.type == ReportType::FEATURE && report.id == 0x10;
  }
  CHECK(polls_beeper);
  CHECK(polls_test);

  esphome::ups_hid::HidReport report{};
  std::vector<uint8_t> payload;
  CHECK(!mapper.build_command("beeper.enable", &report, &payload));  // not read yet

  const uint8_t muted = 3, no_test = 6;
  mapper.update_report(ReportType::FEATURE, 0x11, &muted, 1);
  mapper.update_report(ReportType::FEATURE, 0x10, &no_test, 1);
  std::map<std::string, std::string> vars;
  mapper.build(&vars);
  CHECK_EQ(vars["ups.beeper.status"], std::string("muted"));
  CHECK_EQ(vars["ups.test.result"], std::string("No test initiated"));

  CHECK(mapper.build_command("beeper.enable", &report, &payload));
  CHECK_EQ(report.id, uint8_t(0x11));
  CHECK(report.type == ReportType::FEATURE);
  CHECK_EQ(payload.size(), size_t(1));
  if (!payload.empty())
    CHECK_EQ(payload[0], uint8_t(2));

  CHECK(mapper.build_command("test.battery.start.quick", &report, &payload));
  CHECK_EQ(report.id, uint8_t(0x10));
  if (!payload.empty())
    CHECK_EQ(payload[0], uint8_t(1));

  CHECK(!mapper.build_command("load.off", &report, &payload));
}

TEST(insert_preserves_neighbouring_bits) {
  esphome::ups_hid::HidField field{};
  field.bit_offset = 4;
  field.bit_size = 3;
  uint8_t payload[] = {0xFF};
  CHECK(esphome::ups_hid::hid_insert(field, payload, 1, 2));
  CHECK_EQ(int(payload[0]), int(0b10101111));
  int32_t value = 0;
  CHECK(esphome::ups_hid::hid_extract(field, payload, 1, &value));
  CHECK_EQ(value, 2);
}

TEST(number_formatting_matches_nut_style) {
  CHECK_EQ(format_nut_number(100.0), std::string("100"));
  CHECK_EQ(format_nut_number(13.65), std::string("13.65"));
  CHECK_EQ(format_nut_number(230.10), std::string("230.1"));
  CHECK_EQ(format_nut_number(-0.001), std::string("0"));
}

TEST(status_descriptions_are_plain_english) {
  CHECK_EQ(describe_status("OL CHRG"), std::string("On wall power, charging"));
  CHECK_EQ(describe_status("FSD OB DISCHRG LB"),
           std::string("Forced shutdown in progress, on battery, discharging, battery low"));
  CHECK_EQ(describe_status(""), std::string("Unknown"));
}

TEST(fixture_descriptors_parse_and_report_status) {
  // Real descriptor dumps (hex bytes, whitespace separated) contributed from devices.
  DIR *dir = opendir(FIXTURE_DIR);
  if (dir == nullptr)
    return;
  while (dirent *entry = readdir(dir)) {
    std::string name = entry->d_name;
    if (name.size() < 4 || name.substr(name.size() - 4) != ".hex")
      continue;
    std::ifstream file(std::string(FIXTURE_DIR) + "/" + name);
    std::vector<uint8_t> bytes;
    std::string token;
    while (file >> token)
      bytes.push_back(static_cast<uint8_t>(std::stoul(token, nullptr, 16)));
    HidDescriptor desc;
    std::string error;
    const bool parsed = desc.parse(bytes.data(), bytes.size(), &error);
    if (!parsed)
      std::printf("  fixture %s: %s\n", name.c_str(), error.c_str());
    CHECK(parsed);
    NutMapper mapper;
    mapper.bind(desc, 0, 0);
    CHECK(mapper.can_report_status());
  }
  closedir(dir);
}

int main() { return test::run_all(); }
