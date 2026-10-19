#include "hook_opensles.h"
#include "got_hook.h"
#include "pcm_bridge.h"
#include <android/log.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#define LOG_TAG "CS-OpenSLHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Hooking strategy (2026-10-19): the OpenSL ES hooks used to be Dobby inline
// hooks on the implementation functions the interface vtables point at, which
// rewrote libOpenSLES.so code in memory - the byte-compare signature. Now the
// vtable *entries themselves* are swapped (data, not code), and the
// slCreateEngine fallback is a GOT hook. Same coverage, zero bytes changed in
// any system library's executable segment.

// ============================================================
// Tracking structures
// ============================================================

struct RecorderCallbackInfo {
    slAndroidSimpleBufferQueueCallback originalCallback;
    void* originalContext;
    // Last enqueued buffer info
    void* lastBuffer;
    SLuint32 lastBufferSize;
    // Audio params
    SLuint32 sampleRate;   // milliHz (e.g. 44100000)
    SLuint32 channels;
};

static std::mutex g_sl_mutex;
static std::unordered_map<SLAndroidSimpleBufferQueueItf, RecorderCallbackInfo> g_recorder_map;

// Track which buffer queue interfaces belong to recorders (not players)
static std::unordered_map<SLAndroidSimpleBufferQueueItf, bool> g_is_recorder_bq;

// Track recorder SLObjectItf instances (from CreateAudioRecorder) and their audio params
struct RecorderObjectInfo {
    SLuint32 sampleRate;   // milliHz
    SLuint32 channels;
};
static std::unordered_map<SLObjectItf, RecorderObjectInfo> g_recorder_objects;

// ============================================================
// Original function pointers (vtable entries)
// ============================================================

typedef SLresult (*fn_RegisterCallback)(
    SLAndroidSimpleBufferQueueItf self,
    slAndroidSimpleBufferQueueCallback callback,
    void* pContext);

typedef SLresult (*fn_Enqueue)(
    SLAndroidSimpleBufferQueueItf self,
    const void* pBuffer,
    SLuint32 size);

static fn_RegisterCallback orig_RegisterCallback = nullptr;
static fn_Enqueue orig_Enqueue = nullptr;

// Track whether we've already hooked vtable functions
static bool g_vtable_hooked = false;

// Original slCreateEngine
typedef SLresult (*fn_slCreateEngine)(
    SLObjectItf* pEngine,
    SLuint32 numOptions,
    const SLEngineOption* pEngineOptions,
    SLuint32 numInterfaces,
    const SLInterfaceID* pInterfaceIds,
    const SLboolean* pInterfaceRequired);

static fn_slCreateEngine orig_slCreateEngine = nullptr;

// Original function pointers for engine/object vtable hooks
typedef SLresult (*fn_CreateAudioRecorder)(
    SLEngineItf self,
    SLObjectItf* pRecorder,
    SLDataSource* pAudioSrc,
    SLDataSink* pAudioSnk,
    SLuint32 numInterfaces,
    const SLInterfaceID* pInterfaceIds,
    const SLboolean* pInterfaceRequired);

typedef SLresult (*fn_GetInterface)(
    SLObjectItf self,
    const SLInterfaceID iid,
    void* pInterface);

typedef void (*fn_Destroy)(SLObjectItf self);

static fn_CreateAudioRecorder orig_CreateAudioRecorder = nullptr;
static fn_GetInterface orig_GetInterface = nullptr;
static fn_Destroy orig_Destroy = nullptr;
static bool g_engine_vtable_hooked = false;
static bool g_object_vtable_hooked = false;

// ============================================================
// Wrapped buffer queue callback
// ============================================================

static void wrapped_bq_callback(SLAndroidSimpleBufferQueueItf bq, void* context) {
    RecorderCallbackInfo info{};
    {
        std::lock_guard<std::mutex> lock(g_sl_mutex);
        auto it = g_recorder_map.find(bq);
        if (it != g_recorder_map.end()) {
            info = it->second;
        }
    }

    // 先尝试替换缓冲区中的真实麦克风数据
    // 如果 fill_fake_pcm 返回 < 0 说明 hook 未启用，保留原始数据
    if (info.lastBuffer && info.lastBufferSize > 0) {
        int sampleRate = info.sampleRate / 1000; // milliHz → Hz
        if (sampleRate <= 0) sampleRate = 44100;
        int channels = info.channels;
        if (channels <= 0) channels = 1;

        fill_fake_pcm(info.lastBuffer, info.lastBufferSize, sampleRate, channels);
        // return value < 0 means hook disabled — buffer is untouched (passthrough)
    }

    // Call original callback — it will see fake data and typically re-enqueue next buffer
    if (info.originalCallback) {
        info.originalCallback(bq, info.originalContext);
    }
}

// ============================================================
// Hooked: RegisterCallback
// ============================================================

static SLresult hooked_RegisterCallback(
    SLAndroidSimpleBufferQueueItf self,
    slAndroidSimpleBufferQueueCallback callback,
    void* pContext) {

    // Check if this is a recorder buffer queue
    bool isRecorder = false;
    {
        std::lock_guard<std::mutex> lock(g_sl_mutex);
        auto it = g_is_recorder_bq.find(self);
        if (it != g_is_recorder_bq.end()) {
            isRecorder = it->second;
        }
    }

    if (isRecorder) {
        LOGI("RegisterCallback on recorder buffer queue — wrapping callback");

        {
            std::lock_guard<std::mutex> lock(g_sl_mutex);
            auto& info = g_recorder_map[self];
            info.originalCallback = callback;
            info.originalContext = pContext;
        }

        return orig_RegisterCallback(self, wrapped_bq_callback, nullptr);
    }

    // Not a recorder — passthrough
    return orig_RegisterCallback(self, callback, pContext);
}

// ============================================================
// Hooked: Enqueue — track buffer address and size
// ============================================================

static SLresult hooked_Enqueue(
    SLAndroidSimpleBufferQueueItf self,
    const void* pBuffer,
    SLuint32 size) {

    {
        std::lock_guard<std::mutex> lock(g_sl_mutex);
        auto it = g_recorder_map.find(self);
        if (it != g_recorder_map.end()) {
            it->second.lastBuffer = const_cast<void*>(pBuffer);
            it->second.lastBufferSize = size;
        }
    }

    return orig_Enqueue(self, pBuffer, size);
}

// ============================================================
// Data-only vtable rewrite
// ============================================================
//
// The per-class function tables behind the OpenSL ES interfaces are static
// structs shared by every instance, so swapping one entry hooks the whole
// class. The store goes through got_hook's /proc/self/mem channel - page
// protections bypassed, and a failure is an error, not a signal - with the
// mprotect+direct-store route as fallback for kernels where the mem write
// is refused.
static bool swap_vtable_entry(void** entry_slot, void* hook_fn, void** orig_out,
                              const char* label) {
    if (!entry_slot) return false;
    void* cur = *entry_slot;
    if (!cur) return false;
    if (cur == hook_fn) return true;  // ours already

    bool ok = got_hook::write_ptr((void*)entry_slot, hook_fn);
    if (!ok) {
        long page = sysconf(_SC_PAGESIZE);
        uintptr_t base = reinterpret_cast<uintptr_t>(entry_slot) & ~(uintptr_t)(page - 1);
        if (mprotect(reinterpret_cast<void*>(base), page, PROT_READ | PROT_WRITE) == 0) {
            *entry_slot = hook_fn;
            mprotect(reinterpret_cast<void*>(base), page, PROT_READ);
            ok = true;
        }
    }
    if (!ok) {
        LOGE("vtable-swap %s: write failed", label);
        return false;
    }
    if (orig_out && *orig_out == nullptr) *orig_out = cur;
    LOGI("vtable-swap %s ok (orig=%p)", label, cur);
    return true;
}

// ============================================================
// Hook vtable entries by pointer swap
// ============================================================

static bool try_hook_vtable(SLAndroidSimpleBufferQueueItf bq) {
    if (g_vtable_hooked || !bq) return false;

    // The OpenSL ES interface is a pointer to a vtable struct.
    // SLAndroidSimpleBufferQueueItf is actually:
    //   struct SLAndroidSimpleBufferQueueItf_ {
    //       SLresult (*RegisterCallback)(...);
    //       SLresult (*Clear)(...);
    //       SLresult (*Enqueue)(...);
    //       SLresult (*GetState)(...);
    //   };
    // And SLAndroidSimpleBufferQueueItf = SLAndroidSimpleBufferQueueItf_**

    // Dereference to get vtable pointer
    void** vtable = const_cast<void**>(reinterpret_cast<const void* const*>(*bq));
    if (!vtable) return false;

    // vtable[0] = RegisterCallback
    // vtable[2] = Enqueue (index depends on interface version)
    // But for SLAndroidSimpleBufferQueueItf:
    //   [0] = RegisterCallback
    //   [1] = Clear
    //   [2] = Enqueue
    //   [3] = GetState

    bool r1 = swap_vtable_entry(&vtable[0], (void*)&hooked_RegisterCallback,
                                (void**)&orig_RegisterCallback, "BufferQueue::RegisterCallback");
    if (!r1) LOGE("Failed to hook RegisterCallback");
    bool r2 = swap_vtable_entry(&vtable[2], (void*)&hooked_Enqueue,
                                (void**)&orig_Enqueue, "BufferQueue::Enqueue");
    if (!r2) LOGE("Failed to hook Enqueue");

    g_vtable_hooked = true;
    return r1 && r2;
}

// ============================================================
// Hooked: CreateAudioRecorder — 捕获录音器的音频参数
// ============================================================

static SLresult hooked_CreateAudioRecorder(
    SLEngineItf self,
    SLObjectItf* pRecorder,
    SLDataSource* pAudioSrc,
    SLDataSink* pAudioSnk,
    SLuint32 numInterfaces,
    const SLInterfaceID* pInterfaceIds,
    const SLboolean* pInterfaceRequired) {

    SLresult result = orig_CreateAudioRecorder(self, pRecorder, pAudioSrc, pAudioSnk,
                                                numInterfaces, pInterfaceIds, pInterfaceRequired);

    if (result == SL_RESULT_SUCCESS && pRecorder && *pRecorder) {
        RecorderObjectInfo objInfo{};

        // 从 data sink 中提取音频格式参数
        if (pAudioSnk && pAudioSnk->pFormat) {
            SLDataFormat_PCM* fmt = static_cast<SLDataFormat_PCM*>(pAudioSnk->pFormat);
            if (fmt->formatType == SL_DATAFORMAT_PCM) {
                objInfo.sampleRate = fmt->samplesPerSec; // milliHz
                objInfo.channels = fmt->numChannels;
                LOGI("CreateAudioRecorder: sampleRate=%u milliHz, channels=%u",
                     objInfo.sampleRate, objInfo.channels);
            }
        }

        std::lock_guard<std::mutex> lock(g_sl_mutex);
        g_recorder_objects[*pRecorder] = objInfo;
    }

    return result;
}

// ============================================================
// Hooked: GetInterface — 标记录音器的 buffer queue 并传递音频参数
// ============================================================

static SLresult hooked_GetInterface(
    SLObjectItf self,
    const SLInterfaceID iid,
    void* pInterface) {

    SLresult result = orig_GetInterface(self, iid, pInterface);

    // Only the simple-buffer-queue interface of a known recorder is marked.
    // The original cast the out-pointer unconditionally, so any OTHER
    // interface fetched on a recorder (volume, record, ...) was recorded as a
    // buffer queue and its calls would have been wrapped as one.
    SLAndroidSimpleBufferQueueItf fresh_bq = nullptr;
    if (result == SL_RESULT_SUCCESS && pInterface && iid) {
        bool is_bq_iid =
            iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE ||
            memcmp(iid, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, sizeof(struct SLInterfaceID_)) == 0;
        if (is_bq_iid) {
            std::lock_guard<std::mutex> lock(g_sl_mutex);
            auto it = g_recorder_objects.find(self);
            if (it != g_recorder_objects.end()) {
                // 这是一个录音器对象的 GetInterface 调用
                SLAndroidSimpleBufferQueueItf bq =
                    *static_cast<SLAndroidSimpleBufferQueueItf*>(pInterface);
                if (bq) {
                    g_is_recorder_bq[bq] = true;
                    // 将音频参数传递给 recorder map
                    auto& info = g_recorder_map[bq];
                    info.sampleRate = it->second.sampleRate;
                    info.channels = it->second.channels;
                    fresh_bq = bq;
                    LOGI("GetInterface: marked bq as recorder, sampleRate=%u channels=%u",
                         info.sampleRate, info.channels);
                }
            }
        }
    }

    // The temp-recorder resolution at install time can fail (Realize refused
    // inside this process), leaving the shared BufferQueue table unswapped.
    // Holding the app's own live queue here is the other moment to swap it:
    // every queue the app makes afterwards - this one included - routes
    // RegisterCallback/Enqueue through the wrappers.
    if (fresh_bq && !g_vtable_hooked) {
        LOGI("GetInterface: swapping BufferQueue table from the app's own queue");
        try_hook_vtable(fresh_bq);
    }

    return result;
}

// ============================================================
// Hooked: Destroy — 清理录音器对象跟踪
// ============================================================

static void hooked_Destroy(SLObjectItf self) {
    {
        std::lock_guard<std::mutex> lock(g_sl_mutex);
        g_recorder_objects.erase(self);
    }
    orig_Destroy(self);
}

// ============================================================
// Hook engine vtable to intercept CreateAudioRecorder
// ============================================================

static bool try_hook_engine_vtable(SLEngineItf engine) {
    if (g_engine_vtable_hooked || !engine) return false;

    // SLEngineItf is SLEngineItf_** -> vtable
    void** vtable = const_cast<void**>(reinterpret_cast<const void* const*>(*engine));
    if (!vtable) return false;

    // SLEngineItf_ vtable layout:
    //   [0] = CreateLEDDevice
    //   [1] = CreateVibraDevice
    //   [2] = CreateAudioPlayer
    //   [3] = CreateAudioRecorder
    //   ... etc
    bool ok = swap_vtable_entry(&vtable[3], (void*)&hooked_CreateAudioRecorder,
                                (void**)&orig_CreateAudioRecorder, "Engine::CreateAudioRecorder");
    g_engine_vtable_hooked = true;
    return ok;
}

static bool try_hook_object_vtable(SLObjectItf obj) {
    if (g_object_vtable_hooked || !obj) return false;

    void** vtable = const_cast<void**>(reinterpret_cast<const void* const*>(*obj));
    if (!vtable) return false;

    // SLObjectItf_ vtable layout:
    //   [0] = Realize
    //   [1] = Resume
    //   [2] = GetState
    //   [3] = GetInterface
    //   [4] = RegisterCallback
    //   [5] = AbortAsyncOperation
    //   [6] = Destroy
    //   [7] = SetPriority
    //   [8] = GetPriority
    //   [9] = SetLossOfControlInterfaces

    bool ok = swap_vtable_entry(&vtable[3], (void*)&hooked_GetInterface,
                                (void**)&orig_GetInterface, "Object::GetInterface");
    ok &= swap_vtable_entry(&vtable[6], (void*)&hooked_Destroy,
                            (void**)&orig_Destroy, "Object::Destroy");
    g_object_vtable_hooked = true;
    return ok;
}

// ============================================================
// Try to resolve vtable by creating a temporary recorder
// ============================================================

static void resolve_vtable_via_temp_recorder() {
    SLObjectItf engineObj = nullptr;
    SLEngineItf engine = nullptr;
    SLObjectItf recorderObj = nullptr;

    // Create temporary engine
    SLresult result;
    if (orig_slCreateEngine) {
        result = orig_slCreateEngine(&engineObj, 0, nullptr, 0, nullptr, nullptr);
    } else {
        result = slCreateEngine(&engineObj, 0, nullptr, 0, nullptr, nullptr);
    }

    if (result != SL_RESULT_SUCCESS || !engineObj) {
        LOGI("Could not create temp engine for vtable resolution");
        return;
    }

    result = (*engineObj)->Realize(engineObj, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) {
        (*engineObj)->Destroy(engineObj);
        return;
    }

    result = (*engineObj)->GetInterface(engineObj, SL_IID_ENGINE, &engine);
    if (result != SL_RESULT_SUCCESS) {
        (*engineObj)->Destroy(engineObj);
        return;
    }

    // Hook engine vtable (CreateAudioRecorder) 和 object vtable (GetInterface/Destroy)
    // 用临时对象的 vtable 地址来 hook，因为所有同类对象共享相同的 vtable
    try_hook_engine_vtable(engine);
    try_hook_object_vtable(engineObj);

    // Create a minimal audio recorder to get buffer queue interface
    SLDataLocator_IODevice loc_dev = {
        SL_DATALOCATOR_IODEVICE,
        SL_IODEVICE_AUDIOINPUT,
        SL_DEFAULTDEVICEID_AUDIOINPUT,
        nullptr
    };
    SLDataSource audioSrc = {&loc_dev, nullptr};

    SLDataLocator_AndroidSimpleBufferQueue loc_bq = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1
    };
    SLDataFormat_PCM format_pcm = {
        SL_DATAFORMAT_PCM,
        1,                              // channels
        SL_SAMPLINGRATE_16,             // 16kHz
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_CENTER,
        SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSink audioSnk = {&loc_bq, &format_pcm};

    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean req[] = {SL_BOOLEAN_TRUE};

    result = (*engine)->CreateAudioRecorder(engine, &recorderObj, &audioSrc,
                                            &audioSnk, 1, ids, req);

    if (result != SL_RESULT_SUCCESS || !recorderObj) {
        LOGI("Temp recorder creation failed (result=%d) — will hook via slCreateEngine fallback",
             result);
        (*engineObj)->Destroy(engineObj);
        return;
    }

    result = (*recorderObj)->Realize(recorderObj, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) {
        (*recorderObj)->Destroy(recorderObj);
        (*engineObj)->Destroy(engineObj);
        return;
    }

    SLAndroidSimpleBufferQueueItf bq = nullptr;
    result = (*recorderObj)->GetInterface(recorderObj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &bq);
    if (result == SL_RESULT_SUCCESS && bq) {
        try_hook_vtable(bq);
    }

    // Clean up temporary objects
    (*recorderObj)->Destroy(recorderObj);
    (*engineObj)->Destroy(engineObj);

    LOGI("vtable resolution via temp recorder %s", g_vtable_hooked ? "succeeded" : "failed");
}

// ============================================================
// Fallback: Hook slCreateEngine to intercept recorder creation
// ============================================================

static SLresult hooked_slCreateEngine(
    SLObjectItf* pEngine,
    SLuint32 numOptions,
    const SLEngineOption* pEngineOptions,
    SLuint32 numInterfaces,
    const SLInterfaceID* pInterfaceIds,
    const SLboolean* pInterfaceRequired) {

    SLresult result = orig_slCreateEngine(pEngine, numOptions, pEngineOptions,
                                          numInterfaces, pInterfaceIds, pInterfaceRequired);

    if (result == SL_RESULT_SUCCESS && !g_vtable_hooked) {
        LOGI("slCreateEngine called — attempting deferred vtable resolution");
        // Try again with the app's engine active
        resolve_vtable_via_temp_recorder();
    }

    return result;
}

// ============================================================
// Install hooks
// ============================================================

void install_opensles_hooks() {
    void* handle = dlopen("libOpenSLES.so", RTLD_NOW);
    if (!handle) {
        LOGI("libOpenSLES.so not available - skipping OpenSL ES hooks");
        return;
    }

    // Try to resolve vtable immediately (creates a temp engine + recorder and
    // swaps their shared class-wide function tables)
    resolve_vtable_via_temp_recorder();

    // Fallback: GOT-hook slCreateEngine so a deferred resolution runs on the
    // app's own engine creation, however the caller reaches the function.
    if (!g_vtable_hooked) {
        int hits = got_hook::hook_all("slCreateEngine", (void*)&hooked_slCreateEngine,
                                      (void**)&orig_slCreateEngine);
        if (hits > 0) {
            LOGI("got-hook slCreateEngine patched=%d orig=%p", hits, (void*)orig_slCreateEngine);
        } else {
            // Not fatal: the engine/object vtables are already swapped, and
            // the BufferQueue table arms from the app's own queue inside
            // hooked_GetInterface, so recorder coverage does not depend on
            // catching slCreateEngine itself.
            LOGI("got-hook slCreateEngine patched=0 (BufferQueue arms on first app queue)");
        }
    }

    LOGI("OpenSL ES hooks installation complete (vtable_hooked=%d, data-only, "
         "libOpenSLES text untouched)",
         g_vtable_hooked);
}
