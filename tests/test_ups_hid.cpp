// Tests for the HID report descriptor parser, the HID -> NUT mapping and vendor quirks.

#include <dirent.h>

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
