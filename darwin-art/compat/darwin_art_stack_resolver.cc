#include "stack.h"
#include "thread.h"

#include <string>

extern "C" void darwin_art_walk_managed_frames(void (*callback)(const char*, void*),
                                                 void* context) {
  if (callback == nullptr) return;
  art::Thread* self = art::Thread::Current();
  if (self == nullptr) return;
  if (art::ArtMethod* current = self->GetCurrentMethod(
          nullptr, /*check_suspended=*/false, /*abort_on_error=*/false);
      current != nullptr) {
    std::string name = current->PrettyMethod(/*with_signature=*/false);
    callback(name.c_str(), context);
  }
  art::StackVisitor::WalkStack(
      [callback, context](art::StackVisitor* visitor) {
        art::ArtMethod* method = visitor->GetMethod();
        if (method != nullptr) {
          std::string name = method->PrettyMethod(/*with_signature=*/false);
          callback(name.c_str(), context);
        }
        return true;
      },
      self,
      nullptr,
      art::StackVisitor::StackWalkKind::kIncludeInlinedFrames,
      /*check_suspended=*/false,
      /*include_transitions=*/true);
}
