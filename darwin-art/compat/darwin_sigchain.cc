#include <signal.h>

#include <array>
#include <cstring>

#include "sigchain.h"

namespace art {
namespace {

struct SignalSlot {
  SigchainAction special{};
  struct sigaction previous {};
  bool installed = false;
};

std::array<SignalSlot, NSIG> g_signal_slots;

void DarwinSignalDispatcher(int signal_number, siginfo_t* info, void* context) {
  SignalSlot& slot = g_signal_slots[signal_number];
  if (slot.special.sc_sigaction != nullptr &&
      slot.special.sc_sigaction(signal_number, info, context)) {
    return;
  }

  const struct sigaction& previous = slot.previous;
  if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
    previous.sa_sigaction(signal_number, info, context);
  } else if (previous.sa_handler == SIG_IGN) {
    return;
  } else if (previous.sa_handler != nullptr && previous.sa_handler != SIG_DFL) {
    previous.sa_handler(signal_number);
  } else {
    signal(signal_number, SIG_DFL);
    raise(signal_number);
  }
}

}  // namespace

extern "C" void AddSpecialSignalHandlerFn(int signal_number, SigchainAction* action) {
  if (signal_number <= 0 || signal_number >= NSIG || action == nullptr) {
    return;
  }
  SignalSlot& slot = g_signal_slots[signal_number];
  slot.special = *action;

  struct sigaction dispatcher {};
  dispatcher.sa_sigaction = DarwinSignalDispatcher;
  dispatcher.sa_mask = action->sc_mask;
  dispatcher.sa_flags = SA_SIGINFO | SA_RESTART;
  if (sigaction(signal_number, &dispatcher, &slot.previous) == 0) {
    slot.installed = true;
  }
}

extern "C" void RemoveSpecialSignalHandlerFn(
    int signal_number, bool (*handler)(int, siginfo_t*, void*)) {
  if (signal_number <= 0 || signal_number >= NSIG) {
    return;
  }
  SignalSlot& slot = g_signal_slots[signal_number];
  if (slot.installed && slot.special.sc_sigaction == handler) {
    sigaction(signal_number, &slot.previous, nullptr);
    slot = {};
  }
}

}  // namespace art
