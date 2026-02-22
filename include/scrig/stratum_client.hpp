#pragma once

#include "scrig/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace scrig {

struct StratumJob {
  std::string job_id;
  std::vector<uint8_t> blob;
  uint32_t nonce_offset = 39;
  uint64_t target_le = 0;
  std::array<uint8_t, 32> target_be{};
  bool has_target_be = false;
  uint64_t height = 0;
  bool clean_jobs = false;
};

class StratumClient {
public:
  enum class Mode {
    UNKNOWN,
    LOGIN_SUBMIT,
    MINING_SUBMIT,
    MINING_AUTH_SUBMIT_SHORT,
    LOGIN_CAPS_SUBMIT,
  };

  StratumClient(std::string host, uint16_t port);
  ~StratumClient();

  StratumClient(const StratumClient&) = delete;
  StratumClient& operator=(const StratumClient&) = delete;

  void connect();
  void disconnect();

  bool login(const std::string& user, const std::string& pass, std::string* error_message);
  bool poll_job(StratumJob* out_job, uint32_t timeout_ms, std::string* error_message);
  bool submit_share(
    const std::string& user,
    const std::string& job_id,
    const std::string& nonce_hex,
    const std::string& result_hex,
    bool* accepted,
    std::string* error_message);

  Mode mode() const { return mode_; }
  bool has_session_id() const { return session_id_.has_value(); }

private:
  struct ParsedMessage;

  bool send_json_line(const JsonValue& value);
  bool read_json_line(JsonValue* out, uint32_t timeout_ms, std::string* error_message);
  bool read_message(ParsedMessage* out, uint32_t timeout_ms, std::string* error_message);
  bool request_response(
    const std::string& method,
    JsonValue params,
    JsonValue* response,
    uint32_t timeout_ms,
    bool include_jsonrpc,
    std::string* error_message);
  bool try_login_method(const std::string& user, const std::string& pass, std::string* error_message);
  bool try_mining_method(const std::string& user, const std::string& pass, std::string* error_message);
  bool try_mining_auth_method(const std::string& user, const std::string& pass, std::string* error_message);
  bool try_authorize_only_method(const std::string& user, const std::string& pass, std::string* error_message);
  bool try_login_caps_method(const std::string& user, const std::string& pass, std::string* error_message);
  bool request_response_or_job(
    const std::string& method,
    JsonValue params,
    JsonValue* response,
    uint32_t timeout_ms,
    bool include_jsonrpc,
    std::string* error_message);
  bool reconnect(std::string* error_message);

  bool parse_job_from_message(const JsonValue& msg, StratumJob* out_job);
  bool parse_job_from_object(const JsonValue::object& obj, StratumJob* out_job);
  bool parse_job_from_notify_params(const JsonValue& params, StratumJob* out_job);
  bool parse_job_from_mining_job_params(const JsonValue& params, StratumJob* out_job);
  bool parse_target_le(const JsonValue::object& obj, uint64_t* out_target_le);
  bool parse_hex(const std::string& hex, std::vector<uint8_t>* out) const;
  std::string trim(const std::string& s) const;

  std::string host_;
  uint16_t port_ = 0;
  std::intptr_t socket_fd_ = -1;
  uint64_t next_request_id_ = 1;
  Mode mode_ = Mode::UNKNOWN;
  std::optional<std::string> session_id_;
  std::optional<double> difficulty_;
  std::vector<StratumJob> queued_jobs_;
  std::string rx_buffer_;
};

bool is_stratum_pool_host(const std::string& host, uint16_t port);

} // namespace scrig
