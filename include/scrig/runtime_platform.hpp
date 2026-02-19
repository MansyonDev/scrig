#pragma once

#include <string>

namespace scrig {

class Miner;

bool enable_console_ansi();
[[noreturn]] void force_process_exit();
void clear_console_fallback();

class QuitHotkeyWatcher {
public:
  ~QuitHotkeyWatcher();

  bool start(Miner& miner, std::string* error_message = nullptr);
  void stop();

private:
  class Impl;
  Impl* impl_ = nullptr;
};

} // namespace scrig
