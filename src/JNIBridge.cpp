// JNI front door for the crafting graph.
//
// The Java side calls a static native method:
//
//   package io.aduhtkjm.appliedwheelchair.natives;
//   public final class CraftingGraphKernel {
//     public static native void registerCraftingGraph(byte[] blob);
//   }
//
// The class name is a compile-time knob (AW_JNI_CLASS_NAME) so it can follow
// wherever the mod puts the Java entry point. Native methods are bound in
// JNI_OnLoad so the C++ symbol names do not depend on the package at all.

#include <jni.h>

#include <cstddef>
#include <exception>
#include <span>
#include <string>

#include "aw/CraftingGraph.h"

#ifndef AW_JNI_CLASS_NAME
#define AW_JNI_CLASS_NAME "io/aduhtkjm/appliedwheelchair/natives/CraftingGraphKernel"
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

// Parses the blob and installs it as the process-wide crafting graph, replacing
// any previous one. Throws IllegalArgumentException on a malformed blob.
void JNICALL nativeRegisterCraftingGraph(JNIEnv* env, jclass, jbyteArray blob) {
  if (blob == nullptr) {
    throwJava(env, "java/lang/NullPointerException", "recipe blob must not be null");
    return;
  }

  const jsize length = env->GetArrayLength(blob);
  jbyte *raw = env->GetByteArrayElements(blob, nullptr);
  if (raw == nullptr) {
    return;
  }

  aw::registerCraftingGraph(std::span<const std::byte>((const std::byte*) raw, length));
  if (const char *msg = aw::getCraftingError())
    throwJava(env, "java/lang/IllegalArgumentException", msg);

  env->ReleaseByteArrayElements(blob, raw, JNI_ABORT);
}

const JNINativeMethod jniMethods[] = {
  {
    (char*) "registerCraftingGraph",
    (char*) "([B)V",
    (void*) &nativeRegisterCraftingGraph
  },
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
  return result == JNI_OK ? JNI_VERSION_1_8 : JNI_ERR;
}
