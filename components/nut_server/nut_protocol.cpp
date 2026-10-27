#include "nut_protocol.h"

#include <algorithm>
#include <cctype>

namespace esphome::nut_server {

namespace {

std::string upper(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::toupper(c); });
  return text;
}

std::string err(const char *code) { return std::string("ERR ") + code + "\n"; }

bool has_token(const std::string &status, const char *token) {
  size_t pos = 0;
  const size_t token_len = std::char_traits<char>::length(token);
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

struct VarDesc {
  const char *name;
  const char *desc;
};

// Descriptions for GET DESC, from NUT's cmdvartab
const VarDesc VAR_DESCS[] = {
    {"battery.charge", "Battery charge (percent of full)"},
    {"battery.charge.low", "Remaining battery level when UPS switches to LB (percent)"},
    {"battery.runtime", "Battery runtime (seconds)"},
    {"battery.runtime.low", "Remaining battery runtime when UPS switches to LB (seconds)"},
    {"battery.voltage", "Battery voltage (V)"},
    {"battery.voltage.nominal", "Nominal battery voltage (V)"},
    {"battery.temperature", "Battery temperature (degrees C)"},
    {"input.voltage", "Input voltage (V)"},
    {"input.voltage.nominal", "Nominal input voltage (V)"},
    {"input.frequency", "Input line frequency (Hz)"},
    {"output.voltage", "Output voltage (V)"},
    {"output.voltage.nominal", "Nominal output voltage (V)"},
    {"output.frequency", "Output frequency (Hz)"},
    {"output.current", "Output current (A)"},
    {"ups.load", "Load on UPS (percent of full)"},
    {"ups.power", "Current value of apparent power (VA)"},
    {"ups.realpower", "Current value of real power (W)"},
    {"ups.power.nominal", "UPS power rating (VA)"},
    {"ups.realpower.nominal", "UPS real power rating (W)"},
    {"ups.beeper.status", "UPS beeper status"},
    {"ups.status", "UPS status"},
    {"ups.mfr", "UPS manufacturer"},
    {"ups.model", "UPS model"},
    {"ups.serial", "UPS serial number"},
    {"ups.vendorid", "Vendor ID for USB devices"},
    {"ups.productid", "Product ID for USB devices"},
    {"driver.name", "Driver name"},
    {"driver.version", "Driver version - NUT release"},
};

// Descriptions for GET CMDDESC, from NUT's cmdvartab
const VarDesc CMD_DESCS[] = {
    {"beeper.enable", "Enable the UPS beeper"},
    {"beeper.disable", "Disable the UPS beeper"},
    {"beeper.mute", "Temporarily mute the UPS beeper"},
    {"test.battery.start.quick", "Start a quick battery test"},
    {"test.battery.stop", "Stop the battery test"},
};

bool has_command(const UpsSnapshot &snapshot, const std::string &name) {
  return std::find(snapshot.commands.begin(), snapshot.commands.end(), name) != snapshot.commands.end();
}

}  // namespace

std::vector<std::string> nut_tokenize(const std::string &line) {
  std::vector<std::string> words;
  std::string current;
  bool in_word = false;
  bool in_quotes = false;
  for (size_t i = 0; i < line.size(); i++) {
    char c = line[i];
    if (c == '\\' && i + 1 < line.size()) {
      current += line[++i];
      in_word = true;
    } else if (c == '"') {
      in_quotes = !in_quotes;
      in_word = true;
    } else if (!in_quotes && (c == ' ' || c == '\t' || c == '\r')) {
      if (in_word) {
        words.push_back(current);
        current.clear();
        in_word = false;
      }
    } else {
      current += c;
      in_word = true;
    }
  }
  if (in_word)
    words.push_back(current);
  return words;
}

std::string nut_quote(const std::string &value) {
  std::string out = "\"";
  for (char c : value) {
    if (c == '"' || c == '\\')
      out += '\\';
    out += c;
  }
  out += '"';
  return out;
}

void NutProtocol::open_session(NutSession *session) { this->sessions_.push_back(session); }

void NutProtocol::close_session(NutSession *session) {
  this->sessions_.erase(std::remove(this->sessions_.begin(), this->sessions_.end(), session), this->sessions_.end());
}

size_t NutProtocol::num_logins_() const {
  return std::count_if(this->sessions_.begin(), this->sessions_.end(),
                       [](const NutSession *s) { return s->logged_in; });
}

const NutUser *NutProtocol::authenticated_user_(const NutSession &session) const {
  if (!session.username_set || !session.password_set)
    return nullptr;
  for (const auto &user : this->users_) {
    if (user.username == session.username && user.password == session.password)
      return &user;
  }
  return nullptr;
}

std::string NutProtocol::availability_error_(const UpsSnapshot &snapshot) const {
  if (!snapshot.connected)
    return err("DRIVER-NOT-CONNECTED");
  if (snapshot.stale)
    return err("DATA-STALE");
  return "";
}

std::string NutProtocol::description_(const UpsSnapshot &snapshot) const {
  if (!this->ups_description_.empty())
    return this->ups_description_;
  std::string desc;
  auto mfr = snapshot.vars.find("ups.mfr");
  auto model = snapshot.vars.find("ups.model");
  if (mfr != snapshot.vars.end())
    desc = mfr->second;
  if (model != snapshot.vars.end())
    desc += (desc.empty() ? "" : " ") + model->second;
  return desc.empty() ? "UPS connected to ESPHome" : desc;
}

std::string NutProtocol::effective_status(const UpsSnapshot &snapshot) const {
  auto it = snapshot.vars.find("ups.status");
  std::string status = it != snapshot.vars.end() ? it->second : "";
  if (this->fsd_)
    status = status.empty() ? "FSD" : "FSD " + status;
  return status;
}

bool NutProtocol::lookup_var_(const UpsSnapshot &snapshot, const std::string &name, std::string *value) const {
  auto it = snapshot.vars.find(name);
  if (it == snapshot.vars.end())
    return false;
  *value = name == "ups.status" ? this->effective_status(snapshot) : it->second;
  return true;
}

void NutProtocol::observe(const UpsSnapshot &snapshot, uint32_t now_ms) {
  auto it = snapshot.vars.find("ups.status");
  const bool on_wall = snapshot.connected && it != snapshot.vars.end() && has_token(it->second, "OL");
  if (on_wall && !this->on_wall_power_)
    this->wall_power_since_ms_ = now_ms;
  this->on_wall_power_ = on_wall;
  if (this->fsd_ && on_wall) {
    // Measure from whichever came later: FSD being set, or wall power returning
    const uint32_t elapsed = std::min(now_ms - this->fsd_set_ms_, now_ms - this->wall_power_since_ms_);
    if (elapsed >= FSD_CLEAR_AFTER_MS)
      this->fsd_ = false;
  }
}

std::string NutProtocol::handle_line(NutSession &session, const std::string &line, const UpsSnapshot &snapshot,
                                     uint32_t now_ms) {
  std::vector<std::string> args = nut_tokenize(line);
  if (args.empty())
    return err("UNKNOWN-COMMAND");
  const std::string cmd = upper(args[0]);

  if (cmd == "VER")
    return std::string("Network UPS Tools upsd 2.8.0 - ESPHome NUT server ") + NUT_SERVER_VERSION + "\n";
  if (cmd == "NETVER")
    return "1.3\n";
  if (cmd == "HELP")
    return "Commands: HELP VER GET LIST SET INSTCMD LOGIN LOGOUT USERNAME PASSWORD STARTTLS\n";
  if (cmd == "STARTTLS")
    return err("FEATURE-NOT-CONFIGURED");
  if (cmd == "LOGOUT") {
    session.close_requested = true;
    return "OK Goodbye\n";
  }

  if (cmd == "USERNAME") {
    if (args.size() != 2)
      return err("INVALID-ARGUMENT");
    if (session.username_set)
      return err("ALREADY-SET-USERNAME");
    session.username = args[1];
    session.username_set = true;
    return "OK\n";
  }
  if (cmd == "PASSWORD") {
    if (args.size() != 2)
      return err("INVALID-ARGUMENT");
    if (session.password_set)
      return err("ALREADY-SET-PASSWORD");
    session.password = args[1];
    session.password_set = true;
    return "OK\n";
  }
  if (cmd == "LOGIN") {
    if (args.size() != 2)
      return err("INVALID-ARGUMENT");
    if (!session.username_set)
      return err("USERNAME-REQUIRED");
    if (!session.password_set)
      return err("PASSWORD-REQUIRED");
    if (session.logged_in)
      return err("ALREADY-LOGGED-IN");
    if (args[1] != this->ups_name_)
      return err("UNKNOWN-UPS");
    if (this->authenticated_user_(session) == nullptr)
      return err("ACCESS-DENIED");
    session.logged_in = true;
    return "OK\n";
  }
  if (cmd == "PRIMARY" || cmd == "MASTER") {
    if (args.size() != 2)
      return err("INVALID-ARGUMENT");
    if (args[1] != this->ups_name_)
      return err("UNKNOWN-UPS");
    const NutUser *user = this->authenticated_user_(session);
    if (user == nullptr || !user->primary)
      return err("ACCESS-DENIED");
    session.primary = true;
    return cmd == "PRIMARY" ? "OK PRIMARY-GRANTED\n" : "OK MASTER-GRANTED\n";
  }
  if (cmd == "FSD") {
    if (args.size() != 2)
      return err("INVALID-ARGUMENT");
    if (args[1] != this->ups_name_)
      return err("UNKNOWN-UPS");
    const NutUser *user = this->authenticated_user_(session);
    if (user == nullptr || !user->primary)
      return err("ACCESS-DENIED");
    if (!this->fsd_) {
      this->fsd_ = true;
      this->fsd_set_ms_ = now_ms;
    }
    return "OK FSD-SET\n";
  }

  // Everything below reads UPS data
  if (this->require_login_ && this->authenticated_user_(session) == nullptr &&
      (cmd == "GET" || cmd == "LIST"))
    return err("ACCESS-DENIED");

  if (cmd == "GET")
    return this->handle_get_(session, args, snapshot);
  if (cmd == "LIST")
    return this->handle_list_(session, args, snapshot);
  if (cmd == "SET")
    return err("READONLY");
  if (cmd == "INSTCMD")
    return this->handle_instcmd_(session, args, snapshot);
  return err("UNKNOWN-COMMAND");
}

std::string NutProtocol::handle_get_(NutSession &session, const std::vector<std::string> &args,
                                     const UpsSnapshot &snapshot) {
  (void) session;
  if (args.size() < 3)
    return err("INVALID-ARGUMENT");
  const std::string sub = upper(args[1]);
  const std::string &ups = args[2];
  if (ups != this->ups_name_)
    return err("UNKNOWN-UPS");

  if (sub == "UPSDESC" && args.size() == 3)
    return "UPSDESC " + ups + " " + nut_quote(this->description_(snapshot)) + "\n";
  if (sub == "NUMLOGINS" && args.size() == 3)
    return "NUMLOGINS " + ups + " " + std::to_string(this->num_logins_()) + "\n";
  if (sub == "CMDDESC" && args.size() == 4) {
    for (const auto &desc : CMD_DESCS) {
      if (args[3] == desc.name)
        return "CMDDESC " + ups + " " + args[3] + " " + nut_quote(desc.desc) + "\n";
    }
    return err("CMD-NOT-SUPPORTED");
  }

  if (args.size() != 4 || (sub != "VAR" && sub != "TYPE" && sub != "DESC"))
    return err("INVALID-ARGUMENT");
  const std::string &name = args[3];

  if (sub == "DESC") {
    for (const auto &desc : VAR_DESCS) {
      if (name == desc.name)
        return "DESC " + ups + " " + name + " " + nut_quote(desc.desc) + "\n";
    }
    return "DESC " + ups + " " + name + " " + nut_quote("Description unavailable") + "\n";
  }

  std::string error = this->availability_error_(snapshot);
  if (!error.empty())
    return error;
  std::string value;
  if (!this->lookup_var_(snapshot, name, &value))
    return err("VAR-NOT-SUPPORTED");
  if (sub == "TYPE")
    return "TYPE " + ups + " " + name + " NUMBER\n";
  return "VAR " + ups + " " + name + " " + nut_quote(value) + "\n";
}

std::string NutProtocol::handle_list_(NutSession &session, const std::vector<std::string> &args,
                                      const UpsSnapshot &snapshot) {
  (void) session;
  if (args.size() < 2)
    return err("INVALID-ARGUMENT");
  const std::string sub = upper(args[1]);

  if (sub == "UPS") {
    if (args.size() != 2)
      return err("INVALID-ARGUMENT");
    return "BEGIN LIST UPS\nUPS " + this->ups_name_ + " " + nut_quote(this->description_(snapshot)) +
           "\nEND LIST UPS\n";
  }

  if (args.size() < 3)
    return err("INVALID-ARGUMENT");
  const std::string &ups = args[2];
  if (ups != this->ups_name_)
    return err("UNKNOWN-UPS");

  if (sub == "VAR" && args.size() == 3) {
    std::string error = this->availability_error_(snapshot);
    if (!error.empty())
      return error;
    std::string out = "BEGIN LIST VAR " + ups + "\n";
    for (const auto &var : snapshot.vars) {
      std::string value;
      this->lookup_var_(snapshot, var.first, &value);
      out += "VAR " + ups + " " + var.first + " " + nut_quote(value) + "\n";
    }
    return out + "END LIST VAR " + ups + "\n";
  }
  if (sub == "CMD" && args.size() == 3) {
    std::string out = "BEGIN LIST CMD " + ups + "\n";
    if (snapshot.connected) {
      for (const auto &name : snapshot.commands)
        out += "CMD " + ups + " " + name + "\n";
    }
    return out + "END LIST CMD " + ups + "\n";
  }
  if (sub == "RW" && args.size() == 3)
    return "BEGIN LIST RW " + ups + "\nEND LIST RW " + ups + "\n";
  if (sub == "CLIENT" && args.size() == 3) {
    std::string out = "BEGIN LIST CLIENT " + ups + "\n";
    for (const auto *s : this->sessions_) {
      if (s->logged_in)
        out += "CLIENT " + ups + " " + s->peer + "\n";
    }
    return out + "END LIST CLIENT " + ups + "\n";
  }
  if ((sub == "ENUM" || sub == "RANGE") && args.size() == 4) {
    const std::string &name = args[3];
    std::string error = this->availability_error_(snapshot);
    if (!error.empty())
      return error;
    std::string value;
    if (!this->lookup_var_(snapshot, name, &value))
      return err("VAR-NOT-SUPPORTED");
    return "BEGIN LIST " + sub + " " + ups + " " + name + "\nEND LIST " + sub + " " + ups + " " + name + "\n";
  }
  return err("INVALID-ARGUMENT");
}

std::string NutProtocol::handle_instcmd_(NutSession &session, const std::vector<std::string> &args,
                                         const UpsSnapshot &snapshot) {
  if (args.size() < 3)
    return err("INVALID-ARGUMENT");
  if (args[1] != this->ups_name_)
    return err("UNKNOWN-UPS");
  const NutUser *user = this->authenticated_user_(session);
  if (user == nullptr || !user->allow_commands)
    return err("ACCESS-DENIED");
  std::string error = this->availability_error_(snapshot);
  if (!error.empty())
    return error;
  if (!has_command(snapshot, args[2]))
    return err("CMD-NOT-SUPPORTED");
  if (!this->command_handler_ || !this->command_handler_(args[2]))
    return err("INSTCMD-FAILED");
  return "OK\n";
}

}  // namespace esphome::nut_server
