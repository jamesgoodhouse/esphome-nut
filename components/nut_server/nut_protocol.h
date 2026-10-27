#pragma once

// NUT network protocol (upsd side). Read-only apart from the instant commands the UPS source offers.
// Pure C++ (no ESPHome / ESP-IDF dependencies) so it can be unit-tested on the host. Reference:
// https://networkupstools.org/docs/developer-guide.chunked/net-protocol.html

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "esphome/components/ups_common/ups_data.h"

namespace esphome::nut_server {

using ups_common::UpsSnapshot;

static constexpr const char *NUT_SERVER_VERSION = "0.1.0";

struct NutUser {
  std::string username;
  std::string password;
  /// Primary users may claim PRIMARY and set FSD (the machine that tells others to shut down).
  bool primary;
  /// May run instant commands (beeper, battery test) with INSTCMD.
  bool allow_commands;
};

/// Per-connection state.
struct NutSession {
  /// Client address, for LIST CLIENT and logs.
  std::string peer;
  std::string username;
  std::string password;
  bool username_set{false};
  bool password_set{false};
  bool logged_in{false};
  bool primary{false};
  /// Set when the client sent LOGOUT; the server closes the connection after replying.
  bool close_requested{false};
};

class NutProtocol {
 public:
  /// FSD clears once the UPS has been on wall power for this long.
  static constexpr uint32_t FSD_CLEAR_AFTER_MS = 5 * 60 * 1000;

  void set_ups_name(const std::string &name) { this->ups_name_ = name; }
  const std::string &get_ups_name() const { return this->ups_name_; }
  void set_ups_description(const std::string &description) { this->ups_description_ = description; }
  void set_require_login(bool require_login) { this->require_login_ = require_login; }
  void add_user(const std::string &username, const std::string &password, bool primary,
                bool allow_commands = false) {
    this->users_.push_back(NutUser{username, password, primary, allow_commands});
  }
  /// Runs an instant command on the UPS; returns false if it could not be started.
  void set_command_handler(std::function<bool(const std::string &)> handler) {
    this->command_handler_ = std::move(handler);
  }
  const std::vector<NutUser> &get_users() const { return this->users_; }

  void open_session(NutSession *session);
  void close_session(NutSession *session);

  /// Handle one command line (without the trailing newline). Returns the full reply, newline-terminated.
  std::string handle_line(NutSession &session, const std::string &line, const UpsSnapshot &snapshot,
                          uint32_t now_ms);

  /// Track UPS power state so a latched FSD can clear once wall power is back. Call regularly.
  void observe(const UpsSnapshot &snapshot, uint32_t now_ms);

  bool fsd_active() const { return this->fsd_; }
  /// ups.status as clients see it (FSD prepended while a forced shutdown is latched).
  std::string effective_status(const UpsSnapshot &snapshot) const;

 protected:
  std::string handle_get_(NutSession &session, const std::vector<std::string> &args, const UpsSnapshot &snapshot);
  std::string handle_list_(NutSession &session, const std::vector<std::string> &args, const UpsSnapshot &snapshot);
  std::string handle_instcmd_(NutSession &session, const std::vector<std::string> &args,
                              const UpsSnapshot &snapshot);
  const NutUser *authenticated_user_(const NutSession &session) const;
  /// Empty if the UPS data can be served, otherwise the error reply.
  std::string availability_error_(const UpsSnapshot &snapshot) const;
  std::string description_(const UpsSnapshot &snapshot) const;
  bool lookup_var_(const UpsSnapshot &snapshot, const std::string &name, std::string *value) const;
  size_t num_logins_() const;

  std::string ups_name_{"ups"};
  std::string ups_description_;
  bool require_login_{false};
  std::vector<NutUser> users_;
  std::function<bool(const std::string &)> command_handler_;
  std::vector<NutSession *> sessions_;
  bool fsd_{false};
  uint32_t fsd_set_ms_{0};
  bool on_wall_power_{false};
  uint32_t wall_power_since_ms_{0};
};

/// Split a NUT command line into words, honouring double quotes and backslash escapes.
std::vector<std::string> nut_tokenize(const std::string &line);
/// Quote a value for a NUT reply: wrap in double quotes, escaping backslashes and quotes.
std::string nut_quote(const std::string &value);

}  // namespace esphome::nut_server
