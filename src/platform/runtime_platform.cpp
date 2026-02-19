#include "scrig/runtime_platform.hpp"

#include "scrig/miner.hpp"
#include "scrig/node_client.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace scrig {

bool enable_console_ansi() {
  return true;
}

[[noreturn]] void force_process_exit() {
  std::_Exit(0);
}

void clear_console_fallback() {
  std::cout << "\x1b[2J\x1b[H";
}

class QuitHotkeyWatcher::Impl {
public:
  bool start(Miner&, std::string* error_message) {
    if (error_message != nullptr) {
      *error_message = "hotkeys are unavailable on this platform build";
    }
    return false;
  }

  void stop() {
  }
};

QuitHotkeyWatcher::~QuitHotkeyWatcher() {
  stop();
}

bool QuitHotkeyWatcher::start(Miner& miner, std::string* error_message) {
  (void)miner;
  stop();
  impl_ = new Impl();
  if (!impl_->start(miner, error_message)) {
    delete impl_;
    impl_ = nullptr;
    return false;
  }
  return true;
}

void QuitHotkeyWatcher::stop() {
  if (impl_ == nullptr) {
    return;
  }
  impl_->stop();
  delete impl_;
  impl_ = nullptr;
}

} // namespace scrig
