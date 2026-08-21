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

}  // namespace

darwin_art_graphics_session_t* create_session() {
  art::Thread* owner = darwin_art_process::owner_thread_for_callback();
  if (owner == nullptr || art::Thread::Current() != owner ||
      interactive_root_for_state() == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(g_session_mutex);
  if (g_active_session != nullptr) return nullptr;
  auto* session = new (std::nothrow) darwin_art_graphics_session_t;
  if (session == nullptr) return nullptr;
  session->owner_thread = pthread_self();
  session->owner_art_thread = owner;
  g_active_session = session;
  return session;
}

int32_t close_session(darwin_art_graphics_session_t* session) {
  std::lock_guard<std::mutex> lock(g_session_mutex);
  if (session == nullptr) return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
  if (session != g_active_session) {
    return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
  }
  const int32_t owner_status = check_owner(session);
  if (owner_status != 0) return owner_status;
  if (session->closed) return DARWIN_ART_STATUS_GRAPHICS_SESSION_CLOSED;
  if (session->owner_art_thread->GetState() != art::ThreadState::kNative) {
    return DARWIN_ART_STATUS_GRAPHICS_SESSION_WRONG_THREAD;
  }
  art::ScopedObjectAccess soa(session->owner_art_thread);
  darwin_art_graphics::shutdown(session->owner_art_thread->GetJniEnv());
  session->closed = true;
  return 0;
}

int32_t destroy_session(darwin_art_graphics_session_t* session) {
  std::lock_guard<std::mutex> lock(g_session_mutex);
  if (session == nullptr || session != g_active_session) {
    return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
  }
  const int32_t owner_status = check_owner(session);
  if (owner_status != 0) return owner_status;
  if (!session->closed) {
    return DARWIN_ART_STATUS_GRAPHICS_SESSION_NOT_CLOSED;
  }
  g_active_session = nullptr;
  delete session;
  return 0;
}

int32_t dispatch_pointer(darwin_art_graphics_session_t* session,
                         uint32_t action, float x, float y) {
  std::lock_guard<std::mutex> lock(g_session_mutex);
  if (session == nullptr || session != g_active_session) {
    return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
  }
  const int32_t owner_status = check_owner(session);
  if (owner_status != 0) return owner_status;
  if (session->closed) return DARWIN_ART_STATUS_GRAPHICS_SESSION_CLOSED;
  return darwin_art_dispatch_pointer(action, x, y);
}

int32_t pump_frame(darwin_art_graphics_session_t* session,
                   int64_t frame_time_nanos) {
  std::lock_guard<std::mutex> lock(g_session_mutex);
  if (session == nullptr || session != g_active_session) {
    return DARWIN_ART_STATUS_GRAPHICS_SESSION_INVALID;
  }
  const int32_t owner_status = check_owner(session);
  if (owner_status != 0) return owner_status;
  if (session->closed) return DARWIN_ART_STATUS_GRAPHICS_SESSION_CLOSED;
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
