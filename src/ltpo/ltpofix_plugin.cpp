#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <unistd.h>

extern "C" {

struct NyakoRule {
    uint64_t reserved;
    const char* target_class;
    const char* hook_class;
    const char* method_name;
    const char* hook_sig;
};

int nyako_install_dex_data(const void* dex_data, uint32_t dex_size,
                           const NyakoRule* rules, uint32_t rule_count);

extern const uint8_t _binary_classes_dex_start[];
extern const uint8_t _binary_classes_dex_end[];

static int g_pipe_read_fd  = -1;
static int g_pipe_write_fd = -1;
static pthread_once_t g_watcher_once = PTHREAD_ONCE_INIT;

static const NyakoRule kRules[4] = {
    {
        0,
        "com.android.server.display.mode.DisplayModeDirector",
        "com.nyako.ltpofix.LtpoFixHooks",
        "getDesiredDisplayModeSpecs",
        "(Ljava/lang/Object;I)Lcom/android/server/display/mode/DisplayModeDirector$DesiredDisplayModeSpecs;",
    },
    {
        0,
        "com.android.server.display.LocalDisplayAdapter$LocalDisplayDevice",
        "com.nyako.ltpofix.LtpoFixHooks",
        "findSfDisplayModeIdLocked",
        "(Ljava/lang/Object;II)I",
    },
    {
        0,
        "com.android.server.display.mode.DisplayModeDirector$BrightnessObserver",
        "com.nyako.ltpofix.LtpoFixHooks",
        "setDefaultDisplayState",
        "(Ljava/lang/Object;I)V",
    },
    {
        0,
        "com.android.server.display.LocalDisplayAdapter$LocalDisplayDevice",
        "com.nyako.ltpofix.LtpoFixHooks",
        "setDesiredDisplayModeSpecsLocked",
        "(Ljava/lang/Object;Lcom/android/server/display/mode/DisplayModeDirector$DesiredDisplayModeSpecs;)V",
    },
};

static int matches_proc(const char* proc, const char* target) {
    if (!proc || !target) return 0;
    if (strcmp(proc, target) == 0) return 1;
    size_t len = strlen(target);
    if (strncmp(proc, target, len) == 0 && (proc[len] == ':' || proc[len] == '\0')) return 1;
    return 0;
}

static void publish_pipe_fd_to_jvm(int read_fd) {
    char fd_str[24];
    snprintf(fd_str, sizeof(fd_str), "%d", read_fd);
    setenv("NYAKO_LTPO_PIPE_FD", fd_str, 1);

    using GetVMsFn = jint (*)(JavaVM**, jsize, jsize*);
    auto get_vms = reinterpret_cast<GetVMsFn>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if (!get_vms) {
        static const char* const kArtPaths[] = {
            "/apex/com.android.art/lib64/libart.so",
            "/system/lib64/libart.so",
            "libart.so",
        };
        for (const char* path : kArtPaths) {
            void* h = dlopen(path, RTLD_NOLOAD | RTLD_NOW);
            if (!h) h = dlopen(path, RTLD_NOW);
            if (h) {
                get_vms = reinterpret_cast<GetVMsFn>(dlsym(h, "JNI_GetCreatedJavaVMs"));
                if (get_vms) break;
            }
        }
    }
    if (!get_vms) {
        __android_log_print(ANDROID_LOG_INFO, "NyakoLtpoFix",
                            "env NYAKO_LTPO_PIPE_FD=%d set (no libart JVM sym)", read_fd);
        return;
    }
    JavaVM* vm = nullptr;
    jsize count = 0;
    if (get_vms(&vm, 1, &count) != JNI_OK || count < 1 || !vm) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
    }
    if (!env) return;

    jclass sysCls = env->FindClass("java/lang/System");
    if (sysCls) {
        jmethodID setProp = env->GetStaticMethodID(
            sysCls, "setProperty",
            "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
        if (setProp) {
            jstring k = env->NewStringUTF("nyako.ltpo.pipe_fd");
            jstring v = env->NewStringUTF(fd_str);
            jobject old = env->CallStaticObjectMethod(sysCls, setProp, k, v);
            if (old) env->DeleteLocalRef(old);
            env->DeleteLocalRef(v);
            env->DeleteLocalRef(k);
        }
        env->DeleteLocalRef(sysCls);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) vm->DetachCurrentThread();
}

static void* sf_idle_futex_thread(void*) {
    __android_log_print(ANDROID_LOG_INFO, "NyakoLtpoFix",
                        "futex watcher active (pipe_r=%d pipe_w=%d)", g_pipe_read_fd, g_pipe_write_fd);
    uint32_t global_ser = __system_property_area_serial();
    const prop_info* pi = __system_property_find("debug.nyako.ltpo.sf_idle");
    while (!pi) {
        uint32_t next_g = global_ser;
        __system_property_wait(nullptr, global_ser, &next_g, nullptr);
        global_ser = next_g;
        pi = __system_property_find("debug.nyako.ltpo.sf_idle");
    }

    uint32_t cur_ser = __system_property_serial(pi);
    {
        uint8_t b = 1;
        ssize_t r = write(g_pipe_write_fd, &b, 1);
        (void)r;
    }

    for (;;) {
        uint32_t next_ser = cur_ser;
        __system_property_wait(pi, cur_ser, &next_ser, nullptr);
        if (next_ser == cur_ser) continue;
        cur_ser = next_ser;
        uint8_t b = 1;
        ssize_t r = write(g_pipe_write_fd, &b, 1);
        (void)r;
    }
    return nullptr;
}

static void start_futex_pipe_watcher() {
    int fds[2] = {-1, -1};
    if (pipe2(fds, O_CLOEXEC) != 0) {
        __android_log_print(ANDROID_LOG_WARN, "NyakoLtpoFix", "pipe2 failed");
        return;
    }
    int flags = fcntl(fds[1], F_GETFL, 0);
    if (flags >= 0) fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);
    g_pipe_read_fd = fds[0];
    g_pipe_write_fd = fds[1];

    publish_pipe_fd_to_jvm(g_pipe_read_fd);

    pthread_t tid;
    if (pthread_create(&tid, nullptr, sf_idle_futex_thread, nullptr) == 0) {
        pthread_setname_np(tid, "NyakoLtpoWait");
        pthread_detach(tid);
    }
}

__attribute__((visibility("default")))
const char* nyako_plugin_get_name() {
    return "LTPO Base Mode Override (ColorOS)";
}

__attribute__((visibility("default")))
int nyako_plugin_get_version() {
    return 8;
}

__attribute__((visibility("default")))
const char* nyako_plugin_get_scope() {
    return "android";
}

__attribute__((visibility("default")))
const char* nyako_plugin_get_desc() {
    return "Zero-CPU hardware 1-120Hz LTPO with synchronized DisplayModeSpecs range safety";
}

__attribute__((visibility("default")))
const char* nyako_plugin_get_api() {
    return "android";
}

__attribute__((visibility("default")))
int nyako_plugin_entry(const char* proc_name) {
    if (!matches_proc(proc_name, "android") && !matches_proc(proc_name, "system_server")) {
        return 1;
    }
    uint32_t dex_size = static_cast<uint32_t>(_binary_classes_dex_end - _binary_classes_dex_start);
    __android_log_print(ANDROID_LOG_INFO, "NyakoLtpoFix", "install: %u rule(s) (dex=%uB)", 4u, dex_size);
    int rc = nyako_install_dex_data(_binary_classes_dex_start, dex_size, kRules, 4u);
    pthread_once(&g_watcher_once, start_futex_pipe_watcher);
    return rc;
}

} // extern "C"
