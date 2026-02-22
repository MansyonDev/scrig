#include "scrig/stratum_client.hpp"

#include "scrig/json.hpp"
#include "scrig/net_platform.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace scrig {

namespace {

constexpr const char kStratumAgent[] = "scrig/" SCRIG_VERSION;

bool socket_wait_readable(std::intptr_t raw_socket, uint32_t timeout_ms) {
#if defined(_WIN32)
  SOCKET sock = static_cast<SOCKET>(raw_socket);
#else
  int sock = static_cast<int>(raw_socket);
#endif
  if (sock < 0) {
    return false;
  }

  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(sock, &readfds);

  timeval tv{};
  timeval* ptv = nullptr;
  if (timeout_ms != std::numeric_limits<uint32_t>::max()) {
    tv.tv_sec = static_cast<long>(timeout_ms / 1000U);
    tv.tv_usec = static_cast<long>((timeout_ms % 1000U) * 1000U);
    ptv = &tv;
  }

  const int rc = select(static_cast<int>(sock + 1), &readfds, nullptr, nullptr, ptv);
  return rc > 0 && FD_ISSET(sock, &readfds);
}

bool json_has_error(const JsonValue& message) {
  if (!message.is_object()) {
    return false;
  }
  const auto& obj = message.as_object();
  const auto it = obj.find("error");
  return it != obj.end() && !it->second.is_null();
}

std::string json_error_text(const JsonValue& message) {
  if (!message.is_object()) {
    return "invalid response";
  }
  const auto& obj = message.as_object();
  const auto it = obj.find("error");
  if (it == obj.end() || it->second.is_null()) {
    return "";
  }

  if (it->second.is_string()) {
    return it->second.as_string();
  }
  if (it->second.is_object()) {
    const auto& err = it->second.as_object();
    const auto msg = err.find("message");
    if (msg != err.end() && msg->second.is_string()) {
      return msg->second.as_string();
    }
  }
  return "pool returned error";
}

bool parse_json_rpc_id(const JsonValue& value, std::string* out) {
  if (value.is_uint64()) {
    *out = std::to_string(value.as_uint64());
    return true;
  }
  if (value.is_int64()) {
    *out = std::to_string(value.as_int64());
    return true;
  }
  if (value.is_string()) {
    *out = value.as_string();
    return true;
  }
  return false;
}

uint64_t difficulty_to_target_le(double diff) {
  if (!(diff > 0.0)) {
    return 0;
  }
  const long double max_u64 = static_cast<long double>(std::numeric_limits<uint64_t>::max());
  const long double raw = max_u64 / static_cast<long double>(diff);
  if (raw < 1.0L) {
    return 1;
  }
  if (raw >= max_u64) {
    return std::numeric_limits<uint64_t>::max();
  }
  return static_cast<uint64_t>(raw);
}

uint64_t parse_target_prefix_le(const std::vector<uint8_t>& bytes) {
  uint64_t value = 0;
  const size_t n = std::min<size_t>(8, bytes.size());
  for (size_t i = 0; i < n; ++i) {
    value |= static_cast<uint64_t>(bytes[i]) << (8U * i);
  }
  return value;
}

uint64_t parse_target_prefix_be(const std::vector<uint8_t>& bytes) {
  uint64_t value = 0;
  const size_t n = std::min<size_t>(8, bytes.size());
  for (size_t i = 0; i < n; ++i) {
    value = (value << 8U) | static_cast<uint64_t>(bytes[i]);
  }
  return value;
}

uint64_t distance_u64(uint64_t a, uint64_t b) {
  return (a > b) ? (a - b) : (b - a);
}

bool json_number_to_u64(const JsonValue& value, uint64_t* out) {
  if (value.is_uint64()) {
    *out = value.as_uint64();
    return true;
  }
  if (value.is_int64()) {
    const int64_t v = value.as_int64();
    if (v < 0) {
      return false;
    }
    *out = static_cast<uint64_t>(v);
    return true;
  }
  if (value.is_double()) {
    const double d = value.as_double();
    if (!(d >= 0.0)) {
      return false;
    }
    *out = static_cast<uint64_t>(d);
    return true;
  }
  return false;
}

bool json_number_to_double(const JsonValue& value, double* out) {
  if (!value.is_number()) {
    return false;
  }
  *out = value.as_double();
  return true;
}

std::string to_lower_copy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

bool looks_like_method_missing(const std::string& error) {
  const std::string lower = to_lower_copy(error);
  return lower.find("method not found") != std::string::npos ||
         lower.find("unknown method") != std::string::npos ||
         lower.find("no such method") != std::string::npos;
}

bool looks_like_connection_problem(const std::string& error) {
  const std::string lower = to_lower_copy(error);
  return lower.find("connection closed") != std::string::npos ||
         lower.find("failed to send") != std::string::npos ||
         lower.find("not connected") != std::string::npos ||
         lower.find("timeout waiting for pool message") != std::string::npos ||
         lower.find("reconnect failed") != std::string::npos;
}

} // namespace

struct StratumClient::ParsedMessage {
  JsonValue message;
  std::optional<std::string> id;
  std::optional<std::string> method;
};

StratumClient::StratumClient(std::string host, uint16_t port)
  : host_(std::move(host)), port_(port) {
}

StratumClient::~StratumClient() {
  disconnect();
}

void StratumClient::connect() {
  if (socket_fd_ != invalid_socket_handle()) {
    return;
  }
  initialize_network_stack_once();
  socket_fd_ = static_cast<std::intptr_t>(connect_tcp_socket(host_, port_));
}

void StratumClient::disconnect() {
  if (socket_fd_ != invalid_socket_handle()) {
    close_socket_handle(static_cast<SocketHandle>(socket_fd_));
  }
  socket_fd_ = invalid_socket_handle();
  rx_buffer_.clear();
  queued_jobs_.clear();
  difficulty_.reset();
  session_id_.reset();
  mode_ = Mode::UNKNOWN;
}

bool StratumClient::send_json_line(const JsonValue& value) {
  if (socket_fd_ == invalid_socket_handle()) {
    return false;
  }
  std::string line = to_json(value, false);
  line.push_back('\n');
  size_t written = 0;
  while (written < line.size()) {
    const size_t n = send_socket_data(
      static_cast<SocketHandle>(socket_fd_),
      reinterpret_cast<const uint8_t*>(line.data() + written),
      line.size() - written);
    if (n == 0) {
      return false;
    }
    written += n;
  }
  return true;
}

bool StratumClient::read_json_line(JsonValue* out, uint32_t timeout_ms, std::string* error_message) {
  if (socket_fd_ == invalid_socket_handle()) {
    if (error_message != nullptr) {
      *error_message = "not connected";
    }
    return false;
  }

  while (true) {
    const size_t nl = rx_buffer_.find('\n');
    if (nl != std::string::npos) {
      std::string line = rx_buffer_.substr(0, nl);
      rx_buffer_.erase(0, nl + 1);
      line = trim(line);
      if (line.empty()) {
        continue;
      }
      try {
        *out = parse_json(line);
        return true;
      } catch (const std::exception& ex) {
        if (error_message != nullptr) {
          *error_message = std::string("invalid stratum JSON: ") + ex.what();
        }
        return false;
      }
    }

    if (!socket_wait_readable(socket_fd_, timeout_ms)) {
      if (error_message != nullptr) {
        *error_message = "timeout waiting for pool message";
      }
      return false;
    }

    std::array<uint8_t, 4096> chunk{};
    const size_t n = recv_socket_data(static_cast<SocketHandle>(socket_fd_), chunk.data(), chunk.size());
    if (n == 0) {
      if (error_message != nullptr) {
        *error_message = "pool connection closed";
      }
      return false;
    }
    rx_buffer_.append(reinterpret_cast<const char*>(chunk.data()), n);
  }
}

bool StratumClient::read_message(ParsedMessage* out, uint32_t timeout_ms, std::string* error_message) {
  JsonValue msg;
  if (!read_json_line(&msg, timeout_ms, error_message)) {
    return false;
  }

  ParsedMessage parsed;
  parsed.message = std::move(msg);
  if (parsed.message.is_object()) {
    const auto& obj = parsed.message.as_object();
    if (const auto id_it = obj.find("id"); id_it != obj.end()) {
      std::string id;
      if (parse_json_rpc_id(id_it->second, &id)) {
        parsed.id = id;
      }
    }
    if (const auto method_it = obj.find("method"); method_it != obj.end() && method_it->second.is_string()) {
      parsed.method = method_it->second.as_string();
    }
    if (const auto method_it = obj.find("method"); method_it != obj.end() && method_it->second.is_string()) {
      const std::string method = method_it->second.as_string();
      if (method == "mining.set_difficulty" || method == "set_difficulty") {
        const auto params_it = obj.find("params");
        if (params_it != obj.end()) {
          if (params_it->second.is_array() && !params_it->second.as_array().empty()) {
            difficulty_ = params_it->second.as_array().front().as_double();
          } else if (params_it->second.is_object()) {
            const auto& p = params_it->second.as_object();
            const auto diff_it = p.find("difficulty");
            if (diff_it != p.end()) {
              difficulty_ = diff_it->second.as_double();
            }
          }
        }
      }

      StratumJob queued;
      if (parse_job_from_message(parsed.message, &queued)) {
        queued_jobs_.push_back(std::move(queued));
      }
    }
  }

  *out = std::move(parsed);
  return true;
}

bool StratumClient::request_response(
  const std::string& method,
  JsonValue params,
  JsonValue* response,
  uint32_t timeout_ms,
  bool include_jsonrpc,
  std::string* error_message) {

  const uint64_t req_id_value = next_request_id_++;
  const std::string req_id = std::to_string(req_id_value);
  JsonValue::object request{
    {"id", JsonValue(req_id_value)},
    {"method", JsonValue(method)},
    {"params", std::move(params)},
  };
  if (include_jsonrpc) {
    request["jsonrpc"] = JsonValue("2.0");
  }

  if (!send_json_line(JsonValue(std::move(request)))) {
    if (error_message != nullptr) {
      *error_message = "failed to send pool request";
    }
    return false;
  }

  while (true) {
    ParsedMessage msg;
    if (!read_message(&msg, timeout_ms, error_message)) {
      return false;
    }
    if (!msg.id.has_value()) {
      continue;
    }
    if (*msg.id != req_id) {
      continue;
    }
    *response = std::move(msg.message);
    return true;
  }
}

bool StratumClient::request_response_or_job(
  const std::string& method,
  JsonValue params,
  JsonValue* response,
  uint32_t timeout_ms,
  bool include_jsonrpc,
  std::string* error_message) {

  const uint64_t req_id_value = next_request_id_++;
  const std::string req_id = std::to_string(req_id_value);
  JsonValue::object request{
    {"id", JsonValue(req_id_value)},
    {"method", JsonValue(method)},
    {"params", std::move(params)},
  };
  if (include_jsonrpc) {
    request["jsonrpc"] = JsonValue("2.0");
  }

  if (!send_json_line(JsonValue(std::move(request)))) {
    if (error_message != nullptr) {
      *error_message = "failed to send pool request";
    }
    return false;
  }

  while (true) {
    ParsedMessage msg;
    std::string read_error;
    if (!read_message(&msg, timeout_ms, &read_error)) {
      if (!queued_jobs_.empty()) {
        if (response != nullptr) {
          *response = JsonValue();
        }
        return true;
      }
      if (error_message != nullptr) {
        *error_message = read_error;
      }
      return false;
    }
    if (msg.id.has_value() && *msg.id == req_id) {
      if (response != nullptr) {
        *response = std::move(msg.message);
      }
      return true;
    }
    if (!queued_jobs_.empty()) {
      if (response != nullptr) {
        *response = JsonValue();
      }
      return true;
    }
  }
}

bool StratumClient::reconnect(std::string* error_message) {
  try {
    disconnect();
    connect();
    return true;
  } catch (const std::exception& ex) {
    if (error_message != nullptr) {
      *error_message = std::string("reconnect failed: ") + ex.what();
    }
    return false;
  } catch (...) {
    if (error_message != nullptr) {
      *error_message = "reconnect failed";
    }
    return false;
  }
}

bool StratumClient::login(const std::string& user, const std::string& pass, std::string* error_message) {
  queued_jobs_.clear();
  difficulty_.reset();
  session_id_.reset();
  mode_ = Mode::UNKNOWN;

  struct Attempt {
    const char* name;
    bool (StratumClient::*fn)(const std::string&, const std::string&, std::string*);
  };

  const std::array<Attempt, 5> attempts{{
    {"login", &StratumClient::try_login_method},
    {"mining.auth", &StratumClient::try_mining_auth_method},
    {"Login", &StratumClient::try_login_caps_method},
    {"mining.subscribe+authorize", &StratumClient::try_mining_method},
    {"authorize-only", &StratumClient::try_authorize_only_method}
  }};

  std::vector<std::string> errors;
  errors.reserve(attempts.size());
  for (size_t i = 0; i < attempts.size(); ++i) {
    if (i > 0) {
      std::string reconnect_error;
      if (!reconnect(&reconnect_error)) {
        errors.push_back(reconnect_error);
        continue;
      }
    }

    std::string attempt_error;
    if ((this->*attempts[i].fn)(user, pass, &attempt_error)) {
      return true;
    }

    if (attempt_error.empty()) {
      attempt_error = std::string(attempts[i].name) + " failed";
    }
    errors.push_back(std::move(attempt_error));
  }

  if (error_message != nullptr) {
    if (errors.empty()) {
      *error_message = "stratum login failed";
    } else {
      std::string merged;
      for (size_t i = 0; i < errors.size(); ++i) {
        if (i > 0) {
          merged += " | ";
        }
        merged += errors[i];
      }
      *error_message = std::move(merged);
    }
  }
  return false;
}

bool StratumClient::try_login_method(const std::string& user, const std::string& pass, std::string* error_message) {
  struct LoginReq {
    JsonValue params;
    bool include_jsonrpc = true;
  };

  std::vector<LoginReq> variants;
  variants.push_back(LoginReq{
    JsonValue(JsonValue::object{
      {"login", JsonValue(user)},
      {"pass", JsonValue(pass)},
      {"agent", JsonValue(kStratumAgent)},
    }),
    true,
  });
  variants.push_back(LoginReq{
    JsonValue(JsonValue::array{JsonValue(user), JsonValue(pass), JsonValue(kStratumAgent)}),
    true,
  });
  variants.push_back(LoginReq{
    JsonValue(JsonValue::array{JsonValue(user), JsonValue(pass)}),
    true,
  });
  variants.push_back(LoginReq{
    JsonValue(JsonValue::array{JsonValue(user)}),
    true,
  });

  variants.push_back(LoginReq{
    JsonValue(JsonValue::object{
      {"login", JsonValue(user)},
      {"pass", JsonValue(pass)},
      {"agent", JsonValue(kStratumAgent)},
    }),
    false,
  });
  variants.push_back(LoginReq{
    JsonValue(JsonValue::array{JsonValue(user), JsonValue(pass), JsonValue(kStratumAgent)}),
    false,
  });
  variants.push_back(LoginReq{
    JsonValue(JsonValue::array{JsonValue(user), JsonValue(pass)}),
    false,
  });
  variants.push_back(LoginReq{
    JsonValue(JsonValue::array{JsonValue(user)}),
    false,
  });

  std::string last_error = "login failed";
  for (size_t i = 0; i < variants.size(); ++i) {
    if (i > 0) {
      (void)reconnect(nullptr);
    }

    JsonValue resp;
    std::string req_error;
    if (!request_response_or_job("login", std::move(variants[i].params), &resp, 5000, variants[i].include_jsonrpc, &req_error)) {
      last_error = req_error.empty() ? "login request failed" : req_error;
      continue;
    }

    if (resp.is_object() && json_has_error(resp)) {
      last_error = "login rejected: " + json_error_text(resp);
      continue;
    }

    if (resp.is_object()) {
      const auto& obj = resp.as_object();
      const auto result_it = obj.find("result");
      if (result_it != obj.end() && result_it->second.is_object()) {
        const auto& result = result_it->second.as_object();
        const auto sid_it = result.find("id");
        if (sid_it != result.end()) {
          std::string sid;
          if (parse_json_rpc_id(sid_it->second, &sid)) {
            session_id_ = sid;
          }
        }

        const auto job_it = result.find("job");
        if (job_it != result.end() && job_it->second.is_object()) {
          StratumJob initial;
          if (parse_job_from_object(job_it->second.as_object(), &initial)) {
            queued_jobs_.push_back(std::move(initial));
          }
        }
      }
    }

    mode_ = Mode::LOGIN_SUBMIT;
    return true;
  }

  if (error_message != nullptr) {
    *error_message = last_error;
  }
  return false;
}

bool StratumClient::try_mining_method(const std::string& user, const std::string& pass, std::string* error_message) {
  const std::array<std::string, 3> subscribe_methods{
    "mining.subscribe",
    "Subscribe",
    "subscribe",
  };
  const std::array<std::string, 3> authorize_methods{
    "mining.authorize",
    "Authorize",
    "authorize",
  };

  std::string last_error = "subscribe/authorize failed";
  for (bool include_jsonrpc : {true, false}) {
    for (size_t s = 0; s < subscribe_methods.size(); ++s) {
      if (s > 0 || !include_jsonrpc) {
        (void)reconnect(nullptr);
      }

      JsonValue sub_resp;
      std::string sub_error;
      if (!request_response(
            subscribe_methods[s],
            JsonValue(JsonValue::array{JsonValue(kStratumAgent)}),
            &sub_resp,
            5000,
            include_jsonrpc,
            &sub_error)) {
        last_error = sub_error.empty() ? "subscribe request failed" : sub_error;
        continue;
      }

      if (json_has_error(sub_resp)) {
        last_error = "subscribe rejected: " + json_error_text(sub_resp);
        if (looks_like_method_missing(last_error)) {
          continue;
        }
        continue;
      }

      for (const auto& auth_method : authorize_methods) {
        JsonValue auth_resp;
        std::string auth_error;
        if (!request_response(
              auth_method,
              JsonValue(JsonValue::array{JsonValue(user), JsonValue(pass)}),
              &auth_resp,
              5000,
              include_jsonrpc,
              &auth_error)) {
          last_error = auth_error.empty() ? "authorize request failed" : auth_error;
          continue;
        }

        if (json_has_error(auth_resp)) {
          last_error = "authorize rejected: " + json_error_text(auth_resp);
          continue;
        }

        mode_ = Mode::MINING_SUBMIT;
        StratumJob first_job;
        std::string poll_error;
        if (!poll_job(&first_job, 5000, &poll_error)) {
          if (!looks_like_connection_problem(poll_error)) {
            last_error = poll_error.empty() ? "no initial mining job from pool" : poll_error;
          } else {
            last_error = poll_error;
          }
          continue;
        }
        queued_jobs_.insert(queued_jobs_.begin(), std::move(first_job));
        return true;
      }
    }
  }

  if (error_message != nullptr) {
    *error_message = last_error;
  }
  return false;
}

bool StratumClient::try_mining_auth_method(
  const std::string& user,
  const std::string& pass,
  std::string* error_message) {
  struct AuthReq {
    JsonValue params;
    bool include_jsonrpc = false;
  };

  std::vector<AuthReq> variants;
  variants.push_back(AuthReq{JsonValue(JsonValue::array{JsonValue(user), JsonValue(pass), JsonValue(kStratumAgent)}), false});
  variants.push_back(AuthReq{JsonValue(JsonValue::array{JsonValue(user), JsonValue(""), JsonValue(kStratumAgent)}), false});
  variants.push_back(AuthReq{JsonValue(JsonValue::array{JsonValue(user), JsonValue(pass)}), false});
  variants.push_back(AuthReq{JsonValue(JsonValue::array{JsonValue(user), JsonValue("")}), false});

  variants.push_back(AuthReq{JsonValue(JsonValue::array{JsonValue(user), JsonValue(pass), JsonValue(kStratumAgent)}), true});
  variants.push_back(AuthReq{JsonValue(JsonValue::array{JsonValue(user), JsonValue("")}), true});

  std::string last_error = "mining.auth failed";
  for (size_t i = 0; i < variants.size(); ++i) {
    if (i > 0) {
      (void)reconnect(nullptr);
    }

    JsonValue auth_resp;
    std::string auth_error;
    if (!request_response(
          "mining.auth",
          std::move(variants[i].params),
          &auth_resp,
          5000,
          variants[i].include_jsonrpc,
          &auth_error)) {
      last_error = auth_error.empty() ? "mining.auth request failed" : auth_error;
      continue;
    }
    if (json_has_error(auth_resp)) {
      last_error = "mining.auth rejected: " + json_error_text(auth_resp);
      continue;
    }

    mode_ = Mode::MINING_AUTH_SUBMIT_SHORT;
    StratumJob first_job;
    std::string poll_error;
    if (!poll_job(&first_job, 8000, &poll_error)) {
      last_error = poll_error.empty() ? "no initial mining job from pool" : poll_error;
      continue;
    }
    queued_jobs_.insert(queued_jobs_.begin(), std::move(first_job));
    return true;
  }

  if (error_message != nullptr) {
    *error_message = last_error;
  }
  return false;
}

bool StratumClient::try_authorize_only_method(
  const std::string& user,
  const std::string& pass,
  std::string* error_message) {
  const std::array<std::string, 3> authorize_methods{
    "mining.authorize",
    "Authorize",
    "authorize",
  };

  std::string last_error = "authorize failed";
  for (bool include_jsonrpc : {true, false}) {
    for (size_t i = 0; i < authorize_methods.size(); ++i) {
      if (i > 0 || !include_jsonrpc) {
        (void)reconnect(nullptr);
      }

      JsonValue auth_resp;
      std::string auth_error;
      if (!request_response(
            authorize_methods[i],
            JsonValue(JsonValue::array{JsonValue(user), JsonValue(pass)}),
            &auth_resp,
            5000,
            include_jsonrpc,
            &auth_error)) {
        last_error = auth_error.empty() ? "authorize request failed" : auth_error;
        continue;
      }
      if (json_has_error(auth_resp)) {
        last_error = "authorize rejected: " + json_error_text(auth_resp);
        continue;
      }

      mode_ = Mode::MINING_SUBMIT;
      StratumJob first_job;
      std::string poll_error;
      if (!poll_job(&first_job, 5000, &poll_error)) {
        last_error = poll_error.empty() ? "no initial mining job from pool" : poll_error;
        continue;
      }
      queued_jobs_.insert(queued_jobs_.begin(), std::move(first_job));
      return true;
    }
  }

  if (error_message != nullptr) {
    *error_message = last_error;
  }
  return false;
}

bool StratumClient::try_login_caps_method(
  const std::string& user,
  const std::string& pass,
  std::string* error_message) {

  std::vector<JsonValue> attempts;
  attempts.emplace_back(JsonValue::array{JsonValue(user)});
  attempts.emplace_back(JsonValue::array{JsonValue(user), JsonValue(pass)});
  attempts.emplace_back(JsonValue::array{JsonValue(user), JsonValue(pass), JsonValue(kStratumAgent)});
  attempts.emplace_back(JsonValue::object{
    {"login", JsonValue(user)},
    {"pass", JsonValue(pass)},
    {"agent", JsonValue(kStratumAgent)},
  });

  std::string last_error = "Login rejected";
  for (bool include_jsonrpc : {true, false}) {
    for (size_t i = 0; i < attempts.size(); ++i) {
      if (i > 0 || !include_jsonrpc) {
        (void)reconnect(nullptr);
      }

      JsonValue resp;
      std::string req_error;
      if (!request_response_or_job("Login", attempts[i], &resp, 5000, include_jsonrpc, &req_error)) {
        last_error = req_error.empty() ? "Login request failed" : req_error;
        continue;
      }

      if (resp.is_object() && json_has_error(resp)) {
        last_error = "Login rejected: " + json_error_text(resp);
        continue;
      }

      // Some pools send a mining.job notification without returning a matching id response.
      if (queued_jobs_.empty() && resp.is_object()) {
        const auto& obj = resp.as_object();
        const auto result_it = obj.find("result");
        if (result_it != obj.end() && result_it->second.is_object()) {
          const auto& result = result_it->second.as_object();
          const auto sid_it = result.find("id");
          if (sid_it != result.end()) {
            std::string sid;
            if (parse_json_rpc_id(sid_it->second, &sid)) {
              session_id_ = sid;
            }
          }
        }
      }

      mode_ = Mode::LOGIN_CAPS_SUBMIT;
      return true;
    }
  }

  if (error_message != nullptr) {
    *error_message = last_error;
  }
  return false;
}

bool StratumClient::poll_job(StratumJob* out_job, uint32_t timeout_ms, std::string* error_message) {
  if (!queued_jobs_.empty()) {
    *out_job = std::move(queued_jobs_.front());
    queued_jobs_.erase(queued_jobs_.begin());
    return true;
  }

  while (true) {
    ParsedMessage msg;
    if (!read_message(&msg, timeout_ms, error_message)) {
      return false;
    }
    if (!queued_jobs_.empty()) {
      *out_job = std::move(queued_jobs_.front());
      queued_jobs_.erase(queued_jobs_.begin());
      return true;
    }
  }
}

bool StratumClient::submit_share(
  const std::string& user,
  const std::string& job_id,
  const std::string& nonce_hex,
  const std::string& result_hex,
  bool* accepted,
  std::string* error_message) {

  if (accepted != nullptr) {
    *accepted = false;
  }

  struct SubmitAttempt {
    std::string method;
    JsonValue params;
    bool include_jsonrpc = true;
  };

  auto base_id = session_id_.value_or(user);
  JsonValue::object submit_object{
    {"job_id", JsonValue(job_id)},
    {"nonce", JsonValue(nonce_hex)},
    {"result", JsonValue(result_hex)},
  };
  JsonValue::array submit_array{
    JsonValue(base_id),
    JsonValue(job_id),
    JsonValue(nonce_hex),
    JsonValue(result_hex),
  };

  std::vector<SubmitAttempt> attempts;
  if (mode_ == Mode::LOGIN_SUBMIT) {
    JsonValue::object params = submit_object;
    params["id"] = JsonValue(base_id);
    attempts.push_back(SubmitAttempt{"submit", JsonValue(params), true});
    attempts.push_back(SubmitAttempt{"submit", JsonValue(params), false});
  } else if (mode_ == Mode::MINING_SUBMIT) {
    JsonValue::object params = submit_object;
    params["worker"] = JsonValue(user);
    attempts.push_back(SubmitAttempt{"mining.submit", JsonValue(params), true});
    attempts.push_back(SubmitAttempt{"mining.submit", JsonValue(submit_array), true});
    attempts.push_back(SubmitAttempt{"mining.submit", JsonValue(params), false});
    attempts.push_back(SubmitAttempt{"mining.submit", JsonValue(submit_array), false});
  } else if (mode_ == Mode::MINING_AUTH_SUBMIT_SHORT) {
    JsonValue::array submit_short{
      JsonValue(user),
      JsonValue(job_id),
      JsonValue(nonce_hex),
    };
    JsonValue::array submit_short_plus_result{
      JsonValue(user),
      JsonValue(job_id),
      JsonValue(nonce_hex),
      JsonValue(result_hex),
    };

    attempts.push_back(SubmitAttempt{"mining.submit", JsonValue(submit_short), false});
    attempts.push_back(SubmitAttempt{"mining.submit", JsonValue(submit_short), true});
    attempts.push_back(SubmitAttempt{"mining.submit", JsonValue(submit_short_plus_result), false});
    attempts.push_back(SubmitAttempt{"mining.submit", JsonValue(submit_short_plus_result), true});
  } else if (mode_ == Mode::LOGIN_CAPS_SUBMIT) {
    JsonValue::object params_with_id = submit_object;
    params_with_id["id"] = JsonValue(base_id);

    attempts.push_back(SubmitAttempt{"Submit", JsonValue(params_with_id), true});
    attempts.push_back(SubmitAttempt{"Submit", JsonValue(submit_array), true});
    attempts.push_back(SubmitAttempt{"submit", JsonValue(params_with_id), true});
    attempts.push_back(SubmitAttempt{"submit", JsonValue(submit_array), true});

    JsonValue::object mining_obj = submit_object;
    mining_obj["worker"] = JsonValue(user);
    attempts.push_back(SubmitAttempt{"mining.submit", JsonValue(mining_obj), true});
    attempts.push_back(SubmitAttempt{"mining.submit", JsonValue(submit_array), true});

    attempts.push_back(SubmitAttempt{"Submit", JsonValue(params_with_id), false});
    attempts.push_back(SubmitAttempt{"Submit", JsonValue(submit_array), false});
    attempts.push_back(SubmitAttempt{"submit", JsonValue(params_with_id), false});
    attempts.push_back(SubmitAttempt{"submit", JsonValue(submit_array), false});
    attempts.push_back(SubmitAttempt{"mining.submit", JsonValue(mining_obj), false});
    attempts.push_back(SubmitAttempt{"mining.submit", JsonValue(submit_array), false});
  } else {
    if (error_message != nullptr) {
      *error_message = "pool protocol mode not initialized";
    }
    return false;
  }

  auto is_retryable_error = [](const std::string& message) {
    std::string lower = message;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return lower.find("method not found") != std::string::npos ||
           lower.find("unknown method") != std::string::npos ||
           lower.find("invalid params") != std::string::npos ||
           lower.find("invalid argument") != std::string::npos;
  };

  JsonValue resp;
  std::string last_error;
  bool got_response = false;
  for (size_t i = 0; i < attempts.size(); ++i) {
    std::string req_error;
    if (!request_response(
          attempts[i].method,
          std::move(attempts[i].params),
          &resp,
          10000,
          attempts[i].include_jsonrpc,
          &req_error)) {
      if (mode_ == Mode::LOGIN_CAPS_SUBMIT && i + 1 < attempts.size()) {
        last_error = req_error;
        continue;
      }
      if (error_message != nullptr) {
        *error_message = req_error;
      }
      return false;
    }

    got_response = true;
    if (!json_has_error(resp)) {
      break;
    }

    const std::string err = json_error_text(resp);
    last_error = err;
    if (i + 1 < attempts.size() && is_retryable_error(err)) {
      continue;
    }
    break;
  }

  if (!got_response) {
    if (error_message != nullptr) {
      *error_message = last_error.empty() ? "submit failed" : last_error;
    }
    return false;
  }

  if (json_has_error(resp)) {
    if (error_message != nullptr) {
      *error_message = "share rejected: " + json_error_text(resp);
    }
    if (accepted != nullptr) {
      *accepted = false;
    }
    return true;
  }

  bool ok = true;
  if (resp.is_object()) {
    const auto& obj = resp.as_object();
    const auto res_it = obj.find("result");
    if (res_it != obj.end()) {
      if (res_it->second.is_bool()) {
        ok = res_it->second.as_bool();
      } else if (res_it->second.is_object()) {
        const auto& r = res_it->second.as_object();
        const auto status_it = r.find("status");
        if (status_it != r.end() && status_it->second.is_string()) {
          ok = status_it->second.as_string() == "OK" || status_it->second.as_string() == "ok";
        } else {
          ok = true;
        }
      } else {
        ok = true;
      }
    }
  }

  if (accepted != nullptr) {
    *accepted = ok;
  }
  return true;
}

bool StratumClient::parse_job_from_message(const JsonValue& msg, StratumJob* out_job) {
  if (!msg.is_object()) {
    return false;
  }
  const auto& obj = msg.as_object();
  const auto method_it = obj.find("method");
  if (method_it != obj.end() && method_it->second.is_string()) {
    const std::string method = method_it->second.as_string();
    if (method == "job") {
      const auto params_it = obj.find("params");
      if (params_it != obj.end() && params_it->second.is_object()) {
        return parse_job_from_object(params_it->second.as_object(), out_job);
      }
    }
    if (method == "mining.job") {
      const auto params_it = obj.find("params");
      if (params_it != obj.end()) {
        return parse_job_from_mining_job_params(params_it->second, out_job);
      }
    }
    if (method == "mining.notify") {
      const auto params_it = obj.find("params");
      if (params_it != obj.end()) {
        return parse_job_from_notify_params(params_it->second, out_job);
      }
    }
  }

  const auto result_it = obj.find("result");
  if (result_it != obj.end() && result_it->second.is_object()) {
    const auto& result = result_it->second.as_object();
    const auto job_it = result.find("job");
    if (job_it != result.end() && job_it->second.is_object()) {
      return parse_job_from_object(job_it->second.as_object(), out_job);
    }
  }

  return false;
}

bool StratumClient::parse_job_from_notify_params(const JsonValue& params, StratumJob* out_job) {
  if (params.is_object()) {
    return parse_job_from_object(params.as_object(), out_job);
  }
  if (!params.is_array()) {
    return false;
  }
  const auto& arr = params.as_array();
  if (arr.size() < 3 || !arr[0].is_string() || !arr[1].is_string()) {
    return false;
  }

  JsonValue::object obj;
  obj["job_id"] = JsonValue(arr[0].as_string());
  obj["blob"] = JsonValue(arr[1].as_string());
  if (arr[2].is_string()) {
    obj["target"] = JsonValue(arr[2].as_string());
  } else if (arr[2].is_number()) {
    obj["difficulty"] = JsonValue(arr[2].as_double());
  }
  return parse_job_from_object(obj, out_job);
}

bool StratumClient::parse_job_from_mining_job_params(const JsonValue& params, StratumJob* out_job) {
  if (params.is_object()) {
    return parse_job_from_object(params.as_object(), out_job);
  }
  if (!params.is_array()) {
    return false;
  }

  const auto& arr = params.as_array();
  if (arr.size() < 2 || !arr[0].is_string() || !arr[1].is_string()) {
    return false;
  }

  JsonValue::object obj;
  obj["job_id"] = JsonValue(arr[0].as_string());
  obj["blob"] = JsonValue(arr[1].as_string());
  bool clean_jobs = false;
  bool have_clean_jobs = false;

  bool set_nonce_offset = false;
  bool set_target_or_diff = false;
  for (size_t i = 2; i < arr.size(); ++i) {
    const JsonValue& v = arr[i];

    if (!have_clean_jobs && v.is_bool()) {
      clean_jobs = v.as_bool();
      have_clean_jobs = true;
      continue;
    }

    uint64_t n = 0;
    if (!set_nonce_offset && json_number_to_u64(v, &n) && n <= 512) {
      obj["nonce_offset"] = JsonValue(static_cast<uint64_t>(n));
      set_nonce_offset = true;
      continue;
    }

    if (!set_target_or_diff && v.is_string()) {
      std::vector<uint8_t> hex_bytes;
      if (parse_hex(v.as_string(), &hex_bytes) && !hex_bytes.empty() && hex_bytes.size() <= 32) {
        obj["target"] = JsonValue(v.as_string());
        set_target_or_diff = true;
        continue;
      }
    }

    double d = 0.0;
    if (!set_target_or_diff && json_number_to_double(v, &d) && d > 0.0) {
      obj["difficulty"] = JsonValue(d);
      set_target_or_diff = true;
      continue;
    }
  }

  if (have_clean_jobs) {
    obj["clean_jobs"] = JsonValue(clean_jobs);
  }

  return parse_job_from_object(obj, out_job);
}

bool StratumClient::parse_target_le(const JsonValue::object& obj, uint64_t* out_target_le) {
  uint64_t diff_target = 0;
  bool has_diff_target = false;
  const auto diff_it = obj.find("difficulty");
  if (diff_it != obj.end() && diff_it->second.is_number()) {
    diff_target = difficulty_to_target_le(diff_it->second.as_double());
    has_diff_target = diff_target != 0;
  } else if (difficulty_.has_value()) {
    diff_target = difficulty_to_target_le(*difficulty_);
    has_diff_target = diff_target != 0;
  }

  const auto target_it = obj.find("target");
  if (target_it != obj.end() && target_it->second.is_string()) {
    std::vector<uint8_t> target_bytes;
    if (parse_hex(target_it->second.as_string(), &target_bytes) && !target_bytes.empty()) {
      const uint64_t candidate_le = parse_target_prefix_le(target_bytes);
      if (target_bytes.size() <= 8) {
        *out_target_le = candidate_le;
        return candidate_le != 0;
      }

      // Some pools provide 32-byte target in big-endian form, while others use a
      // short little-endian target. Prefer whichever interpretation matches
      // advertised difficulty when available.
      const uint64_t candidate_be = parse_target_prefix_be(target_bytes);
      uint64_t chosen = candidate_le;

      if (has_diff_target) {
        const uint64_t d_le = distance_u64(candidate_le, diff_target);
        const uint64_t d_be = distance_u64(candidate_be, diff_target);
        chosen = (d_be < d_le) ? candidate_be : candidate_le;
      } else {
        const bool looks_be = target_bytes.size() > 8 && target_bytes[0] == 0;
        if (looks_be && candidate_be != 0) {
          chosen = candidate_be;
        }
      }

      if (chosen != 0) {
        *out_target_le = chosen;
        return true;
      }
    }
  }

  if (has_diff_target) {
    *out_target_le = diff_target;
    return true;
  }

  return false;
}

bool StratumClient::parse_job_from_object(const JsonValue::object& obj, StratumJob* out_job) {
  const auto blob_it = obj.find("blob");
  const auto job_it = obj.find("job_id");
  if (blob_it == obj.end() || job_it == obj.end() || !blob_it->second.is_string() || !job_it->second.is_string()) {
    return false;
  }

  StratumJob job;
  job.job_id = job_it->second.as_string();
  if (!parse_hex(blob_it->second.as_string(), &job.blob) || job.blob.empty()) {
    return false;
  }

  if (const auto h = obj.find("height"); h != obj.end() && h->second.is_number()) {
    if (h->second.is_uint64()) {
      job.height = h->second.as_uint64();
    } else if (h->second.is_int64() && h->second.as_int64() >= 0) {
      job.height = static_cast<uint64_t>(h->second.as_int64());
    }
  }

  if (const auto clean = obj.find("clean_jobs"); clean != obj.end()) {
    if (clean->second.is_bool()) {
      job.clean_jobs = clean->second.as_bool();
    } else if (clean->second.is_number()) {
      job.clean_jobs = clean->second.as_double() != 0.0;
    }
  } else {
    job.clean_jobs = false;
  }

  if (const auto target = obj.find("target"); target != obj.end() && target->second.is_string()) {
    std::vector<uint8_t> target_bytes;
    if (parse_hex(target->second.as_string(), &target_bytes) && target_bytes.size() == 32) {
      std::copy(target_bytes.begin(), target_bytes.end(), job.target_be.begin());
      job.has_target_be = true;
    }
  }

  if (const auto no = obj.find("nonce_offset"); no != obj.end() && no->second.is_uint64()) {
    job.nonce_offset = static_cast<uint32_t>(no->second.as_uint64());
  } else if (const auto ro = obj.find("reserved_offset"); ro != obj.end() && ro->second.is_uint64()) {
    job.nonce_offset = static_cast<uint32_t>(ro->second.as_uint64());
  } else {
    job.nonce_offset = 39;
  }

  if (job.nonce_offset + 4 > job.blob.size()) {
    return false;
  }

  if (!parse_target_le(obj, &job.target_le) || job.target_le == 0) {
    if (!job.has_target_be) {
      return false;
    }
  }

  *out_job = std::move(job);
  return true;
}

bool StratumClient::parse_hex(const std::string& hex, std::vector<uint8_t>* out) const {
  std::string clean = trim(hex);
  if (clean.size() >= 2 && clean[0] == '0' && (clean[1] == 'x' || clean[1] == 'X')) {
    clean = clean.substr(2);
  }
  if (clean.size() % 2 != 0) {
    return false;
  }

  out->clear();
  out->reserve(clean.size() / 2);
  auto hex_val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };

  for (size_t i = 0; i < clean.size(); i += 2) {
    const int hi = hex_val(clean[i]);
    const int lo = hex_val(clean[i + 1]);
    if (hi < 0 || lo < 0) {
      out->clear();
      return false;
    }
    out->push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

std::string StratumClient::trim(const std::string& s) const {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])) != 0) {
    ++start;
  }
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
    --end;
  }
  return s.substr(start, end - start);
}

bool is_stratum_pool_host(const std::string& host, uint16_t port) {
  std::string lower = host;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (lower.find("luckypool.io") != std::string::npos) {
    return true;
  }
  if (port == 5110 || port == 5111) {
    return true;
  }
  return false;
}

} // namespace scrig
