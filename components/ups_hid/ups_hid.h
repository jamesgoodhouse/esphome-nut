#pragma once

// Only chips with a USB OTG host controller; the Python config validation explains this to the user.
#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)

#include <atomic>
#include <string>
#include <vector>

#include "esphome/components/ups_common/ups_data.h"
#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif

#include "hid_parser.h"
#include "nut_mapping.h"

namespace esphome::ups_hid {

static constexpr const char *DRIVER_VERSION = "0.1.0";

class UpsHidBeeperSwitch;

/// Reads a USB HID Power Device (UPS) and exposes it as NUT variables and ESPHome entities.
///
/// Threading: USB transfer callbacks run on the usb_host task. They only copy data and set flags;
/// all parsing, state changes and publishing happen in loop() on the main task.
class UpsHid : public usb_host::USBClient, public ups_common::UpsSource {
#ifdef USE_SENSOR
  SUB_SENSOR(battery_charge)
  SUB_SENSOR(battery_runtime_remaining)
  SUB_SENSOR(battery_voltage)
  SUB_SENSOR(wall_voltage)
  SUB_SENSOR(output_voltage)
  SUB_SENSOR(wall_frequency)
  SUB_SENSOR(load_percent)
#endif
#ifdef USE_BINARY_SENSOR
  SUB_BINARY_SENSOR(ups_connected)
  SUB_BINARY_SENSOR(on_battery)
  SUB_BINARY_SENSOR(charging)
  SUB_BINARY_SENSOR(low_battery)
  SUB_BINARY_SENSOR(battery_needs_replacement)
  SUB_BINARY_SENSOR(overloaded)
#endif
#ifdef USE_TEXT_SENSOR
  SUB_TEXT_SENSOR(status)
  SUB_TEXT_SENSOR(status_code)
  SUB_TEXT_SENSOR(manufacturer)
  SUB_TEXT_SENSOR(model)
  SUB_TEXT_SENSOR(serial_number)
  SUB_TEXT_SENSOR(self_test_result)
#endif

 public:
  // VID/PID 0 matches any device: the UPS is recognised by its HID descriptor instead.
  UpsHid() : USBClient(0, 0) {}

  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_update_interval(uint32_t update_interval_ms) { this->update_interval_ms_ = update_interval_ms; }
  /// Allow changing UPS settings and running commands (beeper, battery test). Off by default.
  void set_allow_control(bool allow_control) { this->allow_control_ = allow_control; }
#ifdef USE_SWITCH
  void set_beeper_switch(UpsHidBeeperSwitch *beeper_switch) { this->beeper_switch_ = beeper_switch; }
#endif

  const ups_common::UpsSnapshot &get_ups_snapshot() const override { return this->snapshot_; }
  /// Queue a NUT instant command (e.g. "beeper.disable"). Returns false if control is not allowed,
  /// no UPS is connected, or the UPS does not support the command.
  bool run_command(const std::string &name) override;

  /// Fires with the NUT status code (e.g. "OB DISCHRG") whenever it changes; empty when no UPS is connected.
  Trigger<std::string> *get_status_change_trigger() { return &this->status_change_trigger_; }

  // Called from the usb_host task
  void on_control_complete(const usb_transfer_t *transfer);
  void on_interrupt_complete(usb_transfer_t *transfer);

 protected:
  enum class Phase : uint8_t {
    NO_DEVICE,
    READING_DESCRIPTOR,
    POLLING,
    IDLE,
    WRITING,
    REJECTED,
  };

  static constexpr size_t MAX_REPORT_DESCRIPTOR = 4096;
  static constexpr size_t INTERRUPT_BUFFER_SIZE = 64;
  static constexpr size_t MAX_PENDING_INPUT_REPORTS = 8;
  static constexpr uint32_t CONTROL_TIMEOUT_MS = 5000;
  static constexpr uint8_t MAX_REPORT_FAILURES = 3;
  static constexpr size_t MAX_PENDING_WRITES = 4;
  /// Some UPSes (e.g. Tripp Lite) keep reporting the old value for a few seconds after a write
  static constexpr uint32_t WRITE_SETTLE_MS = 5000;

  struct PolledReport {
    HidReport report;
    uint8_t failures;
  };
  struct PendingWrite {
    std::string command;
    HidReport report;
    std::vector<uint8_t> payload;
  };
  struct InputReport {
    uint8_t length;
    uint8_t data[INTERRUPT_BUFFER_SIZE];
  };

  void on_connected() override;
  void on_disconnected() override;
  bool reports_connection_itself() const override { return true; }

  bool find_hid_interface_();
  void read_identity_();
  /// Start a control transfer. For OUT requests, out_data holds `length` bytes to send.
  bool submit_control_(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, uint16_t length,
                       const uint8_t *out_data = nullptr);
  void start_write_();
  void start_interrupt_();
  void handle_control_result_();
  void handle_descriptor_(const uint8_t *data, size_t len);
  void log_diagnostics_();
  void handle_report_(bool success, const uint8_t *data, size_t len);
  void poll_next_report_();
  void drain_input_reports_();
  void finish_poll_round_();
  void reject_device_(const char *reason);
  void rebuild_snapshot_();
  void publish_entities_(bool include_measurements);
  void publish_disconnected_();
  uint32_t stale_after_ms_() const;

  uint32_t update_interval_ms_{15000};
  bool allow_control_{false};

  // Device being served
  Phase phase_{Phase::NO_DEVICE};
  /// Bumped on every connect/disconnect so late USB completions for an old device are ignored
  uint32_t device_generation_{0};
  bool descriptor_requested_{false};
  uint8_t interface_number_{0};
  uint8_t interrupt_ep_{0};
  uint16_t interrupt_mps_{0};
  uint16_t report_descriptor_length_{0};
  bool interface_claimed_{false};
  uint16_t vendor_id_{0};
  uint16_t product_id_{0};
  std::string manufacturer_;
  std::string product_;
  std::string serial_;

  HidDescriptor descriptor_;
  /// Kept for the diagnostics in dump_config()
  std::vector<uint8_t> raw_descriptor_;
  NutMapper mapper_;
  std::vector<PolledReport> polled_reports_;
  size_t poll_index_{0};
  bool round_had_data_{false};
  uint32_t last_poll_start_ms_{0};
  std::vector<PendingWrite> pending_writes_;
  PendingWrite current_write_;
  uint32_t last_data_ms_{0};
  bool have_data_{false};

  // Control transfer (one in flight at a time)
  usb_transfer_t *control_transfer_{nullptr};
  std::atomic<bool> control_busy_{false};
  std::atomic<bool> control_done_{false};
  uint32_t control_generation_{0};
  uint32_t control_started_ms_{0};
  bool control_timeout_logged_{false};

  // Interrupt IN transfer, resubmitted from the usb_host task while active
  usb_transfer_t *interrupt_transfer_{nullptr};
  std::atomic<bool> interrupt_active_{false};
  std::atomic<bool> interrupt_in_flight_{false};
  Mutex input_lock_;
  InputReport pending_inputs_[MAX_PENDING_INPUT_REPORTS]{};
  size_t pending_input_count_{0};
  uint32_t dropped_inputs_{0};

  ups_common::UpsSnapshot snapshot_;
  std::string last_status_;
  std::string last_test_result_;
#ifdef USE_SWITCH
  UpsHidBeeperSwitch *beeper_switch_{nullptr};
#endif

  Trigger<std::string> status_change_trigger_;
  std::string notified_status_;
  bool status_notified_{false};
};

#ifdef USE_SWITCH
/// On = beeper enabled, off = disabled. Shows the state read back from the UPS.
class UpsHidBeeperSwitch : public switch_::Switch, public Parented<UpsHid> {
 protected:
  void write_state(bool state) override;
};
#endif

#ifdef USE_BUTTON
/// Runs one NUT instant command on the UPS when pressed.
class UpsHidCommandButton : public button::Button, public Parented<UpsHid> {
 public:
  void set_command(const char *command) { this->command_ = command; }

 protected:
  void press_action() override;
  const char *command_{""};
};
#endif

}  // namespace esphome::ups_hid

#endif
