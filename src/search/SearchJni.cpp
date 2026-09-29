// JNI front door for the search index. It mirrors src/JNIBridge.cpp: the class
// named by AW_JNI_SEARCH_CLASS_NAME declares
//
//   public static native void registerSearch(ByteBuffer[] chunks, int[] offsets,
//                                            int fieldsPerHandle);
//   public static native int[] search(String query, int limit);
//
// and nothing here depends on the package.
//
// The class must not have a static initializer that loads the library, for the
// same reason CraftingGraphKernel must not: JNI_OnLoad runs inside that load.

#include "SearchJni.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "aw/search/TextIndex.h"

namespace {

void throwJava(JNIEnv *env, const char *className, const std::string &message) {
  jclass type = env->FindClass(className);
  // FindClass would have thrown a NoClassDefFoundError anyway.
  if (type == nullptr)
    return;
  env->ThrowNew(type, message.c_str());
  env->DeleteLocalRef(type);
}

void JNICALL nativeRegisterSearch(JNIEnv *env, jclass, jobjectArray chunks,
                                  jintArray offsets, jint fieldsPerHandle) {
  if (chunks == nullptr || offsets == nullptr) {
    throwJava(env, "java/lang/NullPointerException", "the search corpus must not be null");
    return;
  }
  if (fieldsPerHandle <= 0) {
    throwJava(env, "java/lang/IllegalArgumentException", "the field count must be positive");
    return;
  }

  // Collecting the addresses first keeps the Java calls and the native work in
  // separate phases, so no local reference is held across the copy.
  const jsize chunkCount = env->GetArrayLength(chunks);
  std::vector<const std::byte *> pointers((size_t) chunkCount);
  std::vector<std::size_t> sizes((size_t) chunkCount);
  for (jsize i = 0; i < chunkCount; i++) {
    jobject chunk = env->GetObjectArrayElement(chunks, i);
    if (env->ExceptionCheck()) {
      if (chunk != nullptr)
        env->DeleteLocalRef(chunk);
      return;
    }
    if (chunk == nullptr) {
      throwJava(env, "java/lang/IllegalArgumentException", "a search chunk is null");
      return;
    }
    void *address = env->GetDirectBufferAddress(chunk);
    const jlong capacity = env->GetDirectBufferCapacity(chunk);
    env->DeleteLocalRef(chunk);
    if (address == nullptr || capacity < 0) {
      throwJava(env, "java/lang/IllegalArgumentException",
                "every search chunk must be a direct ByteBuffer");
      return;
    }
    pointers[(size_t) i] = (const std::byte *) address;
    sizes[(size_t) i] = (std::size_t) capacity;
  }

  jint *raw = env->GetIntArrayElements(offsets, nullptr);
  // A null result means an exception (out of memory) is already pending.
  if (raw == nullptr)
    return;

  const jsize offsetCount = env->GetArrayLength(offsets);
  // jint and uint32_t share a representation; the spans below only read.
  const std::span<const uint32_t> offsetSpan((const uint32_t *) raw, (size_t) offsetCount);

  std::string error;
  const bool ok = aw::search::registerTextIndex(
      std::span<const std::byte *>(pointers.data(), pointers.size()),
      std::span<const std::size_t>(sizes.data(), sizes.size()),
      offsetSpan, (uint32_t) fieldsPerHandle, error);

  env->ReleaseIntArrayElements(offsets, raw, JNI_ABORT);

  if (!ok)
    throwJava(env, "java/lang/IllegalArgumentException", error);
}

jintArray JNICALL nativeSearch(JNIEnv *env, jclass, jstring query, jint limit) {
  if (query == nullptr) {
    throwJava(env, "java/lang/NullPointerException", "the search query must not be null");
    return nullptr;
  }
  if (limit < 0) {
    throwJava(env, "java/lang/IllegalArgumentException", "the search limit must not be negative");
    return nullptr;
  }

  // GetStringUTFChars yields modified UTF-8. The two spellings only differ for
  // supplementary characters, and the corpus holds BMP Chinese and ASCII, so
  // these are the same bytes the mod sent through StringPool.
  const char *utf = env->GetStringUTFChars(query, nullptr);
  // A null result means an exception (out of memory) is already pending.
  if (utf == nullptr)
    return nullptr;
  const std::string text(utf);
  env->ReleaseStringUTFChars(query, utf);

  const std::vector<uint32_t> hits = aw::search::search(text, (uint32_t) limit);
  jintArray result = env->NewIntArray((jsize) hits.size());
  if (result == nullptr)
    return nullptr;
  if (!hits.empty())
    env->SetIntArrayRegion(result, 0, (jsize) hits.size(), (const jint *) hits.data());
  return result;
}

const JNINativeMethod searchMethods[] = {
    {(char *) "registerSearch", (char *) "([Ljava/nio/ByteBuffer;[II)V",
     (void *) &nativeRegisterSearch},
    {(char *) "search", (char *) "(Ljava/lang/String;I)[I", (void *) &nativeSearch},
};

}  // namespace

namespace aw::search {

bool registerNativeMethods(JNIEnv *env, jclass type) noexcept {
  const jint count = (jint) (sizeof(searchMethods) / sizeof(JNINativeMethod));
  return env->RegisterNatives(type, searchMethods, count) == JNI_OK;
}

}  // namespace aw::search
