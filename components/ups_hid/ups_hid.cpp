#include "ups_hid.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)

#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "hid_usages.h"

namespace esphome::ups_hid {

static const char *const TAG = "ups_hid";

// USB HID class requests and descriptor types
static constexpr uint8_t USB_CLASS_HID_CODE = 0x03;
static constexpr uint8_t HID_DESCRIPTOR_TYPE = 0x21;
static constexpr uint8_t HID_REPORT_DESCRIPTOR_TYPE = 0x22;
static constexpr uint8_t REQUEST_GET_DESCRIPTOR = 0x06;
static constexpr uint8_t REQUEST_GET_REPORT = 0x01;
static constexpr uint8_t REQUEST_TYPE_STANDARD_INTERFACE_IN = 0x81;
static constexpr uint8_t REQUEST_TYPE_CLASS_INTERFACE_IN = 0xA1;
static constexpr uint16_t DEFAULT_REPORT_DESCRIPTOR_LENGTH = 1024;
static constexpr uint32_t MIN_STALE_AFTER_MS = 20000;

static void control_transfer_cb(usb_transfer_t *transfer) {
  static_cast<UpsHid *>(transfer->context)->on_control_complete(transfer);
}

static void interrupt_transfer_cb(usb_transfer_t *transfer) {
  static_cast<UpsHid *>(transfer->context)->on_interrupt_complete(transfer);
}

static std::string trimmed(const char *text) {
  std::string out = text;
  while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
    out.pop_back();
  return out;
}

static bool has_status_token(const std::string &status, const char *token) {
  size_t pos = 0;
  const size_t token_len = std::strlen(token);
  while (pos < status.size()) {
    size_t end = status.find(' ', pos);
    if (end == std::string::npos)
      end = status.size();
    if (end - pos == token_len && status.compare(pos, token_len, token) == 0)
      return true;
    pos = end + 1;
  }
  return false;
}

void UpsHid::setup() {
  USBClient::setup();
  if (this->is_failed())
    return;
  if (usb_host_transfer_alloc(MAX_REPORT_DESCRIPTOR + sizeof(usb_setup_packet_t), 0, &this->control_transfer_) !=
          ESP_OK ||
      usb_host_transfer_alloc(INTERRUPT_BUFFER_SIZE, 0, &this->interrupt_transfer_) != ESP_OK) {
    ESP_LOGE(TAG, "Could not allocate USB transfer buffers");
    this->mark_failed();
    return;
  }
  this->status_set_warning(LOG_STR("No UPS connected"));
  this->publish_disconnected_();
}

void UpsHid::dump_config() {
  ESP_LOGCONFIG(TAG,
                "UPS (USB HID):\n"
                "  Update interval: %" PRIu32 " ms",
                this->update_interval_ms_);
  if (this->snapshot_.connected) {
    ESP_LOGCONFIG(TAG, "  Connected: %04X:%04X %s %s", this->vendor_id_, this->product_id_,
                  this->manufacturer_.c_str(), this->product_.c_str());
  } else {
    ESP_LOGCONFIG(TAG, "  Connected: no");
  }
  // dump_config is replayed whenever a log viewer connects, so the dump is always reachable
  this->log_diagnostics_();
}

// ---- usb_host task context -------------------------------------------------------------

void UpsHid::on_control_complete(const usb_transfer_t *transfer) {
  (void) transfer;
  this->control_done_.store(true, std::memory_order_release);
  this->enable_loop_soon_any_context();
  App.wake_loop_threadsafe();
}

void UpsHid::on_interrupt_complete(usb_transfer_t *transfer) {
  if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes > 0) {
    this->input_lock_.lock();
    if (this->pending_input_count_ < MAX_PENDING_INPUT_REPORTS) {
      auto &slot = this->pending_inputs_[this->pending_input_count_++];
      slot.length = static_cast<uint8_t>(std::min<int>(transfer->actual_num_bytes, INTERRUPT_BUFFER_SIZE));
      std::memcpy(slot.data, transfer->data_buffer, slot.length);
    } else {
      this->dropped_inputs_++;
    }
    this->input_lock_.unlock();
  }
  if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && this->interrupt_active_.load()) {
    if (usb_host_transfer_submit(transfer) == ESP_OK)
      return;  // still in flight
  }
  this->interrupt_active_.store(false);
  this->interrupt_in_flight_.store(false);
  App.wake_loop_threadsafe();
}

// ---- main loop context ------------------------------------------------------------------

void UpsHid::on_connected() {
  this->device_generation_++;
  const usb_device_desc_t *device_desc;
  if (usb_host_get_device_descriptor(this->device_handle_, &device_desc) == ESP_OK) {
    this->vendor_id_ = device_desc->idVendor;
    this->product_id_ = device_desc->idProduct;
  }
  this->read_identity_();
  ESP_LOGI(TAG, "USB device %04X:%04X connected (%s %s)", this->vendor_id_, this->product_id_,
           this->manufacturer_.c_str(), this->product_.c_str());

  if (!this->find_hid_interface_()) {
    this->reject_device_("it is not a USB HID device, so it cannot be a supported UPS");
    return;
  }
  if (usb_host_interface_claim(this->handle_, this->device_handle_, this->interface_number_, 0) != ESP_OK) {
    this->reject_device_("its HID interface could not be claimed");
    return;
  }
  this->interface_claimed_ = true;
  this->phase_ = Phase::READING_DESCRIPTOR;
  this->descriptor_requested_ = false;
}

void UpsHid::on_disconnected() {
  this->interrupt_active_.store(false);
  if (this->interface_claimed_) {
    if (this->interrupt_ep_ != 0) {
      usb_host_endpoint_halt(this->device_handle_, this->interrupt_ep_);
      usb_host_endpoint_flush(this->device_handle_, this->interrupt_ep_);
    }
    usb_host_interface_release(this->handle_, this->device_handle_, this->interface_number_);
    this->interface_claimed_ = false;
  }
  if (this->phase_ != Phase::NO_DEVICE && this->phase_ != Phase::REJECTED)
    ESP_LOGW(TAG, "UPS disconnected");

  this->device_generation_++;
  this->phase_ = Phase::NO_DEVICE;
  this->interrupt_ep_ = 0;
  this->interrupt_mps_ = 0;
  this->report_descriptor_length_ = 0;
  this->polled_reports_.clear();
  this->raw_descriptor_.clear();
  this->mapper_.clear_reports();
  this->have_data_ = false;
  this->snapshot_ = ups_common::UpsSnapshot{};
  this->last_status_.clear();
  this->status_set_warning(LOG_STR("No UPS connected"));
  this->publish_disconnected_();
  USBClient::on_disconnected();
}

bool UpsHid::find_hid_interface_() {
  const usb_config_desc_t *config;
  if (usb_host_get_active_config_descriptor(this->device_handle_, &config) != ESP_OK)
    return false;
  bool found = false;
  bool in_hid = false;
  int offset = 0;
  const auto *desc = reinterpret_cast<const usb_standard_desc_t *>(config);
  while ((desc = usb_parse_next_descriptor(desc, config->wTotalLength, &offset)) != nullptr) {
    if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      if (found)
        break;  // only the first HID interface is used
      const auto *intf = reinterpret_cast<const usb_intf_desc_t *>(desc);
      in_hid = intf->bInterfaceClass == USB_CLASS_HID_CODE && intf->bAlternateSetting == 0;
      if (in_hid) {
        this->interface_number_ = intf->bInterfaceNumber;
        found = true;
      }
    } else if (desc->bDescriptorType == HID_DESCRIPTOR_TYPE && in_hid && desc->bLength >= 9) {
      // HID descriptor: bcdHID(2) bCountryCode bNumDescriptors, then (bDescriptorType, wLength) pairs
      const auto *raw = reinterpret_cast<const uint8_t *>(desc);
      const uint8_t count = raw[5];
      for (uint8_t i = 0; i < count && 6 + 3 * i + 2 < desc->bLength; i++) {
        if (raw[6 + 3 * i] == HID_REPORT_DESCRIPTOR_TYPE)
          this->report_descriptor_length_ = raw[7 + 3 * i] | (raw[8 + 3 * i] << 8);
      }
    } else if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && in_hid) {
      const auto *ep = reinterpret_cast<const usb_ep_desc_t *>(desc);
      if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_INT &&
          (ep->bEndpointAddress & 0x80) != 0 && this->interrupt_ep_ == 0) {
        this->interrupt_ep_ = ep->bEndpointAddress;
        this->interrupt_mps_ = ep->wMaxPacketSize & 0x7FF;
      }
    }
  }
  return found;
}

void UpsHid::read_identity_() {
  this->manufacturer_.clear();
  this->product_.clear();
  this->serial_.clear();
  usb_device_info_t info;
  if (usb_host_device_info(this->device_handle_, &info) != ESP_OK)
    return;
  char buf[usb_host::DESC_STRING_BUF_SIZE];
  if (usb_host::copy_descriptor_string(info.str_desc_manufacturer, buf))
    this->manufacturer_ = trimmed(buf);
  if (usb_host::copy_descriptor_string(info.str_desc_product, buf))
    this->product_ = trimmed(buf);
  if (usb_host::copy_descriptor_string(info.str_desc_serial_num, buf))
    this->serial_ = trimmed(buf);
}

void UpsHid::reject_device_(const char *reason) {
  ESP_LOGE(TAG,
           "USB device %04X:%04X (%s %s) is not supported: %s.\n"
           "  This component works with UPSes that follow the USB HID Power Device standard,\n"
           "  which covers most APC, CyberPower, Eaton and similar consumer UPSes.",
           this->vendor_id_, this->product_id_, this->manufacturer_.c_str(), this->product_.c_str(), reason);
  this->phase_ = Phase::REJECTED;  // so on_disconnected() doesn't report a lost UPS
  this->disconnect();
  this->phase_ = Phase::REJECTED;
  this->status_set_error(LOG_STR("Connected USB device is not a supported UPS"));
}

bool UpsHid::submit_control_(uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                             uint16_t length) {
  if (this->control_busy_.load())
    return false;
  auto *setup = reinterpret_cast<usb_setup_packet_t *>(this->control_transfer_->data_buffer);
  setup->bmRequestType = request_type;
  setup->bRequest = request;
  setup->wValue = value;
  setup->wIndex = index;
  setup->wLength = length;
  this->control_transfer_->num_bytes = sizeof(usb_setup_packet_t) + length;
  this->control_transfer_->device_handle = this->device_handle_;
  this->control_transfer_->bEndpointAddress = 0;
  this->control_transfer_->callback = control_transfer_cb;
  this->control_transfer_->context = this;
  this->control_done_.store(false);
  this->control_busy_.store(true);
  this->control_generation_ = this->device_generation_;
  this->control_started_ms_ = millis();
  this->control_timeout_logged_ = false;
  esp_err_t err = usb_host_transfer_submit_control(this->handle_, this->control_transfer_);
  if (err != ESP_OK) {
    this->control_busy_.store(false);
    ESP_LOGW(TAG, "USB control request failed to start: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

void UpsHid::start_interrupt_() {
  if (this->interrupt_ep_ == 0 || this->interrupt_in_flight_.load())
    return;
  const uint16_t mps = std::min<uint16_t>(this->interrupt_mps_, INTERRUPT_BUFFER_SIZE);
  if (mps == 0)
    return;
  this->interrupt_transfer_->device_handle = this->device_handle_;
  this->interrupt_transfer_->bEndpointAddress = this->interrupt_ep_;
  this->interrupt_transfer_->num_bytes = mps;
  this->interrupt_transfer_->callback = interrupt_transfer_cb;
  this->interrupt_transfer_->context = this;
  this->interrupt_active_.store(true);
  this->interrupt_in_flight_.store(true);
  if (usb_host_transfer_submit(this->interrupt_transfer_) != ESP_OK) {
    this->interrupt_active_.store(false);
    this->interrupt_in_flight_.store(false);
    ESP_LOGD(TAG, "Interrupt endpoint not available; relying on polling only");
  }
}

void UpsHid::loop() {
  this->process_usb_events_();

  if (this->control_done_.load(std::memory_order_acquire)) {
    this->control_done_.store(false);
    this->control_busy_.store(false);
    if (this->control_generation_ == this->device_generation_)
      this->handle_control_result_();
  } else if (this->control_busy_.load() && !this->control_timeout_logged_ &&
             millis() - this->control_started_ms_ > CONTROL_TIMEOUT_MS) {
    this->control_timeout_logged_ = true;
    ESP_LOGW(TAG, "UPS is not answering USB requests");
  }

  if (this->phase_ == Phase::READING_DESCRIPTOR && !this->descriptor_requested_ && !this->control_busy_.load()) {
    uint16_t length = this->report_descriptor_length_ != 0 ? this->report_descriptor_length_
                                                           : DEFAULT_REPORT_DESCRIPTOR_LENGTH;
    length = std::min<uint16_t>(length, MAX_REPORT_DESCRIPTOR);
    this->descriptor_requested_ = this->submit_control_(REQUEST_TYPE_STANDARD_INTERFACE_IN, REQUEST_GET_DESCRIPTOR,
                                                        HID_REPORT_DESCRIPTOR_TYPE << 8, this->interface_number_,
                                                        length);
  }

  this->drain_input_reports_();

  const uint32_t now = millis();
  if (this->phase_ == Phase::IDLE && now - this->last_poll_start_ms_ >= this->update_interval_ms_) {
    this->phase_ = Phase::POLLING;
    this->poll_index_ = 0;
    this->round_had_data_ = false;
    this->last_poll_start_ms_ = now;
    this->poll_next_report_();
  }

  // Notify automations of status changes. Done from loop() so the first "no UPS" notification happens
  // after every component (e.g. a status LED) has finished setup.
  static const std::string NO_STATUS;
  auto it = this->snapshot_.vars.find("ups.status");
  const std::string &status = this->snapshot_.connected && it != this->snapshot_.vars.end() ? it->second : NO_STATUS;
  if (!this->status_notified_ || status != this->notified_status_) {
    this->status_notified_ = true;
    this->notified_status_ = status;
    this->status_change_trigger_.trigger(status);
  }

  if (this->snapshot_.connected) {
    const bool stale = now - this->last_data_ms_ > this->stale_after_ms_();
    if (stale != this->snapshot_.stale) {
      this->snapshot_.stale = stale;
      if (stale) {
        ESP_LOGW(TAG, "No fresh data from the UPS for %" PRIu32 " s", (now - this->last_data_ms_) / 1000);
        this->status_set_warning(LOG_STR("UPS stopped responding"));
      } else {
        ESP_LOGI(TAG, "UPS is responding again");
        this->status_clear_warning();
      }
      this->publish_entities_(false);
    }
  }
}

void UpsHid::handle_control_result_() {
  const bool ok = this->control_transfer_->status == USB_TRANSFER_STATUS_COMPLETED;
  const uint8_t *data = this->control_transfer_->data_buffer + sizeof(usb_setup_packet_t);
  // For control transfers, actual_num_bytes includes the setup packet
  const int actual = this->control_transfer_->actual_num_bytes - static_cast<int>(sizeof(usb_setup_packet_t));
  const size_t len = actual > 0 ? static_cast<size_t>(actual) : 0;

  switch (this->phase_) {
    case Phase::READING_DESCRIPTOR:
      if (!ok || len == 0) {
        this->reject_device_("its HID report descriptor could not be read");
        return;
      }
      this->handle_descriptor_(data, len);
      break;
    case Phase::POLLING:
      this->handle_report_(ok, data, len);
      break;
    default:
      break;
  }
}

void UpsHid::log_diagnostics_() {
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  // Shared by users when asking for support of a new UPS
  if (this->raw_descriptor_.empty())
    return;
  const size_t len = this->raw_descriptor_.size();
  ESP_LOGV(TAG, "Report descriptor dump (%04X:%04X, %u bytes):", this->vendor_id_, this->product_id_,
           static_cast<unsigned>(len));
  for (size_t i = 0; i < len; i += 32) {
    char line[32 * 3 + 1];
    size_t pos = 0;
    for (size_t j = i; j < len && j < i + 32; j++)
      pos += snprintf(line + pos, sizeof(line) - pos, "%02x ", this->raw_descriptor_[j]);
    line[pos > 0 ? pos - 1 : 0] = '\0';
    ESP_LOGV(TAG, "  %s", line);
  }
  ESP_LOGV(TAG, "Values read from the UPS:");
  for (const auto &line : this->mapper_.describe())
    ESP_LOGV(TAG, "  %s", line.c_str());
#endif
}

void UpsHid::handle_descriptor_(const uint8_t *data, size_t len) {
  ESP_LOGD(TAG, "Report descriptor: %u bytes", static_cast<unsigned>(len));
  this->raw_descriptor_.assign(data, data + len);

  std::string error;
  if (!this->descriptor_.parse(data, len, &error)) {
    ESP_LOGE(TAG, "Report descriptor parse error: %s", error.c_str());
    this->reject_device_("its HID report descriptor could not be understood");
    return;
  }
  if (!this->descriptor_.has_usage_page(usage::PAGE_POWER_DEVICE) &&
      !this->descriptor_.has_usage_page(usage::PAGE_BATTERY_SYSTEM)) {
    this->reject_device_("it is a HID device (like a keyboard or mouse) but has no UPS / power device data");
    return;
  }
  this->mapper_.bind(this->descriptor_, this->vendor_id_, this->product_id_);
  if (!this->mapper_.can_report_status()) {
    this->reject_device_(
        "it looks like a UPS but does not report whether it is on wall power in the standard way. "
        "Please open an issue and include the report descriptor dump (set the logger level to VERBOSE)");
    return;
  }

  this->polled_reports_.clear();
  for (const auto &needed : this->mapper_.needed_reports()) {
    const HidReport *report = this->descriptor_.find_report(needed.type, needed.id);
    if (report != nullptr)
      this->polled_reports_.push_back(PolledReport{*report, 0});
  }
  ESP_LOGI(TAG, "Recognised UPS: %u values and status flags from %u reports", (unsigned) this->mapper_.bound_count(),
           (unsigned) this->polled_reports_.size());

  this->start_interrupt_();
  this->phase_ = Phase::POLLING;
  this->poll_index_ = 0;
  this->round_had_data_ = false;
  this->last_poll_start_ms_ = millis();
  this->poll_next_report_();
}

void UpsHid::poll_next_report_() {
  while (this->poll_index_ < this->polled_reports_.size() &&
         this->polled_reports_[this->poll_index_].failures >= MAX_REPORT_FAILURES)
    this->poll_index_++;
  if (this->poll_index_ >= this->polled_reports_.size()) {
    this->finish_poll_round_();
    return;
  }
  const HidReport &report = this->polled_reports_[this->poll_index_].report;
  const uint16_t length = report.byte_length() + (this->descriptor_.uses_report_ids() ? 1 : 0);
  if (!this->submit_control_(REQUEST_TYPE_CLASS_INTERFACE_IN, REQUEST_GET_REPORT,
                             (static_cast<uint16_t>(report.type) << 8) | report.id, this->interface_number_, length)) {
    this->finish_poll_round_();  // try again next interval
  }
}

void UpsHid::handle_report_(bool success, const uint8_t *data, size_t len) {
  if (this->poll_index_ >= this->polled_reports_.size())
    return;
  auto &polled = this->polled_reports_[this->poll_index_];
  if (success && len > 0) {
    if (this->descriptor_.uses_report_ids() && data[0] == polled.report.id) {
      data++;
      len--;
    }
    this->mapper_.update_report(polled.report.type, polled.report.id, data, len);
    polled.failures = 0;
    this->round_had_data_ = true;
  } else if (++polled.failures == MAX_REPORT_FAILURES) {
    ESP_LOGD(TAG, "UPS does not answer report %u (type %u); no longer asking for it", polled.report.id,
             static_cast<unsigned>(polled.report.type));
  }
  this->poll_index_++;
  this->poll_next_report_();
}

void UpsHid::finish_poll_round_() {
  this->phase_ = Phase::IDLE;
  if (this->round_had_data_) {
    this->last_data_ms_ = millis();
    this->have_data_ = true;
  } else if (!this->have_data_) {
    ESP_LOGW(TAG, "UPS did not answer any data requests; will keep trying");
  }
  this->rebuild_snapshot_();
  this->publish_entities_(true);
}

void UpsHid::drain_input_reports_() {
  InputReport reports[MAX_PENDING_INPUT_REPORTS];
  size_t count;
  uint32_t dropped;
  this->input_lock_.lock();
  count = this->pending_input_count_;
  std::memcpy(reports, this->pending_inputs_, count * sizeof(InputReport));
  this->pending_input_count_ = 0;
  dropped = this->dropped_inputs_;
  this->dropped_inputs_ = 0;
  this->input_lock_.unlock();

  if (dropped > 0)
    ESP_LOGD(TAG, "Dropped %" PRIu32 " UPS status updates", dropped);
  if (count == 0 || (this->phase_ != Phase::POLLING && this->phase_ != Phase::IDLE))
    return;
  for (size_t i = 0; i < count; i++) {
    const uint8_t *data = reports[i].data;
    size_t len = reports[i].length;
    uint8_t id = 0;
    if (this->descriptor_.uses_report_ids()) {
      id = data[0];
      data++;
      len--;
    }
    this->mapper_.update_report(ReportType::INPUT, id, data, len);
  }
  this->last_data_ms_ = millis();
  this->have_data_ = true;
  this->rebuild_snapshot_();
  this->publish_entities_(false);
}

void UpsHid::rebuild_snapshot_() {
  auto &vars = this->snapshot_.vars;
  vars.clear();
  this->mapper_.build(&vars);
  const bool has_status = vars.count("ups.status") != 0;

  char id[8];
  snprintf(id, sizeof(id), "%04x", this->vendor_id_);
  vars["ups.vendorid"] = id;
  snprintf(id, sizeof(id), "%04x", this->product_id_);
  vars["ups.productid"] = id;
  if (!this->manufacturer_.empty()) {
    vars["ups.mfr"] = this->manufacturer_;
    vars["device.mfr"] = this->manufacturer_;
  }
  if (!this->product_.empty()) {
    vars["ups.model"] = this->product_;
    vars["device.model"] = this->product_;
  }
  if (!this->serial_.empty()) {
    vars["ups.serial"] = this->serial_;
    vars["device.serial"] = this->serial_;
  }
  vars["device.type"] = "ups";
  vars["driver.name"] = "esphome-ups_hid";
  vars["driver.version"] = DRIVER_VERSION;

  if (has_status && !this->snapshot_.connected) {
    this->snapshot_.connected = true;
    this->snapshot_.stale = false;
    ESP_LOGI(TAG, "UPS ready: %s %s, status: %s", this->manufacturer_.c_str(), this->product_.c_str(),
             vars["ups.status"].c_str());
    this->status_clear_warning();
    this->status_clear_error();
    this->report_connected_();
#ifdef USE_TEXT_SENSOR
    if (this->manufacturer_text_sensor_ != nullptr)
      this->manufacturer_text_sensor_->publish_state(this->manufacturer_);
    if (this->model_text_sensor_ != nullptr)
      this->model_text_sensor_->publish_state(this->product_);
    if (this->serial_number_text_sensor_ != nullptr)
      this->serial_number_text_sensor_->publish_state(this->serial_);
#endif
  }
}

uint32_t UpsHid::stale_after_ms_() const { return std::max<uint32_t>(this->update_interval_ms_ * 4, MIN_STALE_AFTER_MS); }

void UpsHid::publish_entities_(bool include_measurements) {
  if (!this->snapshot_.connected)
    return;
  const auto &vars = this->snapshot_.vars;
  auto it = vars.find("ups.status");
  const std::string status = it != vars.end() ? it->second : "";

#ifdef USE_SENSOR
  if (include_measurements) {
    auto publish = [&vars](sensor::Sensor *sensor, const char *name) {
      if (sensor == nullptr)
        return;
      auto var = vars.find(name);
      sensor->publish_state(var != vars.end() ? strtof(var->second.c_str(), nullptr) : NAN);
    };
    publish(this->battery_charge_sensor_, "battery.charge");
    publish(this->battery_runtime_remaining_sensor_, "battery.runtime");
    publish(this->battery_voltage_sensor_, "battery.voltage");
    publish(this->wall_voltage_sensor_, "input.voltage");
    publish(this->output_voltage_sensor_, "output.voltage");
    publish(this->wall_frequency_sensor_, "input.frequency");
    publish(this->load_percent_sensor_, "ups.load");
  }
#endif

#ifdef USE_BINARY_SENSOR
  if (this->ups_connected_binary_sensor_ != nullptr)
    this->ups_connected_binary_sensor_->publish_state(true);
  auto flag = [&status](binary_sensor::BinarySensor *sensor, const char *token) {
    if (sensor != nullptr)
      sensor->publish_state(has_status_token(status, token));
  };
  flag(this->on_battery_binary_sensor_, "OB");
  flag(this->charging_binary_sensor_, "CHRG");
  flag(this->low_battery_binary_sensor_, "LB");
  flag(this->battery_needs_replacement_binary_sensor_, "RB");
  flag(this->overloaded_binary_sensor_, "OVER");
#endif

#ifdef USE_TEXT_SENSOR
  const std::string shown = this->snapshot_.stale ? std::string("STALE") : status;
  if (shown != this->last_status_) {
    this->last_status_ = shown;
    if (this->status_text_sensor_ != nullptr)
      this->status_text_sensor_->publish_state(this->snapshot_.stale ? std::string("UPS not responding")
                                                                     : describe_status(status));
    if (this->status_code_text_sensor_ != nullptr)
      this->status_code_text_sensor_->publish_state(status);
  }
#endif
}

void UpsHid::publish_disconnected_() {
#ifdef USE_SENSOR
  for (auto *sensor : {this->battery_charge_sensor_, this->battery_runtime_remaining_sensor_,
                       this->battery_voltage_sensor_, this->wall_voltage_sensor_, this->output_voltage_sensor_,
                       this->wall_frequency_sensor_, this->load_percent_sensor_}) {
    if (sensor != nullptr)
      sensor->publish_state(NAN);
  }
#endif
#ifdef USE_BINARY_SENSOR
  if (this->ups_connected_binary_sensor_ != nullptr)
    this->ups_connected_binary_sensor_->publish_state(false);
#endif
#ifdef USE_TEXT_SENSOR
  if (this->status_text_sensor_ != nullptr)
    this->status_text_sensor_->publish_state("No UPS connected");
  if (this->status_code_text_sensor_ != nullptr)
    this->status_code_text_sensor_->publish_state("");
#endif
}

}  // namespace esphome::ups_hid

#endif
