#include "runtime_graphics_session.h"

#include <pthread.h>

#include <cstdint>
#include <mutex>
#include <new>

#include "runtime_graphics_probe.h"
#include "runtime_graphics_state.h"
#include "runtime_process_state.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-current-inl.h"

struct darwin_art_graphics_session_t {
  pthread_t owner_thread{};
  art::Thread* owner_art_thread = nullptr;
  bool bound_to_process = false;
  bool closed = false;
};

namespace darwin_art_graphics {
namespace {

std::mutex g_session_mutex;
darwin_art_graphics_session_t* g_active_session = nullptr;

int32_t check_owner(const darwin_art_graphics_session_t* session) {
  if (session == nullptr) {
    return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
  }
  if (pthread_equal(session->owner_thread, pthread_self()) == 0 ||
      art::Thread::Current() != session->owner_art_thread ||
      session->owner_art_thread == nullptr) {
    return DARWIN_ART_STATUS_GRAPHICS_SESSION_WRONG_THREAD;
  }
  return 0;
}

int32_t check_active_locked(const darwin_art_graphics_session_t* session) {
  if (session == nullptr || session != g_active_session) {
    return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
  }
  if (session->closed) return DARWIN_ART_STATUS_GRAPHICS_SESSION_CLOSED;
  return 0;
}

}  // namespace

darwin_art_graphics_session_t* create_session() {
  std::lock_guard<std::mutex> lock(g_session_mutex);
  if (g_active_session != nullptr) return nullptr;
  auto* session = new (std::nothrow) darwin_art_graphics_session_t;
  if (session == nullptr) return nullptr;
  session->owner_thread = pthread_self();
  g_active_session = session;
  return session;
}

int32_t bind_session_for_process(void* context) {
  if (context == nullptr) return 0;
  std::lock_guard<std::mutex> lock(g_session_mutex);
  auto* session = static_cast<darwin_art_graphics_session_t*>(context);
  if (session != g_active_session ||
      pthread_equal(session->owner_thread, pthread_self()) == 0 ||
      session->closed || session->bound_to_process) {
    return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
  }
  session->bound_to_process = true;
  return 0;
}

int32_t bind_session_art_thread(art::Thread* thread) {
  if (thread == nullptr) return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
  std::lock_guard<std::mutex> lock(g_session_mutex);
  if (g_active_session == nullptr || !g_active_session->bound_to_process ||
      g_active_session->closed ||
      pthread_equal(g_active_session->owner_thread, pthread_self()) == 0) {
    return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
  }
  g_active_session->owner_art_thread = thread;
  return 0;
}

int32_t close_session(darwin_art_graphics_session_t* session) {
  art::Thread* owner = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    if (session == nullptr || session != g_active_session) {
      return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
    }
    if (session->closed) return DARWIN_ART_STATUS_GRAPHICS_SESSION_CLOSED;
    if (pthread_equal(session->owner_thread, pthread_self()) == 0 ||
        (session->owner_art_thread != nullptr &&
         art::Thread::Current() != session->owner_art_thread)) {
      return DARWIN_ART_STATUS_GRAPHICS_SESSION_WRONG_THREAD;
    }
    owner = session->owner_art_thread;
    if (owner != nullptr && owner->GetState() != art::ThreadState::kNative) {
      return DARWIN_ART_STATUS_GRAPHICS_SESSION_WRONG_THREAD;
    }
    session->closed = true;
  }
  // Native graphics teardown is performed by run_shutdown while the ART
  // runtime is still in its canonical shutdown transaction. Closing the
  // opaque handle here only stops new calls; doing JNI/HWUI destruction in a
  // separate pre-shutdown transaction leaves Runtime::DestroyJavaVM with an
  // attached-thread state it cannot safely detach.
  (void)owner;
  return 0;
}

int32_t destroy_session(darwin_art_graphics_session_t* session) {
  std::lock_guard<std::mutex> lock(g_session_mutex);
  if (session == nullptr || session != g_active_session) {
    return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
  }
  if (pthread_equal(session->owner_thread, pthread_self()) == 0 ||
      (session->owner_art_thread != nullptr &&
       art::Thread::Current() != session->owner_art_thread)) {
    return DARWIN_ART_STATUS_GRAPHICS_SESSION_WRONG_THREAD;
  }
  if (!session->closed) {
    return DARWIN_ART_STATUS_GRAPHICS_SESSION_NOT_CLOSED;
  }
  g_active_session = nullptr;
  delete session;
  return 0;
}

int32_t dispatch_pointer(darwin_art_graphics_session_t* session,
                         uint32_t action, float x, float y) {
  {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    const int32_t active_status = check_active_locked(session);
    if (active_status != 0) return active_status;
    const int32_t owner_status = check_owner(session);
    if (owner_status != 0) return owner_status;
  }
  return darwin_art_dispatch_pointer(action, x, y);
}

int32_t pump_frame(darwin_art_graphics_session_t* session,
                   int64_t frame_time_nanos) {
  {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    const int32_t active_status = check_active_locked(session);
    if (active_status != 0) return active_status;
    const int32_t owner_status = check_owner(session);
    if (owner_status != 0) return owner_status;
  }
  return darwin_art_pump_framework_frame(frame_time_nanos);
}

}  // namespace darwin_art_graphics

extern "C" DARWIN_ART_EXPORT darwin_art_graphics_session_t*
darwin_art_graphics_session_create() {
  return darwin_art_graphics::create_session();
}

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_graphics_session_close(
    darwin_art_graphics_session_t* session) {
  return darwin_art_graphics::close_session(session);
}

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_graphics_session_destroy(
    darwin_art_graphics_session_t* session) {
  return darwin_art_graphics::destroy_session(session);
}

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_graphics_session_dispatch_pointer(
    darwin_art_graphics_session_t* session, uint32_t action, float x, float y) {
  return darwin_art_graphics::dispatch_pointer(session, action, x, y);
}

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_graphics_session_pump_frame(
    darwin_art_graphics_session_t* session, int64_t frame_time_nanos) {
  return darwin_art_graphics::pump_frame(session, frame_time_nanos);
}
