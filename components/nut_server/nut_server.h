#pragma once

#include <memory>
#include <string>
#include <vector>

#include "esphome/components/socket/socket.h"
#include "esphome/components/ups_common/ups_data.h"
#include "esphome/core/component.h"

#include "nut_protocol.h"

namespace esphome::nut_server {

/// A read-only NUT server (upsd) on TCP port 3493, serving one UPS to NUT clients such as
/// Synology DSM, TrueNAS, Linux upsmon and Home Assistant. Runs entirely in the main loop.
class NutServer : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_source(ups_common::UpsSource *source) { this->source_ = source; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_max_connections(uint8_t max_connections) { this->max_connections_ = max_connections; }
  void set_ups_name(const std::string &name) { this->protocol_.set_ups_name(name); }
  void set_ups_description(const std::string &description) { this->protocol_.set_ups_description(description); }
  void set_require_login(bool require_login) {
    this->require_login_ = require_login;
    this->protocol_.set_require_login(require_login);
  }
  void add_user(const std::string &username, const std::string &password, bool primary, bool allow_commands) {
    this->protocol_.add_user(username, password, primary, allow_commands);
  }
  /// Set by code generation when it turned off the api component's reboot-without-Home-Assistant timer.
  void set_api_reboot_disabled(bool disabled) { this->api_reboot_disabled_ = disabled; }

 protected:
  static constexpr size_t MAX_LINE_LENGTH = 512;
  static constexpr uint32_t IDLE_TIMEOUT_MS = 10 * 60 * 1000;
  static constexpr uint32_t LISTEN_RETRY_MS = 5000;

  struct Client {
    std::unique_ptr<socket::Socket> socket;
    NutSession session;
    std::string input;
    std::string output;
    uint32_t last_activity_ms{0};
    bool closing{false};
  };

  bool start_listening_();
  void accept_clients_(uint32_t now);
  void serve_client_(Client &client, const ups_common::UpsSnapshot &snapshot, uint32_t now);
  void flush_client_(Client &client);

  ups_common::UpsSource *source_{nullptr};
  NutProtocol protocol_;
  std::unique_ptr<socket::ListenSocket> listen_;
  // unique_ptr keeps each NutSession at a stable address while it is registered with protocol_
  std::vector<std::unique_ptr<Client>> clients_;
  uint32_t last_listen_attempt_ms_{0};
  uint16_t port_{3493};
  uint8_t max_connections_{4};
  bool require_login_{false};
  bool api_reboot_disabled_{false};
};

}  // namespace esphome::nut_server
