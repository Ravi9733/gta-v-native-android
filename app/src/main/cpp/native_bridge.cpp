#include <jni.h>

extern "C" JNIEXPORT jstring JNICALL
Java_org_macnativegames_gtavandroid_MainActivity_nativePortStatus(
    JNIEnv* env, jobject /* thiz */) {
    constexpr char kStatus[] =
        "Android ARM64 build shell is active.\n\n"
        "Game runtime is not connected yet. The upstream renderer is "
        "Metal-only; an Android Vulkan NVN backend and Android host "
        "services must be implemented before the game can boot.\n\n"
        "This development shell is intentionally not presented as a "
        "working GTA V port.";
    return env->NewStringUTF(kStatus);
}
