#include "native_audio_hook.h"
#include "got_hook.h"
#include "integrity_check.h"
#include "pcm_bridge.h"
#include <android/log.h>
#include <jni.h>
#include <thread>
#include <unistd.h>

#define LOG_TAG "CS-NativeHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static JavaVM* g_jvm = nullptr;

JavaVM* get_java_vm() {
    return g_jvm;
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_jvm = vm;
    LOGI("JNI_OnLoad completed");
    return JNI_VERSION_1_6;
}

// A bounded rescan window covers caller modules the install pass could not
// see: engines load their audio libraries after Application.onCreate, and
// those bind their GOT entries at that later moment. The first phase polls
// fast enough to catch them during engine init, the second keeps a slower
// eye for a few minutes, then the window closes for good - the scan is
// read-mostly work that should not run for the life of the process. It ends
// early once every AAudio symbol has at least one entry armed; the OpenSL ES
// side needs no rescans because its vtables arm event-driven.
static void start_rescan_worker() {
    std::thread([] {
        static const char* kAaudio[] = {
            "AAudioStream_read",
            "AAudioStreamBuilder_setDataCallback",
            "AAudioStreamBuilder_openStream",
            "AAudioStream_close",
        };
        auto armed = [&] {
            for (const char* s : kAaudio) {
                if (got_hook::hits_of(s) == 0) return false;
            }
            return true;
        };
        int last = -1;
        auto round = [&](const char* phase) {
            int total = got_hook::refresh();
            if (total != last) {
                last = total;
                LOGI("rescan(%s): got entries patched=%d", phase, total);
            }
            return armed();
        };
        for (int i = 0; i < 60; i++) {  // 500ms x 60 = 30s: engine init
            if (round("fast")) {
                LOGI("all AAudio symbols armed; rescan window closed");
                return;
            }
            usleep(500 * 1000);
        }
        for (int i = 0; i < 150; i++) {  // 2s x 150 = 5min: settle
            if (round("settle")) {
                LOGI("all AAudio symbols armed; rescan window closed");
                return;
            }
            usleep(2000 * 1000);
        }
        LOGI("rescan window closed");
    }).detach();
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_io_github_zensu357_camswap_NativeAudioHook_nativeInit(JNIEnv* env, jclass clazz) {
    LOGI("nativeInit called (GOT/data-only build)");

    // Initialize the bridge while the classloader context is right (the
    // Xposed module's classloader)
    pcm_bridge_init(env, clazz);

    // Opt-in loader interposition (persist.camswap.loader_hook=1); late
    // caller coverage otherwise comes from the rescan worker below.
    got_hook::install_loader_hooks();

    install_aaudio_hooks();
    install_opensles_hooks();

    // The self-check the manager's virtual-camera page reads: after the
    // data-only rewrite both system libraries must diff clean against disk.
    integrity_report("libaaudio.so");
    integrity_report("libOpenSLES.so");

    start_rescan_worker();

    LOGI("nativeInit: all hooks installed");
    return JNI_TRUE;
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_zensu357_camswap_NativeAudioHook_nativeRelease(JNIEnv* /*env*/, jclass /*clazz*/) {
    LOGI("nativeRelease called");
    // The GOT/vtable entries persist until process exit; nothing to undo.
}

