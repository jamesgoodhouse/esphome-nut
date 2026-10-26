#include "nut_server.h"

#include <cerrno>
#include <span>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::nut_server {

static const char *const TAG = "nut_server";

static constexpr int LISTEN_BACKLOG = 4;

void NutServer::setup() {
  if (this->api_reboot_disabled_) {
    ESP_LOGI(TAG, "Home Assistant is optional: this device will not reboot when Home Assistant is unreachable, "
                  "so the UPS stays monitored even if Home Assistant is down");
  }
  this->start_listening_();
}

void NutServer::dump_config() {
  ESP_LOGCONFIG(TAG,
                "NUT server:\n"
                "  Port: %u\n"
                "  UPS name: %s (clients connect to %s@<this device's IP>)\n"
                "  Max connections: %u\n"
                "  Require login: %s\n"
                "  Users: %u",
                this->port_, this->protocol_.get_ups_name().c_str(), this->protocol_.get_ups_name().c_str(),
                this->max_connections_, YESNO(this->require_login_),
                static_cast<unsigned>(this->protocol_.get_users().size()));
  for (const auto &user : this->protocol_.get_users())
    ESP_LOGCONFIG(TAG, "    %s (%s)", user.username.c_str(), user.primary ? "primary" : "secondary");
  ESP_LOGCONFIG(TAG, "  Reboot if Home Assistant is unreachable: %s", YESNO(!this->api_reboot_disabled_));
}

bool NutServer::start_listening_() {
  this->last_listen_attempt_ms_ = millis();
  this->listen_ = socket::socket_ip_loop_monitored(SOCK_STREAM, IPPROTO_TCP);
  if (this->listen_ == nullptr) {
    ESP_LOGW(TAG, "Could not create socket: %d", errno);
    return false;
  }
  int yes = 1;
  this->listen_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct sockaddr_storage local;
  socklen_t local_len =
      socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&local), sizeof(local), this->port_);
  if (this->listen_->setblocking(false) != 0 || local_len == 0 ||
      this->listen_->bind(reinterpret_cast<struct sockaddr *>(&local), local_len) != 0 ||
      this->listen_->listen(LISTEN_BACKLOG) != 0) {
    ESP_LOGW(TAG, "Could not listen on port %u: %d", this->port_, errno);
    this->listen_.reset();
    return false;
  }
  ESP_LOGI(TAG, "Listening on port %u", this->port_);
  return true;
}

void NutServer::loop() {
  const uint32_t now = millis();
  if (this->listen_ == nullptr) {
    if (now - this->last_listen_attempt_ms_ >= LISTEN_RETRY_MS)
      this->start_listening_();
    return;
  }

  static const ups_common::UpsSnapshot NO_UPS;
  const auto &snapshot = this->source_ != nullptr ? this->source_->get_ups_snapshot() : NO_UPS;
  this->protocol_.observe(snapshot, now);

  this->accept_clients_();
  for (auto &client : this->clients_)
    this->serve_client_(*client, snapshot, now);

  // Drop closed connections
  for (auto it = this->clients_.begin(); it != this->clients_.end();) {
    Client &client = **it;
    if (client.closing && client.output.empty()) {
      ESP_LOGD(TAG, "Client %s disconnected", client.session.peer.c_str());
      this->protocol_.close_session(&client.session);
      it = this->clients_.erase(it);
    } else {
      ++it;
    }
  }
}

void NutServer::accept_clients_() {
  if (!this->listen_->ready())
    return;
  // Drain all pending connections (see the ready() contract in socket.h)
  while (true) {
    struct sockaddr_storage peer {};
    socklen_t peer_len = sizeof(peer);
    auto sock = this->listen_->accept_loop_monitored(reinterpret_cast<struct sockaddr *>(&peer), &peer_len);
    if (sock == nullptr)
      return;
    char address[socket::SOCKADDR_STR_LEN];
    socket::format_sockaddr_to(reinterpret_cast<struct sockaddr *>(&peer), peer_len,
                               std::span<char, socket::SOCKADDR_STR_LEN>(address));
    if (this->clients_.size() >= this->max_connections_) {
      ESP_LOGW(TAG, "Refusing client %s: already serving %u clients (raise max_connections to allow more)",
               address, this->max_connections_);
      continue;  // socket closes when it goes out of scope
    }
    sock->setblocking(false);
    int yes = 1;
    sock->setsockopt(IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    auto client = std::make_unique<Client>();
    client->socket = std::move(sock);
    client->session.peer = address;
    client->last_activity_ms = millis();
    this->protocol_.open_session(&client->session);
    ESP_LOGD(TAG, "Client %s connected", address);
    this->clients_.push_back(std::move(client));
  }
}

void NutServer::serve_client_(Client &client, const ups_common::UpsSnapshot &snapshot, uint32_t now) {
  if (!client.closing && client.socket->ready()) {
    char buf[128];
    while (true) {
      ssize_t n = client.socket->read(buf, sizeof(buf));
      if (n > 0) {
        client.input.append(buf, n);
        client.last_activity_ms = now;
        continue;
      }
      if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        client.closing = true;  // peer closed or connection error
        client.output.clear();
      }
      break;
    }
  }

  size_t newline;
  while (!client.closing && (newline = client.input.find('\n')) != std::string::npos) {
    std::string line = client.input.substr(0, newline);
    client.input.erase(0, newline + 1);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    ESP_LOGV(TAG, "%s> %s", client.session.peer.c_str(),
             line.rfind("PASSWORD", 0) == 0 ? "PASSWORD ****" : line.c_str());
    client.output += this->protocol_.handle_line(client.session, line, snapshot, now);
    if (client.session.close_requested)
      client.closing = true;
  }
  if (!client.closing && client.input.size() > MAX_LINE_LENGTH) {
    ESP_LOGW(TAG, "Client %s sent an over-long line; disconnecting", client.session.peer.c_str());
    client.closing = true;
    client.output.clear();
  }
  if (!client.closing && now - client.last_activity_ms > IDLE_TIMEOUT_MS) {
    ESP_LOGD(TAG, "Client %s idle; disconnecting", client.session.peer.c_str());
    client.closing = true;
    client.output.clear();
  }
  this->flush_client_(client);
}

void NutServer::flush_client_(Client &client) {
  while (!client.output.empty()) {
    ssize_t n = client.socket->write(client.output.data(), client.output.size());
    if (n > 0) {
      client.output.erase(0, n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return;  // try again next loop
    client.output.clear();  // connection broken
    client.closing = true;
    return;
  }
}

}  // namespace esphome::nut_server
