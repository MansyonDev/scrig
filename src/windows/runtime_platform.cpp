#include "scrig/runtime_platform.hpp"

#include "scrig/miner.hpp"
#include "scrig/node_client.hpp"

#include <atomic>
#include <conio.h>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <windows.h>

namespace scrig {

bool enable_console_ansi() {
  HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h_out == INVALID_HANDLE_VALUE) {
    return false;
  }

  DWORD mode = 0;
  if (!GetConsoleMode(h_out, &mode)) {
    return false;
  }

  const DWORD requested = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
  if (!SetConsoleMode(h_out, requested)) {
    if (!SetConsoleMode(h_out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
      return false;
    }
  }
  return true;
}

[[noreturn]] void force_process_exit() {
  (void)TerminateProcess(GetCurrentProcess(), 0);
  std::_Exit(0);
}

void clear_console_fallback() {
  HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h_out == INVALID_HANDLE_VALUE) {
    return;
  }

  CONSOLE_SCREEN_BUFFER_INFO csbi{};
  if (!GetConsoleScreenBufferInfo(h_out, &csbi)) {
    return;
  }

  const DWORD cell_count = static_cast<DWORD>(csbi.dwSize.X) * static_cast<DWORD>(csbi.dwSize.Y);
  const COORD origin{0, 0};
  DWORD written = 0;
  (void)FillConsoleOutputCharacterA(h_out, ' ', cell_count, origin, &written);
  (void)FillConsoleOutputAttribute(h_out, csbi.wAttributes, cell_count, origin, &written);
  (void)SetConsoleCursorPosition(h_out, origin);
}

class QuitHotkeyWatcher::Impl {
public:
  ~Impl() {
    stop();
  }

  bool start(Miner& miner, std::string*) {
    miner_ = &miner;
    running_.store(true, std::memory_order_relaxed);
    HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
    if (h_in != INVALID_HANDLE_VALUE) {
      (void)FlushConsoleInputBuffer(h_in);
    }
    worker_ = std::thread([this]() { run(); });
    return true;
  }

  void stop() {
    running_.store(false, std::memory_order_relaxed);
    if (worker_.joinable()) {
      worker_.join();
    }
  }

private:
  void trigger_stop() {
    if (miner_ != nullptr) {
      miner_->request_stop();
    }
    NodeClient::interrupt_all();
    running_.store(false, std::memory_order_relaxed);
  }

  void handle_hotkey(unsigned char ch) {
    if (ch == 17) {
      trigger_stop();
      force_process_exit();
    }

    if (ch == static_cast<unsigned char>('q') || ch == static_cast<unsigned char>('Q')) {
      trigger_stop();
      force_process_exit();
    }

    if (miner_ == nullptr) {
      return;
    }

    if (ch == static_cast<unsigned char>('h') || ch == static_cast<unsigned char>('H')) {
      miner_->report_hashrate_now();
      return;
    }
    if (ch == static_cast<unsigned char>('p') || ch == static_cast<unsigned char>('P')) {
      miner_->request_pause();
      return;
    }
    if (ch == static_cast<unsigned char>('r') || ch == static_cast<unsigned char>('R')) {
      miner_->request_resume();
      return;
    }
  }

  void run() {
    while (running_.load(std::memory_order_relaxed)) {
      if (_kbhit() == 0) {
        Sleep(25);
        continue;
      }

      const int raw = _getch();
      if (raw == 0 || raw == 224) {
        if (_kbhit() != 0) {
          (void)_getch();
        }
        continue;
      }

      if (raw == 3 || raw == 17) {
        trigger_stop();
        return;
      }

      handle_hotkey(static_cast<unsigned char>(raw));
      if (!running_.load(std::memory_order_relaxed)) {
        return;
      }
    }
  }

  Miner* miner_ = nullptr;
  std::atomic<bool> running_{false};
  std::thread worker_{};
};

QuitHotkeyWatcher::~QuitHotkeyWatcher() {
  stop();
}

bool QuitHotkeyWatcher::start(Miner& miner, std::string* error_message) {
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
