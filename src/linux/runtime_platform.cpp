#include "scrig/runtime_platform.hpp"

#include "scrig/miner.hpp"
#include "scrig/node_client.hpp"

#include <atomic>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <sys/select.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

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
  ~Impl() {
    stop();
  }

  bool start(Miner& miner, std::string* error_message) {
    miner_ = &miner;
    running_.store(true, std::memory_order_relaxed);

    tty_fd_ = ::open("/dev/tty", O_RDONLY);
    bool using_stdin_dup = false;
    if (tty_fd_ < 0 && isatty(STDIN_FILENO)) {
      tty_fd_ = ::dup(STDIN_FILENO);
      using_stdin_dup = true;
    }
    if (tty_fd_ < 0) {
      running_.store(false, std::memory_order_relaxed);
      if (error_message != nullptr) {
        *error_message = "failed to open terminal input device";
      }
      return false;
    }

    if (!enable_unix_raw_mode()) {
      ::close(tty_fd_);
      tty_fd_ = -1;
      if (!using_stdin_dup && isatty(STDIN_FILENO)) {
        tty_fd_ = ::dup(STDIN_FILENO);
        using_stdin_dup = true;
      }
      if (tty_fd_ < 0 || !enable_unix_raw_mode()) {
        if (tty_fd_ >= 0) {
          ::close(tty_fd_);
          tty_fd_ = -1;
        }
        running_.store(false, std::memory_order_relaxed);
        if (error_message != nullptr) {
          *error_message = "failed to enable raw terminal mode";
        }
        return false;
      }
    }

    worker_ = std::thread([this]() { run(); });
    return true;
  }

  void stop() {
    running_.store(false, std::memory_order_relaxed);
    if (worker_.joinable()) {
      worker_.join();
    }
    disable_unix_raw_mode();
    if (tty_fd_ >= 0) {
      ::close(tty_fd_);
      tty_fd_ = -1;
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

  bool enable_unix_raw_mode() {
    if (raw_mode_enabled_) {
      return true;
    }
    if (tty_fd_ < 0) {
      return false;
    }
    if (tcgetattr(tty_fd_, &saved_termios_) != 0) {
      return false;
    }

    termios raw = saved_termios_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(tty_fd_, TCSANOW, &raw) != 0) {
      return false;
    }

    raw_mode_enabled_ = true;
    return true;
  }

  void disable_unix_raw_mode() {
    if (!raw_mode_enabled_) {
      return;
    }
    if (tty_fd_ >= 0) {
      (void)tcsetattr(tty_fd_, TCSANOW, &saved_termios_);
    }
    raw_mode_enabled_ = false;
  }

  void run() {
    while (running_.load(std::memory_order_relaxed)) {
      if (tty_fd_ < 0) {
        return;
      }
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(tty_fd_, &readfds);
      timeval timeout{};
      timeout.tv_sec = 0;
      timeout.tv_usec = 100000;

      const int ready = select(tty_fd_ + 1, &readfds, nullptr, nullptr, &timeout);
      if (ready <= 0 || !FD_ISSET(tty_fd_, &readfds)) {
        continue;
      }

      unsigned char ch = 0;
      const ssize_t n = ::read(tty_fd_, &ch, 1);
      if (n != 1) {
        continue;
      }

      handle_hotkey(ch);
      if (!running_.load(std::memory_order_relaxed)) {
        return;
      }
    }
  }

  Miner* miner_ = nullptr;
  std::atomic<bool> running_{false};
  std::thread worker_{};
  bool raw_mode_enabled_ = false;
  int tty_fd_ = -1;
  termios saved_termios_{};
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
