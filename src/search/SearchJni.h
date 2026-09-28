#ifndef AW_SEARCH_JNI_H
#define AW_SEARCH_JNI_H

// Private bridge between the search index and JNI_OnLoad. It lives next to the
// search sources, not under include/, because only the aw_jni translation units
// see it and only they are compiled with the JVM headers.

#include <jni.h>

namespace aw::search {

// Binds SearchKernel's native methods onto `type`. Returns false when the JVM
// rejects the registration, which is a build-time contract error rather than a
// runtime condition, so JNI_OnLoad can fail the load cleanly.
bool registerNativeMethods(JNIEnv *env, jclass type) noexcept;

}  // namespace aw::search

#endif
