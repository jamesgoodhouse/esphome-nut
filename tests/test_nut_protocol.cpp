// Tests for the NUT network protocol handler, using command sequences real clients send.

#include <string>

#include "esphome/components/nut_server/nut_protocol.h"
#include "test_framework.h"

using esphome::nut_server::nut_quote;
using esphome::nut_server::nut_tokenize;
using esphome::nut_server::NutProtocol;
using esphome::nut_server::NutSession;
using esphome::ups_common::UpsSnapshot;

namespace {

UpsSnapshot online_snapshot(const std::string &status = "OL CHRG") {
  UpsSnapshot snap;
  snap.connected = true;
  snap.vars["battery.charge"] = "87";
  snap.vars["ups.status"] = status;
  snap.vars["ups.mfr"] = "APC";
  snap.vars["ups.model"] = "Back-UPS ES 600";
  return snap;
}

NutProtocol make_server() {
  NutProtocol nut;
  nut.add_user("monuser", "secret", false);
  nut.add_user("admin", "hunter2", true);
  return nut;
}

}  // namespace

TEST(version_and_help) {
  NutProtocol nut = make_server();
  NutSession s;
  auto snap = online_snapshot();
  CHECK_EQ(nut.handle_line(s, "NETVER", snap, 0), std::string("1.3\n"));
  CHECK(nut.handle_line(s, "VER", snap, 0).find("Network UPS Tools upsd") == 0);
  CHECK(nut.handle_line(s, "HELP", snap, 0).find("Commands:") == 0);
}

TEST(upsc_style_reads_without_login) {
  NutProtocol nut = make_server();
  NutSession s;
  auto snap = online_snapshot();
  CHECK_EQ(nut.handle_line(s, "LIST UPS", snap, 0),
           std::string("BEGIN LIST UPS\nUPS ups \"APC Back-UPS ES 600\"\nEND LIST UPS\n"));
  CHECK_EQ(nut.handle_line(s, "GET VAR ups battery.charge", snap, 0),
           std::string("VAR ups battery.charge \"87\"\n"));
  CHECK_EQ(nut.handle_line(s, "LIST VAR ups", snap, 0),
           std::string("BEGIN LIST VAR ups\n"
                       "VAR ups battery.charge \"87\"\n"
                       "VAR ups ups.mfr \"APC\"\n"
                       "VAR ups ups.model \"Back-UPS ES 600\"\n"
                       "VAR ups ups.status \"OL CHRG\"\n"
                       "END LIST VAR ups\n"));
  CHECK_EQ(nut.handle_line(s, "GET TYPE ups battery.charge", snap, 0),
           std::string("TYPE ups battery.charge NUMBER\n"));
  CHECK_EQ(nut.handle_line(s, "GET DESC ups battery.charge", snap, 0),
           std::string("DESC ups battery.charge \"Battery charge (percent of full)\"\n"));
  CHECK_EQ(nut.handle_line(s, "LIST CMD ups", snap, 0), std::string("BEGIN LIST CMD ups\nEND LIST CMD ups\n"));
  CHECK_EQ(nut.handle_line(s, "LIST RW ups", snap, 0), std::string("BEGIN LIST RW ups\nEND LIST RW ups\n"));
}

TEST(errors_for_unknown_things) {
  NutProtocol nut = make_server();
  NutSession s;
  auto snap = online_snapshot();
  CHECK_EQ(nut.handle_line(s, "GET VAR ups no.such.var", snap, 0), std::string("ERR VAR-NOT-SUPPORTED\n"));
  CHECK_EQ(nut.handle_line(s, "GET VAR other battery.charge", snap, 0), std::string("ERR UNKNOWN-UPS\n"));
  CHECK_EQ(nut.handle_line(s, "FROB", snap, 0), std::string("ERR UNKNOWN-COMMAND\n"));
  CHECK_EQ(nut.handle_line(s, "GET VAR ups", snap, 0), std::string("ERR INVALID-ARGUMENT\n"));
  CHECK_EQ(nut.handle_line(s, "SET VAR ups ups.delay.shutdown 20", snap, 0), std::string("ERR READONLY\n"));
  CHECK_EQ(nut.handle_line(s, "INSTCMD ups beeper.off", snap, 0), std::string("ERR CMD-NOT-SUPPORTED\n"));
  CHECK_EQ(nut.handle_line(s, "STARTTLS", snap, 0), std::string("ERR FEATURE-NOT-CONFIGURED\n"));
}

TEST(no_ups_or_stale_data_is_reported) {
  NutProtocol nut = make_server();
  NutSession s;
  UpsSnapshot gone;
  CHECK_EQ(nut.handle_line(s, "GET VAR ups ups.status", gone, 0), std::string("ERR DRIVER-NOT-CONNECTED\n"));
  CHECK_EQ(nut.handle_line(s, "LIST VAR ups", gone, 0), std::string("ERR DRIVER-NOT-CONNECTED\n"));
  // LIST UPS still works so clients can see the UPS exists
  CHECK(nut.handle_line(s, "LIST UPS", gone, 0).find("UPS ups ") != std::string::npos);
  auto stale = online_snapshot();
  stale.stale = true;
  CHECK_EQ(nut.handle_line(s, "GET VAR ups ups.status", stale, 0), std::string("ERR DATA-STALE\n"));
}

TEST(synology_style_secondary_login) {
  NutProtocol nut = make_server();
  NutSession s;
  s.peer = "192.168.1.20";
  nut.open_session(&s);
  auto snap = online_snapshot();
  CHECK_EQ(nut.handle_line(s, "USERNAME monuser", snap, 0), std::string("OK\n"));
  CHECK_EQ(nut.handle_line(s, "PASSWORD secret", snap, 0), std::string("OK\n"));
  CHECK_EQ(nut.handle_line(s, "LOGIN ups", snap, 0), std::string("OK\n"));
  CHECK_EQ(nut.handle_line(s, "GET NUMLOGINS ups", snap, 0), std::string("NUMLOGINS ups 1\n"));
  CHECK_EQ(nut.handle_line(s, "LIST CLIENT ups", snap, 0),
           std::string("BEGIN LIST CLIENT ups\nCLIENT ups 192.168.1.20\nEND LIST CLIENT ups\n"));
  CHECK_EQ(nut.handle_line(s, "LOGIN ups", snap, 0), std::string("ERR ALREADY-LOGGED-IN\n"));
  // A secondary cannot claim primary or force a shutdown
  CHECK_EQ(nut.handle_line(s, "PRIMARY ups", snap, 0), std::string("ERR ACCESS-DENIED\n"));
  CHECK_EQ(nut.handle_line(s, "FSD ups", snap, 0), std::string("ERR ACCESS-DENIED\n"));
  CHECK_EQ(nut.handle_line(s, "LOGOUT", snap, 0), std::string("OK Goodbye\n"));
  CHECK(s.close_requested);
  nut.close_session(&s);
  CHECK_EQ(nut.handle_line(s, "GET NUMLOGINS ups", snap, 0), std::string("NUMLOGINS ups 0\n"));
}

TEST(login_checks_credentials) {
  NutProtocol nut = make_server();
  NutSession s;
  auto snap = online_snapshot();
  CHECK_EQ(nut.handle_line(s, "LOGIN ups", snap, 0), std::string("ERR USERNAME-REQUIRED\n"));
  nut.handle_line(s, "USERNAME monuser", snap, 0);
  CHECK_EQ(nut.handle_line(s, "LOGIN ups", snap, 0), std::string("ERR PASSWORD-REQUIRED\n"));
  nut.handle_line(s, "PASSWORD wrong", snap, 0);
  CHECK_EQ(nut.handle_line(s, "LOGIN ups", snap, 0), std::string("ERR ACCESS-DENIED\n"));
  CHECK_EQ(nut.handle_line(s, "USERNAME again", snap, 0), std::string("ERR ALREADY-SET-USERNAME\n"));
}

TEST(primary_can_force_shutdown_and_it_clears_on_wall_power) {
  NutProtocol nut = make_server();
  NutSession primary;
  auto on_battery = online_snapshot("OB DISCHRG");
  nut.handle_line(primary, "USERNAME admin", on_battery, 0);
  nut.handle_line(primary, "PASSWORD hunter2", on_battery, 0);
  CHECK_EQ(nut.handle_line(primary, "PRIMARY ups", on_battery, 0), std::string("OK PRIMARY-GRANTED\n"));
  CHECK_EQ(nut.handle_line(primary, "MASTER ups", on_battery, 0), std::string("OK MASTER-GRANTED\n"));
  CHECK_EQ(nut.handle_line(primary, "LOGIN ups", on_battery, 0), std::string("OK\n"));
  CHECK_EQ(nut.handle_line(primary, "FSD ups", on_battery, 1000), std::string("OK FSD-SET\n"));

  NutSession secondary;
  CHECK_EQ(nut.handle_line(secondary, "GET VAR ups ups.status", on_battery, 2000),
           std::string("VAR ups ups.status \"FSD OB DISCHRG\"\n"));

  // Still latched while on battery, no matter how long
  nut.observe(on_battery, 1000000);
  CHECK(nut.fsd_active());

  // Wall power back: clears after five minutes of it
  auto online = online_snapshot("OL CHRG");
  nut.observe(online, 2000000);
  CHECK(nut.fsd_active());
  nut.observe(online, 2000000 + NutProtocol::FSD_CLEAR_AFTER_MS - 1);
  CHECK(nut.fsd_active());
  nut.observe(online, 2000000 + NutProtocol::FSD_CLEAR_AFTER_MS);
  CHECK(!nut.fsd_active());
  CHECK_EQ(nut.handle_line(secondary, "GET VAR ups ups.status", online, 0),
           std::string("VAR ups ups.status \"OL CHRG\"\n"));
}

TEST(fsd_set_on_wall_power_still_lasts_five_minutes) {
  NutProtocol nut = make_server();
  NutSession primary;
  auto online = online_snapshot("OL");
  nut.observe(online, 0);
  nut.handle_line(primary, "USERNAME admin", online, 0);
  nut.handle_line(primary, "PASSWORD hunter2", online, 0);
  nut.handle_line(primary, "FSD ups", online, 600000);
  nut.observe(online, 600000 + 60000);
  CHECK(nut.fsd_active());
  nut.observe(online, 600000 + NutProtocol::FSD_CLEAR_AFTER_MS);
  CHECK(!nut.fsd_active());
}

TEST(require_login_blocks_anonymous_reads) {
  NutProtocol nut = make_server();
  nut.set_require_login(true);
  NutSession s;
  auto snap = online_snapshot();
  CHECK_EQ(nut.handle_line(s, "GET VAR ups battery.charge", snap, 0), std::string("ERR ACCESS-DENIED\n"));
  CHECK_EQ(nut.handle_line(s, "LIST UPS", snap, 0), std::string("ERR ACCESS-DENIED\n"));
  nut.handle_line(s, "USERNAME monuser", snap, 0);
  nut.handle_line(s, "PASSWORD secret", snap, 0);
  CHECK_EQ(nut.handle_line(s, "GET VAR ups battery.charge", snap, 0),
           std::string("VAR ups battery.charge \"87\"\n"));
}

TEST(custom_ups_name_and_description) {
  NutProtocol nut = make_server();
  nut.set_ups_name("rack");
  nut.set_ups_description("Rack UPS");
  NutSession s;
  auto snap = online_snapshot();
  CHECK_EQ(nut.handle_line(s, "GET UPSDESC rack", snap, 0), std::string("UPSDESC rack \"Rack UPS\"\n"));
  CHECK_EQ(nut.handle_line(s, "GET VAR ups ups.status", snap, 0), std::string("ERR UNKNOWN-UPS\n"));
}

TEST(tokenizer_and_quoting) {
  auto words = nut_tokenize("USERNAME \"a b\\\"c\"  x\\ y");
  CHECK_EQ(words.size(), size_t(3));
  if (words.size() == 3) {
    CHECK_EQ(words[1], std::string("a b\"c"));
    CHECK_EQ(words[2], std::string("x y"));
  }
  CHECK_EQ(nut_tokenize("LIST UPS\r").size(), size_t(2));
  CHECK_EQ(nut_quote("say \"hi\" \\o/"), std::string("\"say \\\"hi\\\" \\\\o/\""));
}

int main() { return test::run_all(); }
