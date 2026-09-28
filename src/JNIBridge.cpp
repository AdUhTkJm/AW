// JNI front door for the crafting graph.
//
// The Java side calls static native methods:
//
//   package io.aduhtkjm.appliedwheelchair.natives;
//   public final class CraftingGraphKernel {
//     public static native void registerCraftingGraph(byte[] blob);
//     public static native int  status();
//     public static native void setPlannerOptions(String json);
//     public static native void setSolverOptions(String json);
//     public static native byte[] plan(byte[] request);
//   }
//
// A second class, SearchKernel, carries the search index entry point. Its
// natives are bound by src/search/SearchJni.cpp and registered from here, so
// the library still has exactly one JNI_OnLoad:
//
//   public final class SearchKernel {
//     public static native void registerSearch(ByteBuffer[] chunks, int[] offsets,
//                                              int fieldsPerHandle);
//   }
//
// The methods are bound in JNI_OnLoad, so no symbol name depends on the
// package: only the two AW_JNI_*_CLASS_NAME macros do, and they are
// compile-time knobs.
//
// Concurrency contract: the mod owns exactly one worker thread and never calls
// two of these at once. Nothing here takes a lock; the status flag is the
// cooperation point, and `status()` is the only method that is safe to call
// from another thread while one of the others is running. It reads a single
// atomic and touches no planner state.
//
// The class named by AW_JNI_CLASS_NAME must NOT have a static initializer that
// calls System.load / System.loadLibrary: JNI_OnLoad runs inside that load, and
// FindClass below would re-enter it. The mod keeps the loading in a separate
// NativeLoader class for exactly this reason.

#include <jni.h>

#include <cstddef>
#include <span>
#include <string>

#include "aw/CraftingGraph.h"
#include "aw/OptionsJson.h"
#include "aw/Protocol.h"
#include "search/SearchJni.h"

#ifndef AW_JNI_CLASS_NAME
#define AW_JNI_CLASS_NAME "io/aduhtkjm/appliedwheelchair/natives/CraftingGraphKernel"
#endif

#ifndef AW_JNI_SEARCH_CLASS_NAME
#define AW_JNI_SEARCH_CLASS_NAME "io/aduhtkjm/appliedwheelchair/natives/SearchKernel"
#endif

namespace {

void throwJava(JNIEnv *env, const char *className, const std::string &message) {
  jclass type = env->FindClass(className);
  // FindClass would have thrown a NoClassDefFoundError anyway.
  if (type == nullptr) {
    return;
  }
  env->ThrowNew(type, message.c_str());
  env->DeleteLocalRef(type);
}

// Copies a Java string into a std::string. Returns false when the argument was
// null (a Java-side bug) or when the VM could not hand out the characters, in
// which case an exception is already pending.
bool copyString(JNIEnv *env, jstring text, std::string &out) {
  if (text == nullptr) {
    throwJava(env, "java/lang/NullPointerException", "options must not be null");
    return false;
  }
  const char *chars = env->GetStringUTFChars(text, nullptr);
  if (chars == nullptr)
    return false;
  out.assign(chars);
  env->ReleaseStringUTFChars(text, chars);
  return true;
}

// Parses the blob and installs it as the process-wide crafting graph, replacing
// any previous one. Throws IllegalArgumentException on a malformed blob.
void JNICALL nativeRegisterCraftingGraph(JNIEnv *env, jclass, jbyteArray blob) {
  if (blob == nullptr) {
    throwJava(env, "java/lang/NullPointerException", "recipe blob must not be null");
    return;
  }

  const jsize length = env->GetArrayLength(blob);
  jbyte *raw = env->GetByteArrayElements(blob, nullptr);
  if (raw == nullptr)
    return;

  {
    // Registration is the long pass: decoding, the parse-time cleanup, tag
    // inlining and every dominance/pack certificate. A UI thread polling
    // status() sees PREPROCESSING for its whole duration.
    const aw::KernelStatusGuard guard(aw::KernelStatus::PREPROCESSING);
    aw::registerCraftingGraph(
        std::span<const std::byte>((const std::byte *) raw, (size_t) length));
  }

  const char *message = aw::getCraftingError();
  if (message != nullptr)
    throwJava(env, "java/lang/IllegalArgumentException", message);

  env->ReleaseByteArrayElements(blob, raw, JNI_ABORT);
}

jint JNICALL nativeStatus(JNIEnv *, jclass) {
  return (jint) aw::getKernelStatus();
}

void JNICALL nativeSetPlannerOptions(JNIEnv *env, jclass, jstring json) {
  std::string text;
  if (!copyString(env, json, text))
    return;

  std::string error;
  if (!aw::applyPlannerOptionsJson(text, error))
    throwJava(env, "java/lang/IllegalArgumentException", error);
}

void JNICALL nativeSetSolverOptions(JNIEnv *env, jclass, jstring json) {
  std::string text;
  if (!copyString(env, json, text))
    return;

  std::string error;
  if (!aw::applySolverOptionsJson(text, error))
    throwJava(env, "java/lang/IllegalArgumentException", error);
}

// Decodes the request, runs the query and encodes the answer. One call, so the
// whole query is atomic with respect to the status flag and to the graph.
jbyteArray JNICALL nativePlan(JNIEnv *env, jclass, jbyteArray request) {
  if (request == nullptr) {
    throwJava(env, "java/lang/NullPointerException", "plan request must not be null");
    return nullptr;
  }

  const jsize length = env->GetArrayLength(request);
  jbyte *raw = env->GetByteArrayElements(request, nullptr);
  if (raw == nullptr)
    return nullptr;

  std::string error;
  aw::vector<std::byte> response = aw::planBlob(
      std::span<const std::byte>((const std::byte *) raw, (size_t) length), error);
  env->ReleaseByteArrayElements(request, raw, JNI_ABORT);

  if (!error.empty()) {
    throwJava(env, "java/lang/IllegalArgumentException", error);
    return nullptr;
  }

  const jsize responseLength = (jsize) response.size();
  jbyteArray out = env->NewByteArray(responseLength);
  if (out == nullptr)
    return nullptr;
  if (responseLength > 0) {
    env->SetByteArrayRegion(out, 0, responseLength,
                            (const jbyte *) response.data());
    if (env->ExceptionCheck()) {
      env->DeleteLocalRef(out);
      return nullptr;
    }
  }
  return out;
}

const JNINativeMethod jniMethods[] = {
  {(char*) "registerCraftingGraph", (char*) "([B)V", (void*) &nativeRegisterCraftingGraph},
  {(char*) "status", (char*) "()I", (void*) &nativeStatus},
  {(char*) "setPlannerOptions", (char*) "(Ljava/lang/String;)V", (void*) &nativeSetPlannerOptions},
  {(char*) "setSolverOptions", (char*) "(Ljava/lang/String;)V", (void*) &nativeSetSolverOptions},
  {(char*) "plan", (char*) "([B)[B", (void*) &nativePlan},
};

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void*) {
  JNIEnv *env = nullptr;
  if (vm->GetEnv((void **) &env, JNI_VERSION_1_8) != JNI_OK || env == nullptr) {
    return JNI_ERR;
  }

  jclass type = env->FindClass(AW_JNI_CLASS_NAME);
  if (!type) {
    return JNI_ERR;
  }
  const jint count = (jint) (sizeof(jniMethods) / sizeof(JNINativeMethod));
  const jint result = env->RegisterNatives(type, jniMethods, count);
  env->DeleteLocalRef(type);
  if (result != JNI_OK) {
    return JNI_ERR;
  }

  jclass searchType = env->FindClass(AW_JNI_SEARCH_CLASS_NAME);
  if (!searchType) {
    return JNI_ERR;
  }
  const bool searchOk = aw::search::registerNativeMethods(env, searchType);
  env->DeleteLocalRef(searchType);
  return searchOk ? JNI_VERSION_1_8 : JNI_ERR;
}
