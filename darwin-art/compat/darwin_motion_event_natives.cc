#include "darwin_motion_event_natives.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <iterator>
#include <vector>

namespace {

constexpr jint kActionMask = 0xff;
constexpr jint kToolTypeFinger = 1;
constexpr jint kAxisX = 0;
constexpr jint kAxisY = 1;
constexpr jint kAxisPressure = 2;
constexpr jint kAxisSize = 3;
constexpr jint kAxisTouchMajor = 4;
constexpr jint kAxisTouchMinor = 5;
constexpr jint kAxisToolMajor = 6;
constexpr jint kAxisToolMinor = 7;
constexpr jint kAxisOrientation = 8;
constexpr jint kAxisRelativeX = 27;
constexpr jint kAxisRelativeY = 28;
constexpr jint kMaxPointers = 16;

struct DarwinMotionPointer {
  jint id = 0;
  jint tool_type = kToolTypeFinger;
  float x = 0.0f;
  float y = 0.0f;
  float pressure = 1.0f;
  float size = 1.0f;
  float touch_major = 1.0f;
  float touch_minor = 1.0f;
  float tool_major = 1.0f;
  float tool_minor = 1.0f;
  float orientation = 0.0f;
  float relative_x = 0.0f;
  float relative_y = 0.0f;
};

struct DarwinMotionEvent {
  jint id = 0;
  jint device_id = 0;
  jint source = 0x1002;  // SOURCE touchscreen | SOURCE_CLASS_POINTER
  jint display_id = 0;
  jint action = 0;
  jint flags = 0;
  jint edge_flags = 0;
  jint meta_state = 0;
  jint button_state = 0;
  jint classification = 0;
  float x_offset = 0.0f;
  float y_offset = 0.0f;
  float x_precision = 1.0f;
  float y_precision = 1.0f;
  float raw_x_cursor = std::numeric_limits<float>::quiet_NaN();
  float raw_y_cursor = std::numeric_limits<float>::quiet_NaN();
  int64_t down_time_nanos = 0;
  int64_t event_time_nanos = 0;
  std::vector<DarwinMotionPointer> pointers;
};

struct PointerFields {
  jfieldID id = nullptr;
  jfieldID tool_type = nullptr;
};

struct CoordFields {
  jfieldID x = nullptr;
  jfieldID y = nullptr;
  jfieldID pressure = nullptr;
  jfieldID size = nullptr;
  jfieldID touch_major = nullptr;
  jfieldID touch_minor = nullptr;
  jfieldID tool_major = nullptr;
  jfieldID tool_minor = nullptr;
  jfieldID orientation = nullptr;
  jfieldID relative_x = nullptr;
  jfieldID relative_y = nullptr;
  jfieldID is_resampled = nullptr;
};

PointerFields g_pointer_fields;
CoordFields g_coord_fields;
jint g_next_id = 1;

DarwinMotionEvent* Event(jlong native_ptr) {
  return reinterpret_cast<DarwinMotionEvent*>(static_cast<uintptr_t>(native_ptr));
}

bool ValidEvent(const DarwinMotionEvent* event) {
  return event != nullptr && event->pointers.size() <= kMaxPointers;
}

bool ValidPointer(const DarwinMotionEvent* event, jint index) {
  return ValidEvent(event) && index >= 0 &&
         static_cast<size_t>(index) < event->pointers.size();
}

bool ValidPointerCount(JNIEnv* env, jint count) {
  if (count >= 1 && count <= kMaxPointers) return true;
  if (env != nullptr) env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),
                                     "invalid MotionEvent pointer count");
  return false;
}

DarwinMotionPointer ReadPointer(JNIEnv* env, jobject properties, jobject coords) {
  DarwinMotionPointer pointer;
  pointer.id = env->GetIntField(properties, g_pointer_fields.id);
  pointer.tool_type = env->GetIntField(properties, g_pointer_fields.tool_type);
  pointer.x = env->GetFloatField(coords, g_coord_fields.x);
  pointer.y = env->GetFloatField(coords, g_coord_fields.y);
  pointer.pressure = env->GetFloatField(coords, g_coord_fields.pressure);
  pointer.size = env->GetFloatField(coords, g_coord_fields.size);
  pointer.touch_major = env->GetFloatField(coords, g_coord_fields.touch_major);
  pointer.touch_minor = env->GetFloatField(coords, g_coord_fields.touch_minor);
  pointer.tool_major = env->GetFloatField(coords, g_coord_fields.tool_major);
  pointer.tool_minor = env->GetFloatField(coords, g_coord_fields.tool_minor);
  pointer.orientation = env->GetFloatField(coords, g_coord_fields.orientation);
  pointer.relative_x = env->GetFloatField(coords, g_coord_fields.relative_x);
  pointer.relative_y = env->GetFloatField(coords, g_coord_fields.relative_y);
  return pointer;
}

void WritePointer(JNIEnv* env, const DarwinMotionPointer& pointer, jobject properties,
                  jobject coords) {
  if (properties != nullptr) {
    env->SetIntField(properties, g_pointer_fields.id, pointer.id);
    env->SetIntField(properties, g_pointer_fields.tool_type, pointer.tool_type);
  }
  env->SetFloatField(coords, g_coord_fields.x, pointer.x);
  env->SetFloatField(coords, g_coord_fields.y, pointer.y);
  env->SetFloatField(coords, g_coord_fields.pressure, pointer.pressure);
  env->SetFloatField(coords, g_coord_fields.size, pointer.size);
  env->SetFloatField(coords, g_coord_fields.touch_major, pointer.touch_major);
  env->SetFloatField(coords, g_coord_fields.touch_minor, pointer.touch_minor);
  env->SetFloatField(coords, g_coord_fields.tool_major, pointer.tool_major);
  env->SetFloatField(coords, g_coord_fields.tool_minor, pointer.tool_minor);
  env->SetFloatField(coords, g_coord_fields.orientation, pointer.orientation);
  env->SetFloatField(coords, g_coord_fields.relative_x, pointer.relative_x);
  env->SetFloatField(coords, g_coord_fields.relative_y, pointer.relative_y);
  env->SetBooleanField(coords, g_coord_fields.is_resampled, JNI_FALSE);
}

float AxisValue(const DarwinMotionPointer& pointer, jint axis) {
  switch (axis) {
    case kAxisX: return pointer.x;
    case kAxisY: return pointer.y;
    case kAxisPressure: return pointer.pressure;
    case kAxisSize: return pointer.size;
    case kAxisTouchMajor: return pointer.touch_major;
    case kAxisTouchMinor: return pointer.touch_minor;
    case kAxisToolMajor: return pointer.tool_major;
    case kAxisToolMinor: return pointer.tool_minor;
    case kAxisOrientation: return pointer.orientation;
    case kAxisRelativeX: return pointer.relative_x;
    case kAxisRelativeY: return pointer.relative_y;
    default: return 0.0f;
  }
}

void ThrowIllegalArgument(JNIEnv* env, const char* message) {
  jclass klass = env->FindClass("java/lang/IllegalArgumentException");
  if (klass != nullptr) env->ThrowNew(klass, message);
  env->DeleteLocalRef(klass);
}

jlong NativeInitialize(JNIEnv* env, jclass, jlong native_ptr, jint device_id,
                       jint source, jint display_id, jint action, jint flags,
                       jint edge_flags, jint meta_state, jint button_state,
                       jint classification, jfloat x_offset, jfloat y_offset,
                       jfloat x_precision, jfloat y_precision,
                       jlong down_time_nanos, jlong event_time_nanos,
                       jint pointer_count, jobjectArray properties_array,
                       jobjectArray coords_array) {
  if (!ValidPointerCount(env, pointer_count) || properties_array == nullptr ||
      coords_array == nullptr || env->GetArrayLength(properties_array) < pointer_count ||
      env->GetArrayLength(coords_array) < pointer_count) {
    if (!env->ExceptionCheck()) ThrowIllegalArgument(env, "invalid MotionEvent arrays");
    return 0;
  }
  auto event = std::unique_ptr<DarwinMotionEvent>(Event(native_ptr));
  if (event == nullptr) event = std::make_unique<DarwinMotionEvent>();
  event->id = event->id == 0 ? g_next_id++ : event->id;
  event->device_id = device_id;
  event->source = source;
  event->display_id = display_id;
  event->action = action;
  event->flags = flags;
  event->edge_flags = edge_flags;
  event->meta_state = meta_state;
  event->button_state = button_state;
  event->classification = classification;
  event->x_offset = x_offset;
  event->y_offset = y_offset;
  event->x_precision = x_precision;
  event->y_precision = y_precision;
  event->down_time_nanos = down_time_nanos;
  event->event_time_nanos = event_time_nanos;
  event->pointers.clear();
  event->pointers.reserve(pointer_count);
  for (jint index = 0; index < pointer_count; ++index) {
    jobject properties = env->GetObjectArrayElement(properties_array, index);
    jobject coords = env->GetObjectArrayElement(coords_array, index);
    if (properties == nullptr || coords == nullptr || env->ExceptionCheck()) {
      env->DeleteLocalRef(properties);
      env->DeleteLocalRef(coords);
      return 0;
    }
    event->pointers.push_back(ReadPointer(env, properties, coords));
    env->DeleteLocalRef(properties);
    env->DeleteLocalRef(coords);
  }
  return static_cast<jlong>(reinterpret_cast<uintptr_t>(event.release()));
}

void NativeDispose(JNIEnv*, jclass, jlong native_ptr) { delete Event(native_ptr); }

void NativeAddBatch(JNIEnv* env, jclass, jlong native_ptr, jlong event_time_nanos,
                    jobjectArray coords_array, jint meta_state) {
  DarwinMotionEvent* event = Event(native_ptr);
  if (!ValidEvent(event) || coords_array == nullptr ||
      env->GetArrayLength(coords_array) < static_cast<jsize>(event->pointers.size())) {
    ThrowIllegalArgument(env, "invalid MotionEvent batch");
    return;
  }
  for (size_t index = 0; index < event->pointers.size(); ++index) {
    jobject coords = env->GetObjectArrayElement(coords_array, static_cast<jsize>(index));
    if (coords == nullptr || env->ExceptionCheck()) {
      env->DeleteLocalRef(coords);
      return;
    }
    DarwinMotionPointer pointer = event->pointers[index];
    pointer.x = env->GetFloatField(coords, g_coord_fields.x);
    pointer.y = env->GetFloatField(coords, g_coord_fields.y);
    pointer.pressure = env->GetFloatField(coords, g_coord_fields.pressure);
    event->pointers[index] = pointer;
    env->DeleteLocalRef(coords);
  }
  event->event_time_nanos = event_time_nanos;
  event->meta_state |= meta_state;
}

void NativeGetPointerCoords(JNIEnv* env, jclass, jlong native_ptr, jint pointer_index,
                            jint, jobject out_coords) {
  DarwinMotionEvent* event = Event(native_ptr);
  if (!ValidPointer(event, pointer_index) || out_coords == nullptr) {
    ThrowIllegalArgument(env, "invalid MotionEvent pointer index");
    return;
  }
  DarwinMotionPointer pointer = event->pointers[static_cast<size_t>(pointer_index)];
  WritePointer(env, pointer, nullptr, out_coords);
}

void NativeGetPointerProperties(JNIEnv* env, jclass, jlong native_ptr, jint pointer_index,
                                jobject out_properties) {
  DarwinMotionEvent* event = Event(native_ptr);
  if (!ValidPointer(event, pointer_index) || out_properties == nullptr) {
    ThrowIllegalArgument(env, "invalid MotionEvent pointer index");
    return;
  }
  const auto& pointer = event->pointers[static_cast<size_t>(pointer_index)];
  env->SetIntField(out_properties, g_pointer_fields.id, pointer.id);
  env->SetIntField(out_properties, g_pointer_fields.tool_type, pointer.tool_type);
}

jint NativeGetPointerId(JNIEnv*, jclass, jlong native_ptr, jint index) {
  auto* event = Event(native_ptr);
  return ValidPointer(event, index) ? event->pointers[static_cast<size_t>(index)].id : -1;
}
jint NativeGetToolType(JNIEnv*, jclass, jlong native_ptr, jint index) {
  auto* event = Event(native_ptr);
  return ValidPointer(event, index) ? event->pointers[static_cast<size_t>(index)].tool_type : 0;
}
jlong NativeGetEventTimeNanos(JNIEnv*, jclass, jlong native_ptr, jint) {
  auto* event = Event(native_ptr);
  return ValidEvent(event) ? event->event_time_nanos : 0;
}
jfloat NativeGetAxisValue(JNIEnv*, jclass, jlong native_ptr, jint axis, jint index, jint) {
  auto* event = Event(native_ptr);
  return ValidPointer(event, index) ? AxisValue(event->pointers[static_cast<size_t>(index)], axis)
                                    : 0.0f;
}
jfloat NativeGetRawAxisValue(JNIEnv* env, jclass clazz, jlong ptr, jint axis, jint index, jint history) {
  return NativeGetAxisValue(env, clazz, ptr, axis, index, history);
}

jint NativeGetId(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->id : 0; }
jint NativeGetDeviceId(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->device_id : -1; }
jint NativeGetSource(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->source : 0; }
void NativeSetSource(jlong ptr, jint value) { if (auto* e = Event(ptr)) e->source = value; }
jint NativeGetDisplayId(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->display_id : 0; }
void NativeSetDisplayId(jlong ptr, jint value) { if (auto* e = Event(ptr)) e->display_id = value; }
jint NativeGetAction(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->action : 0; }
void NativeSetAction(jlong ptr, jint value) { if (auto* e = Event(ptr)) e->action = value; }
jboolean NativeIsTouchEvent(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? JNI_TRUE : JNI_FALSE; }
jint NativeGetFlags(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->flags : 0; }
void NativeSetFlags(jlong ptr, jint value) { if (auto* e = Event(ptr)) e->flags = value; }
jint NativeGetEdgeFlags(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->edge_flags : 0; }
void NativeSetEdgeFlags(jlong ptr, jint value) { if (auto* e = Event(ptr)) e->edge_flags = value; }
jint NativeGetMetaState(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->meta_state : 0; }
jint NativeGetButtonState(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->button_state : 0; }
void NativeSetButtonState(jlong ptr, jint value) { if (auto* e = Event(ptr)) e->button_state = value; }
jint NativeGetClassification(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->classification : 0; }
jint NativeGetActionButton(jlong ptr) { return 0; }
void NativeSetActionButton(jlong, jint) {}
void NativeOffsetLocation(jlong ptr, jfloat dx, jfloat dy) {
  if (auto* e = Event(ptr)) for (auto& p : e->pointers) { p.x += dx; p.y += dy; }
}
jfloat NativeGetRawXOffset(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->x_offset : 0; }
jfloat NativeGetRawYOffset(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->y_offset : 0; }
jfloat NativeGetXPrecision(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->x_precision : 1; }
jfloat NativeGetYPrecision(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->y_precision : 1; }
jfloat NativeGetXCursorPosition(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->raw_x_cursor : NAN; }
jfloat NativeGetYCursorPosition(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->raw_y_cursor : NAN; }
void NativeSetCursorPosition(jlong ptr, jfloat x, jfloat y) { if (auto* e = Event(ptr)) { e->raw_x_cursor = x; e->raw_y_cursor = y; } }
jlong NativeGetDownTimeNanos(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? e->down_time_nanos : 0; }
void NativeSetDownTimeNanos(jlong ptr, jlong value) { if (auto* e = Event(ptr)) e->down_time_nanos = value; }
jint NativeGetPointerCount(jlong ptr) { auto* e = Event(ptr); return ValidEvent(e) ? static_cast<jint>(e->pointers.size()) : 0; }
jint NativeFindPointerIndex(jlong ptr, jint id) {
  auto* e = Event(ptr); if (!ValidEvent(e)) return -1;
  for (size_t i = 0; i < e->pointers.size(); ++i) if (e->pointers[i].id == id) return static_cast<jint>(i);
  return -1;
}
jint NativeGetHistorySize(jlong) { return 0; }
void NativeScale(jlong ptr, jfloat scale) { if (auto* e = Event(ptr)) for (auto& p : e->pointers) { p.x *= scale; p.y *= scale; } }
jint NativeGetSurfaceRotation(jlong) { return 0; }

jlong NativeCopy(jlong dest_ptr, jlong source_ptr, jboolean) {
  auto* source = Event(source_ptr);
  if (source == nullptr) return 0;
  auto event = std::unique_ptr<DarwinMotionEvent>(Event(dest_ptr));
  if (event == nullptr) event = std::make_unique<DarwinMotionEvent>();
  *event = *source;
  return static_cast<jlong>(reinterpret_cast<uintptr_t>(event.release()));
}
jlong NativeSplit(jlong dest_ptr, jlong source_ptr, jint) { return NativeCopy(dest_ptr, source_ptr, JNI_FALSE); }

const JNINativeMethod kMethods[] = {
    {"nativeInitialize", "(JIIIIIIIIIFFFFJJI[Landroid/view/MotionEvent$PointerProperties;[Landroid/view/MotionEvent$PointerCoords;)J", reinterpret_cast<void*>(&NativeInitialize)},
    {"nativeDispose", "(J)V", reinterpret_cast<void*>(&NativeDispose)},
    {"nativeAddBatch", "(JJ[Landroid/view/MotionEvent$PointerCoords;I)V", reinterpret_cast<void*>(&NativeAddBatch)},
    {"nativeGetPointerCoords", "(JIILandroid/view/MotionEvent$PointerCoords;)V", reinterpret_cast<void*>(&NativeGetPointerCoords)},
    {"nativeGetPointerProperties", "(JILandroid/view/MotionEvent$PointerProperties;)V", reinterpret_cast<void*>(&NativeGetPointerProperties)},
    {"nativeGetPointerId", "(JI)I", reinterpret_cast<void*>(&NativeGetPointerId)},
    {"nativeGetToolType", "(JI)I", reinterpret_cast<void*>(&NativeGetToolType)},
    {"nativeGetEventTimeNanos", "(JI)J", reinterpret_cast<void*>(&NativeGetEventTimeNanos)},
    {"nativeGetRawAxisValue", "(JIII)F", reinterpret_cast<void*>(&NativeGetRawAxisValue)},
    {"nativeGetAxisValue", "(JIII)F", reinterpret_cast<void*>(&NativeGetAxisValue)},
    {"nativeGetId", "(J)I", reinterpret_cast<void*>(&NativeGetId)},
    {"nativeGetDeviceId", "(J)I", reinterpret_cast<void*>(&NativeGetDeviceId)},
    {"nativeGetSource", "(J)I", reinterpret_cast<void*>(&NativeGetSource)},
    {"nativeSetSource", "(JI)V", reinterpret_cast<void*>(&NativeSetSource)},
    {"nativeGetDisplayId", "(J)I", reinterpret_cast<void*>(&NativeGetDisplayId)},
    {"nativeSetDisplayId", "(JI)V", reinterpret_cast<void*>(&NativeSetDisplayId)},
    {"nativeGetAction", "(J)I", reinterpret_cast<void*>(&NativeGetAction)},
    {"nativeSetAction", "(JI)V", reinterpret_cast<void*>(&NativeSetAction)},
    {"nativeIsTouchEvent", "(J)Z", reinterpret_cast<void*>(&NativeIsTouchEvent)},
    {"nativeGetFlags", "(J)I", reinterpret_cast<void*>(&NativeGetFlags)},
    {"nativeSetFlags", "(JI)V", reinterpret_cast<void*>(&NativeSetFlags)},
    {"nativeGetEdgeFlags", "(J)I", reinterpret_cast<void*>(&NativeGetEdgeFlags)},
    {"nativeSetEdgeFlags", "(JI)V", reinterpret_cast<void*>(&NativeSetEdgeFlags)},
    {"nativeGetMetaState", "(J)I", reinterpret_cast<void*>(&NativeGetMetaState)},
    {"nativeGetButtonState", "(J)I", reinterpret_cast<void*>(&NativeGetButtonState)},
    {"nativeSetButtonState", "(JI)V", reinterpret_cast<void*>(&NativeSetButtonState)},
    {"nativeGetClassification", "(J)I", reinterpret_cast<void*>(&NativeGetClassification)},
    {"nativeGetActionButton", "(J)I", reinterpret_cast<void*>(&NativeGetActionButton)},
    {"nativeSetActionButton", "(JI)V", reinterpret_cast<void*>(&NativeSetActionButton)},
    {"nativeOffsetLocation", "(JFF)V", reinterpret_cast<void*>(&NativeOffsetLocation)},
    {"nativeGetRawXOffset", "(J)F", reinterpret_cast<void*>(&NativeGetRawXOffset)},
    {"nativeGetRawYOffset", "(J)F", reinterpret_cast<void*>(&NativeGetRawYOffset)},
    {"nativeGetXPrecision", "(J)F", reinterpret_cast<void*>(&NativeGetXPrecision)},
    {"nativeGetYPrecision", "(J)F", reinterpret_cast<void*>(&NativeGetYPrecision)},
    {"nativeGetXCursorPosition", "(J)F", reinterpret_cast<void*>(&NativeGetXCursorPosition)},
    {"nativeGetYCursorPosition", "(J)F", reinterpret_cast<void*>(&NativeGetYCursorPosition)},
    {"nativeSetCursorPosition", "(JFF)V", reinterpret_cast<void*>(&NativeSetCursorPosition)},
    {"nativeGetDownTimeNanos", "(J)J", reinterpret_cast<void*>(&NativeGetDownTimeNanos)},
    {"nativeSetDownTimeNanos", "(JJ)V", reinterpret_cast<void*>(&NativeSetDownTimeNanos)},
    {"nativeGetPointerCount", "(J)I", reinterpret_cast<void*>(&NativeGetPointerCount)},
    {"nativeFindPointerIndex", "(JI)I", reinterpret_cast<void*>(&NativeFindPointerIndex)},
    {"nativeGetHistorySize", "(J)I", reinterpret_cast<void*>(&NativeGetHistorySize)},
    {"nativeScale", "(JF)V", reinterpret_cast<void*>(&NativeScale)},
    {"nativeGetSurfaceRotation", "(J)I", reinterpret_cast<void*>(&NativeGetSurfaceRotation)},
    {"nativeCopy", "(JJZ)J", reinterpret_cast<void*>(&NativeCopy)},
    {"nativeSplit", "(JJI)J", reinterpret_cast<void*>(&NativeSplit)},
};

}  // namespace

namespace darwin_art {

bool RegisterMotionEventNatives(JNIEnv* env) {
  if (env == nullptr) return false;
  jclass motion_event = env->FindClass("android/view/MotionEvent");
  jclass pointer_properties = env->FindClass("android/view/MotionEvent$PointerProperties");
  jclass pointer_coords = env->FindClass("android/view/MotionEvent$PointerCoords");
  if (motion_event == nullptr || pointer_properties == nullptr || pointer_coords == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(motion_event);
    env->DeleteLocalRef(pointer_properties);
    env->DeleteLocalRef(pointer_coords);
    return false;
  }
  g_pointer_fields.id = env->GetFieldID(pointer_properties, "id", "I");
  g_pointer_fields.tool_type = env->GetFieldID(pointer_properties, "toolType", "I");
  g_coord_fields.x = env->GetFieldID(pointer_coords, "x", "F");
  g_coord_fields.y = env->GetFieldID(pointer_coords, "y", "F");
  g_coord_fields.pressure = env->GetFieldID(pointer_coords, "pressure", "F");
  g_coord_fields.size = env->GetFieldID(pointer_coords, "size", "F");
  g_coord_fields.touch_major = env->GetFieldID(pointer_coords, "touchMajor", "F");
  g_coord_fields.touch_minor = env->GetFieldID(pointer_coords, "touchMinor", "F");
  g_coord_fields.tool_major = env->GetFieldID(pointer_coords, "toolMajor", "F");
  g_coord_fields.tool_minor = env->GetFieldID(pointer_coords, "toolMinor", "F");
  g_coord_fields.orientation = env->GetFieldID(pointer_coords, "orientation", "F");
  g_coord_fields.relative_x = env->GetFieldID(pointer_coords, "relativeX", "F");
  g_coord_fields.relative_y = env->GetFieldID(pointer_coords, "relativeY", "F");
  g_coord_fields.is_resampled = env->GetFieldID(pointer_coords, "isResampled", "Z");
  const bool fields_ok =
      !env->ExceptionCheck() && g_pointer_fields.id != nullptr &&
      g_pointer_fields.tool_type != nullptr && g_coord_fields.x != nullptr &&
      g_coord_fields.y != nullptr && g_coord_fields.pressure != nullptr &&
      g_coord_fields.size != nullptr && g_coord_fields.touch_major != nullptr &&
      g_coord_fields.touch_minor != nullptr && g_coord_fields.tool_major != nullptr &&
      g_coord_fields.tool_minor != nullptr && g_coord_fields.orientation != nullptr &&
      g_coord_fields.relative_x != nullptr && g_coord_fields.relative_y != nullptr &&
      g_coord_fields.is_resampled != nullptr;
  const jint status = fields_ok ? env->RegisterNatives(motion_event, kMethods,
                                                       static_cast<jint>(std::size(kMethods)))
                                : JNI_ERR;
  env->DeleteLocalRef(motion_event);
  env->DeleteLocalRef(pointer_properties);
  env->DeleteLocalRef(pointer_coords);
  return fields_ok && status == JNI_OK && !env->ExceptionCheck();
}

}  // namespace darwin_art
