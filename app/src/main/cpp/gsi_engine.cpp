#include <jni.h>
#include <string>
#include <sstream>
#include <memory>
#include <mutex>

#include "include/logger.h"
#include "include/image_verifier.h"
#include "include/display_pipeline.h"
#include "include/block_device.h"
#include "include/ext4_reader.h"
#include "include/sandbox_manager.h"
#include "include/process_spawner.h"
#include "include/virtual_framebuffer.h"
#include "include/input_bridge.h"
#include "include/virtual_binder.h"
#include "include/hook_engine.h"
#include "include/property_service.h"
#include "include/boot_manager.h"
#include "include/gralloc_bridge.h"
#include "include/network_bridge.h"
#include "include/logcat_broker.h"

namespace {
    std::mutex gFsMutex;
    std::shared_ptr<gsi::IBlockDevice> gBlockDev;
    std::unique_ptr<gsi::Ext4Reader> gExt4Reader;
}

extern "C" {

JNIEXPORT jstring JNICALL
Java_com_gsi_runtime_GsiEngine_nativeGetEngineInfo(JNIEnv* env, jobject /* this */) {
    std::ostringstream oss;
    oss << "GSI Core Engine v1.7.0 [ABI: "
#if defined(__aarch64__)
        << "arm64-v8a"
#elif defined(__x86_64__)
        << "x86_64"
#elif defined(__arm__)
        << "armeabi-v7a"
#elif defined(__i386__)
        << "x86"
#else
        << "unknown"
#endif
        << " | Gralloc & Graphics Bridge | Init Boot Engine | Property Service | Privilege Spoofing | Virtual Binder | VFB & Touch | EXT4 VFS]";
    return env->NewStringUTF(oss.str().c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativeInit(JNIEnv* /* env */, jobject /* this */) {
    LOGI("GSI Native Engine initialized successfully");
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeSetSurface(JNIEnv* env, jobject /* this */, jobject surface) {
    LOGI("Java_com_gsi_runtime_GsiEngine_nativeSetSurface called");
    gsi::DisplayPipeline::getInstance().setSurface(env, surface);
}

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeClearSurface(JNIEnv* /* env */, jobject /* this */) {
    LOGI("Java_com_gsi_runtime_GsiEngine_nativeClearSurface called");
    gsi::DisplayPipeline::getInstance().clearSurface();
}

JNIEXPORT jobject JNICALL
Java_com_gsi_runtime_GsiEngine_nativeVerifyImageFd(JNIEnv* env, jobject /* this */, jint fd) {
    LOGI("Verifying image file descriptor: %d", fd);
    gsi::ImageVerificationResult res = gsi::ImageVerifier::verifyFileDescriptor(fd);

    jclass infoClass = env->FindClass("com/gsi/runtime/GsiImageInfo");
    if (!infoClass) {
        LOGE("Cannot find class com/gsi/runtime/GsiImageInfo");
        return nullptr;
    }

    jmethodID ctor = env->GetMethodID(infoClass, "<init>", "(ZLjava/lang/String;JJJLjava/lang/String;Ljava/lang/String;)V");
    if (!ctor) {
        LOGE("Cannot find constructor for GsiImageInfo");
        return nullptr;
    }

    std::string formatStr;
    switch (res.format) {
        case gsi::ImageFormat::ANDROID_SPARSE: formatStr = "Android Sparse"; break;
        case gsi::ImageFormat::RAW_EXT4: formatStr = "Raw EXT4"; break;
        default: formatStr = "Unknown"; break;
    }

    jstring jFormat = env->NewStringUTF(formatStr.c_str());
    jstring jDesc = env->NewStringUTF(res.description.c_str());
    jstring jVolume = env->NewStringUTF(res.volumeName.c_str());

    jobject obj = env->NewObject(
        infoClass, ctor,
        res.isValid ? JNI_TRUE : JNI_FALSE,
        jFormat,
        static_cast<jlong>(res.blockSize),
        static_cast<jlong>(res.totalBlocks),
        static_cast<jlong>(res.uncompressedSizeBytes),
        jDesc,
        jVolume
    );

    return obj;
}

// -------------------------------------------------------------
// Tahap 2: User-Space Filesystem & EXT4 Parser APIs
// -------------------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativeOpenFilesystem(JNIEnv* /* env */, jobject /* this */, jint fd) {
    std::lock_guard<std::mutex> lock(gFsMutex);

    gExt4Reader.reset();
    gBlockDev.reset();

    auto sparseDev = gsi::SparseBlockDevice::create(fd);
    if (sparseDev) {
        LOGI("Opened SparseBlockDevice successfully");
        gBlockDev = std::move(sparseDev);
    } else {
        auto ver = gsi::ImageVerifier::verifyFileDescriptor(fd);
        if (ver.isValid && ver.format == gsi::ImageFormat::RAW_EXT4) {
            LOGI("Opened RawBlockDevice successfully");
            gBlockDev = std::make_shared<gsi::RawBlockDevice>(fd, ver.blockSize, ver.totalBlocks);
        } else {
            LOGE("Failed to open block device: Neither sparse nor raw EXT4");
            return JNI_FALSE;
        }
    }

    gExt4Reader = gsi::Ext4Reader::open(gBlockDev);
    if (!gExt4Reader) {
        LOGE("Failed to initialize user-space EXT4 reader");
        gBlockDev.reset();
        return JNI_FALSE;
    }

    LOGI("User-Space EXT4 filesystem mounted successfully in memory (Volume: \"%s\")",
         gExt4Reader->getVolumeName().c_str());
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeCloseFilesystem(JNIEnv* /* env */, jobject /* this */) {
    std::lock_guard<std::mutex> lock(gFsMutex);
    gExt4Reader.reset();
    gBlockDev.reset();
    LOGI("User-Space EXT4 filesystem unmounted and closed");
}

JNIEXPORT jobject JNICALL
Java_com_gsi_runtime_GsiEngine_nativeGetBuildInfo(JNIEnv* env, jobject /* this */) {
    std::lock_guard<std::mutex> lock(gFsMutex);
    if (!gExt4Reader) {
        LOGW("nativeGetBuildInfo: Filesystem not opened");
        return nullptr;
    }

    gsi::BuildPropInfo prop = gExt4Reader->extractBuildProp();

    jclass infoClass = env->FindClass("com/gsi/runtime/GsiBuildInfo");
    if (!infoClass) return nullptr;

    jmethodID ctor = env->GetMethodID(
        infoClass,
        "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ZLjava/lang/String;)V"
    );
    if (!ctor) return nullptr;

    jstring jOs = env->NewStringUTF(prop.osVersion.c_str());
    jstring jSdk = env->NewStringUTF(prop.sdkVersion.c_str());
    jstring jBuild = env->NewStringUTF(prop.buildId.c_str());
    jstring jPatch = env->NewStringUTF(prop.securityPatch.c_str());
    jstring jModel = env->NewStringUTF(prop.model.c_str());
    jstring jFp = env->NewStringUTF(prop.fingerprint.c_str());
    jstring jRaw = env->NewStringUTF(prop.rawContent.c_str());

    jobject obj = env->NewObject(
        infoClass, ctor,
        jOs, jSdk, jBuild, jPatch, jModel, jFp,
        prop.isTrebleEnabled ? JNI_TRUE : JNI_FALSE,
        jRaw
    );

    return obj;
}

JNIEXPORT jobjectArray JNICALL
Java_com_gsi_runtime_GsiEngine_nativeListDirectory(JNIEnv* env, jobject /* this */, jstring jPath) {
    std::lock_guard<std::mutex> lock(gFsMutex);
    if (!gExt4Reader) return nullptr;

    const char* pathChars = env->GetStringUTFChars(jPath, nullptr);
    std::string path = pathChars ? pathChars : "/";
    if (pathChars) env->ReleaseStringUTFChars(jPath, pathChars);

    std::vector<gsi::FsFileEntry> entries;
    if (!gExt4Reader->listDirectory(path, entries)) {
        LOGE("Failed to list directory: %s", path.c_str());
        return nullptr;
    }

    jclass entryClass = env->FindClass("com/gsi/runtime/GsiFileEntry");
    if (!entryClass) return nullptr;

    jmethodID ctor = env->GetMethodID(
        entryClass,
        "<init>",
        "(Ljava/lang/String;Ljava/lang/String;JJZZ)V"
    );
    if (!ctor) return nullptr;

    jobjectArray array = env->NewObjectArray(entries.size(), entryClass, nullptr);
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        jstring jName = env->NewStringUTF(e.name.c_str());
        jstring jP = env->NewStringUTF(e.path.c_str());

        jobject item = env->NewObject(
            entryClass, ctor,
            jName, jP,
            static_cast<jlong>(e.inode),
            static_cast<jlong>(e.size),
            e.isDirectory ? JNI_TRUE : JNI_FALSE,
            e.isSymlink ? JNI_TRUE : JNI_FALSE
        );

        env->SetObjectArrayElement(array, i, item);
        env->DeleteLocalRef(item);
        env->DeleteLocalRef(jName);
        env->DeleteLocalRef(jP);
    }

    return array;
}

JNIEXPORT jstring JNICALL
Java_com_gsi_runtime_GsiEngine_nativeReadFileText(JNIEnv* env, jobject /* this */, jstring jPath) {
    std::lock_guard<std::mutex> lock(gFsMutex);
    if (!gExt4Reader) return nullptr;

    const char* pathChars = env->GetStringUTFChars(jPath, nullptr);
    std::string path = pathChars ? pathChars : "";
    if (pathChars) env->ReleaseStringUTFChars(jPath, pathChars);

    std::string content;
    if (!gExt4Reader->readFileString(path, content)) {
        return nullptr;
    }

    return env->NewStringUTF(content.c_str());
}

// -------------------------------------------------------------
// Tahap 3: Sandbox Manager & Process Spawner APIs
// -------------------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativePrepareSandbox(JNIEnv* env, jobject /* this */, jstring jSandboxDir) {
    const char* dir = env->GetStringUTFChars(jSandboxDir, nullptr);
    std::string sDir = dir ? dir : "";
    if (dir) env->ReleaseStringUTFChars(jSandboxDir, dir);

    return gsi::SandboxManager::getInstance().initializeSandbox(sDir) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativeExtractBinaryFromVfs(JNIEnv* env, jobject /* this */, jstring jVfsPath, jstring jTargetName) {
    std::lock_guard<std::mutex> lock(gFsMutex);
    if (!gExt4Reader) return JNI_FALSE;

    const char* vfsPath = env->GetStringUTFChars(jVfsPath, nullptr);
    const char* targetName = env->GetStringUTFChars(jTargetName, nullptr);
    std::string sVfs = vfsPath ? vfsPath : "";
    std::string sTarget = targetName ? targetName : "";
    if (vfsPath) env->ReleaseStringUTFChars(jVfsPath, vfsPath);
    if (targetName) env->ReleaseStringUTFChars(jTargetName, targetName);

    bool ok = gsi::SandboxManager::getInstance().extractBinaryFromVfs(*gExt4Reader, sVfs, sTarget);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativeExecuteCommand(JNIEnv* env, jobject /* this */, jstring jCmd, jstring jSandboxDir) {
    const char* cmd = env->GetStringUTFChars(jCmd, nullptr);
    const char* sDir = env->GetStringUTFChars(jSandboxDir, nullptr);
    std::string sCmd = cmd ? cmd : "";
    std::string sSandbox = sDir ? sDir : "";
    if (cmd) env->ReleaseStringUTFChars(jCmd, cmd);
    if (sDir) env->ReleaseStringUTFChars(jSandboxDir, sDir);

    return gsi::ProcessSpawner::getInstance().execute(sCmd, sSandbox) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_gsi_runtime_GsiEngine_nativePollProcessOutput(JNIEnv* env, jobject /* this */) {
    std::string out = gsi::ProcessSpawner::getInstance().pollOutput();
    return env->NewStringUTF(out.c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativeSendProcessInput(JNIEnv* env, jobject /* this */, jstring jInput) {
    const char* inp = env->GetStringUTFChars(jInput, nullptr);
    std::string sInp = inp ? inp : "";
    if (inp) env->ReleaseStringUTFChars(jInput, inp);

    return gsi::ProcessSpawner::getInstance().sendInput(sInp) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativeIsProcessActive(JNIEnv* /* env */, jobject /* this */) {
    return gsi::ProcessSpawner::getInstance().isRunning() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeKillProcess(JNIEnv* /* env */, jobject /* this */) {
    gsi::ProcessSpawner::getInstance().terminate();
}

// -------------------------------------------------------------
// Tahap 4: Virtual Framebuffer & Touch Input APIs
// -------------------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativeInitVfb(JNIEnv* env, jobject /* this */, jint width, jint height, jstring jSandboxDir) {
    const char* dir = env->GetStringUTFChars(jSandboxDir, nullptr);
    std::string sDir = dir ? dir : "";
    if (dir) env->ReleaseStringUTFChars(jSandboxDir, dir);

    bool vfbOk = gsi::VirtualFramebuffer::getInstance().initialize(
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        sDir
    );

    bool inputOk = gsi::InputBridge::getInstance().initialize(
        sDir,
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height)
    );

    return (vfbOk && inputOk) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeSetVfbMode(JNIEnv* /* env */, jobject /* this */, jboolean enable) {
    gsi::DisplayPipeline::getInstance().setVfbMode(enable == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativeIsVfbMode(JNIEnv* /* env */, jobject /* this */) {
    return gsi::DisplayPipeline::getInstance().isVfbMode() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeSendTouchEvent(JNIEnv* /* env */, jobject /* this */, jint action, jfloat normX, jfloat normY, jint pointerId) {
    gsi::InputBridge::getInstance().injectTouchEvent(action, normX, normY, pointerId);
}

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeDrawTouchFeedback(JNIEnv* /* env */, jobject /* this */, jint x, jint y, jint color, jint radius) {
    gsi::VirtualFramebuffer::getInstance().drawTouchCircle(x, y, static_cast<uint32_t>(color), radius);
}

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeClearVfb(JNIEnv* /* env */, jobject /* this */, jint color) {
    gsi::VirtualFramebuffer::getInstance().clearScreen(static_cast<uint32_t>(color));
}

// -------------------------------------------------------------
// Tahap 5: Virtual Binder & Hook Deployment APIs
// -------------------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativeDeployHookLibrary(JNIEnv* env, jobject /* this */, jstring jHostLibDir) {
    const char* dir = env->GetStringUTFChars(jHostLibDir, nullptr);
    std::string sDir = dir ? dir : "";
    if (dir) env->ReleaseStringUTFChars(jHostLibDir, dir);

    return gsi::SandboxManager::getInstance().deployHookLibrary(sDir) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_gsi_runtime_GsiEngine_nativeRunBinderDiagnostic(JNIEnv* env, jobject /* this */) {
    std::string report = gsi::VirtualBinderBroker::getInstance().runSelfTest();
    return env->NewStringUTF(report.c_str());
}

// -------------------------------------------------------------
// Display Pipeline Native APIs
// -------------------------------------------------------------

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeStartTestRender(JNIEnv* /* env */, jobject /* this */) {
    gsi::DisplayPipeline::getInstance().startTestRender();
}

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeStopTestRender(JNIEnv* /* env */, jobject /* this */) {
    gsi::DisplayPipeline::getInstance().stopTestRender();
}

JNIEXPORT jint JNICALL
Java_com_gsi_runtime_GsiEngine_nativeGetRenderedFrames(JNIEnv* /* env */, jobject /* this */) {
    return gsi::DisplayPipeline::getInstance().getRenderedFrames();
}

// -------------------------------------------------------------
// Tahap 6: Property Service & Privilege Spoofing APIs
// -------------------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativeStartPropertyService(JNIEnv* env, jobject /* this */, jstring jSandboxDir, jstring jBuildPropText) {
    const char* dirChars = env->GetStringUTFChars(jSandboxDir, nullptr);
    std::string sDir = dirChars ? dirChars : "";
    if (dirChars) env->ReleaseStringUTFChars(jSandboxDir, dirChars);

    bool ok = gsi::PropertyService::getInstance().start(sDir);
    if (!ok) return JNI_FALSE;

    if (jBuildPropText != nullptr) {
        const char* propChars = env->GetStringUTFChars(jBuildPropText, nullptr);
        if (propChars) {
            std::string propContent(propChars);
            env->ReleaseStringUTFChars(jBuildPropText, propChars);
            gsi::PropertyService::getInstance().loadFromBuildProp(propContent);
        }
    }

    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeStopPropertyService(JNIEnv* /* env */, jobject /* this */) {
    gsi::PropertyService::getInstance().stop();
}

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeSetProperty(JNIEnv* env, jobject /* this */, jstring jKey, jstring jValue) {
    const char* kChars = env->GetStringUTFChars(jKey, nullptr);
    const char* vChars = env->GetStringUTFChars(jValue, nullptr);
    std::string key = kChars ? kChars : "";
    std::string val = vChars ? vChars : "";
    if (kChars) env->ReleaseStringUTFChars(jKey, kChars);
    if (vChars) env->ReleaseStringUTFChars(jValue, vChars);

    if (!key.empty()) {
        gsi::PropertyService::getInstance().setProperty(key, val);
    }
}

JNIEXPORT jstring JNICALL
Java_com_gsi_runtime_GsiEngine_nativeGetProperty(JNIEnv* env, jobject /* this */, jstring jKey, jstring jDefaultVal) {
    const char* kChars = env->GetStringUTFChars(jKey, nullptr);
    const char* dChars = jDefaultVal ? env->GetStringUTFChars(jDefaultVal, nullptr) : nullptr;
    std::string key = kChars ? kChars : "";
    std::string def = dChars ? dChars : "";
    if (kChars) env->ReleaseStringUTFChars(jKey, kChars);
    if (dChars) env->ReleaseStringUTFChars(jDefaultVal, dChars);

    std::string val = gsi::PropertyService::getInstance().getProperty(key, def);
    return env->NewStringUTF(val.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_gsi_runtime_GsiEngine_nativeGetAllProperties(JNIEnv* env, jobject /* this */) {
    std::string all = gsi::PropertyService::getInstance().getAllPropertiesFormatted();
    return env->NewStringUTF(all.c_str());
}

JNIEXPORT jint JNICALL
Java_com_gsi_runtime_GsiEngine_nativeGetPropertyCount(JNIEnv* /* env */, jobject /* this */) {
    return static_cast<jint>(gsi::PropertyService::getInstance().getPropertyCount());
}

JNIEXPORT jstring JNICALL
Java_com_gsi_runtime_GsiEngine_nativeRunPrivilegeDiagnostic(JNIEnv* env, jobject /* this */) {
    const char* report = gsi_run_privilege_diagnostic();
    return env->NewStringUTF(report ? report : "Diagnostic returned null");
}

// -------------------------------------------------------------
// Tahap 7 / Option B: Init Boot Sequence & Service Supervisor APIs
// -------------------------------------------------------------

JNIEXPORT jint JNICALL
Java_com_gsi_runtime_GsiEngine_nativeLoadInitFromVfs(JNIEnv* /* env */, jobject /* this */) {
    std::lock_guard<std::mutex> lock(gFsMutex);
    if (!gExt4Reader) {
        LOGW("nativeLoadInitFromVfs: Filesystem not opened");
        return 0;
    }
    size_t count = gsi::BootManager::getInstance().loadInitFromVfs(*gExt4Reader);
    return static_cast<jint>(count);
}

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativeStartBootSequence(JNIEnv* env, jobject /* this */, jstring jSandboxDir) {
    const char* dir = env->GetStringUTFChars(jSandboxDir, nullptr);
    std::string sDir = dir ? dir : "";
    if (dir) env->ReleaseStringUTFChars(jSandboxDir, dir);

    return gsi::BootManager::getInstance().startBootSequence(sDir) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeStopBootSequence(JNIEnv* /* env */, jobject /* this */) {
    gsi::BootManager::getInstance().stopBootSequence();
}

JNIEXPORT jstring JNICALL
Java_com_gsi_runtime_GsiEngine_nativeGetBootPhase(JNIEnv* env, jobject /* this */) {
    std::string phase = gsi::BootManager::getInstance().getPhaseString();
    return env->NewStringUTF(phase.c_str());
}

JNIEXPORT jobjectArray JNICALL
Java_com_gsi_runtime_GsiEngine_nativeGetServicesList(JNIEnv* env, jobject /* this */) {
    auto services = gsi::BootManager::getInstance().getServicesSnapshot();

    jclass infoClass = env->FindClass("com/gsi/runtime/GsiServiceInfo");
    if (!infoClass) return nullptr;

    jmethodID ctor = env->GetMethodID(
        infoClass,
        "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V"
    );
    if (!ctor) return nullptr;

    jobjectArray arr = env->NewObjectArray(services.size(), infoClass, nullptr);
    for (size_t i = 0; i < services.size(); ++i) {
        const auto& s = services[i];

        std::string classStr;
        for (size_t c = 0; c < s.classes.size(); ++c) {
            if (c > 0) classStr += ", ";
            classStr += s.classes[c];
        }

        jstring jName = env->NewStringUTF(s.name.c_str());
        jstring jBin = env->NewStringUTF(s.binaryPath.c_str());
        jstring jCls = env->NewStringUTF(classStr.c_str());
        jstring jUser = env->NewStringUTF(s.user.c_str());
        jstring jStatus = env->NewStringUTF(s.status.c_str());

        jobject item = env->NewObject(
            infoClass, ctor,
            jName, jBin, jCls, jUser, jStatus, static_cast<jint>(s.pid)
        );

        env->SetObjectArrayElement(arr, i, item);
        env->DeleteLocalRef(item);
        env->DeleteLocalRef(jName);
        env->DeleteLocalRef(jBin);
        env->DeleteLocalRef(jCls);
        env->DeleteLocalRef(jUser);
        env->DeleteLocalRef(jStatus);
    }

    return arr;
}

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativeControlService(JNIEnv* env, jobject /* this */, jstring jName, jint action) {
    const char* nameChars = env->GetStringUTFChars(jName, nullptr);
    std::string sName = nameChars ? nameChars : "";
    if (nameChars) env->ReleaseStringUTFChars(jName, nameChars);

    if (action == 1) {
        return gsi::BootManager::getInstance().startService(sName) ? JNI_TRUE : JNI_FALSE;
    } else if (action == 2) {
        return gsi::BootManager::getInstance().stopService(sName) ? JNI_TRUE : JNI_FALSE;
    } else if (action == 3) {
        return gsi::BootManager::getInstance().restartService(sName) ? JNI_TRUE : JNI_FALSE;
    }

    return JNI_FALSE;
}

// -------------------------------------------------------------
// Tahap 8 / Option C: Gralloc & Synthetic Graphics Bridge APIs
// -------------------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativeInitGralloc(JNIEnv* env, jobject /* this */, jstring jSandboxDir) {
    const char* dirChars = env->GetStringUTFChars(jSandboxDir, nullptr);
    std::string sDir = dirChars ? dirChars : "";
    if (dirChars) env->ReleaseStringUTFChars(jSandboxDir, dirChars);

    return gsi::GrallocBridge::getInstance().initialize(sDir) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeStartBootAnimation(JNIEnv* /* env */, jobject /* this */, jboolean enable) {
    if (enable) {
        gsi::GrallocBridge::getInstance().startBootAnimation();
    } else {
        gsi::GrallocBridge::getInstance().stopBootAnimation();
    }
}

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativeIsBootAnimationActive(JNIEnv* /* env */, jobject /* this */) {
    return gsi::GrallocBridge::getInstance().isBootAnimationActive() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_gsi_runtime_GsiEngine_nativeGetGraphicsStats(JNIEnv* env, jobject /* this */) {
    std::string stats = gsi::GrallocBridge::getInstance().getGraphicsStats();
    return env->NewStringUTF(stats.c_str());
}

// -------------------------------------------------------------
// Pilar 1: Virtual Network & DNS Bridge APIs
// -------------------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativeInitNetwork(JNIEnv* env, jobject /* this */, jstring jSandboxDir, jstring jPrimaryDns, jstring jSecondaryDns) {
    const char* dirChars = env->GetStringUTFChars(jSandboxDir, nullptr);
    std::string sDir = dirChars ? dirChars : "";
    if (dirChars) env->ReleaseStringUTFChars(jSandboxDir, dirChars);

    const char* dns1Chars = jPrimaryDns ? env->GetStringUTFChars(jPrimaryDns, nullptr) : nullptr;
    std::string dns1 = dns1Chars ? dns1Chars : "8.8.8.8";
    if (dns1Chars) env->ReleaseStringUTFChars(jPrimaryDns, dns1Chars);

    const char* dns2Chars = jSecondaryDns ? env->GetStringUTFChars(jSecondaryDns, nullptr) : nullptr;
    std::string dns2 = dns2Chars ? dns2Chars : "1.1.1.1";
    if (dns2Chars) env->ReleaseStringUTFChars(jSecondaryDns, dns2Chars);

    return gsi::NetworkBridge::getInstance().initialize(sDir, dns1, dns2) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeShutdownNetwork(JNIEnv* /* env */, jobject /* this */) {
    gsi::NetworkBridge::getInstance().shutdown();
}

JNIEXPORT jstring JNICALL
Java_com_gsi_runtime_GsiEngine_nativeTestNetworkConnectivity(JNIEnv* env, jobject /* this */, jstring jHost, jint jPort) {
    const char* hostChars = env->GetStringUTFChars(jHost, nullptr);
    std::string host = hostChars ? hostChars : "google.com";
    if (hostChars) env->ReleaseStringUTFChars(jHost, hostChars);

    std::string result = gsi::NetworkBridge::getInstance().testConnectivity(host, jPort);
    return env->NewStringUTF(result.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_gsi_runtime_GsiEngine_nativeGetNetworkStats(JNIEnv* env, jobject /* this */) {
    std::string stats = gsi::NetworkBridge::getInstance().getNetworkStatsString();
    return env->NewStringUTF(stats.c_str());
}

// -------------------------------------------------------------
// Pilar 2: Real-Time Logcat Broker & Tombstone APIs
// -------------------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_gsi_runtime_GsiEngine_nativeInitLogcatBroker(JNIEnv* env, jobject /* this */, jstring jSandboxDir) {
    const char* dirChars = env->GetStringUTFChars(jSandboxDir, nullptr);
    std::string sDir = dirChars ? dirChars : "";
    if (dirChars) env->ReleaseStringUTFChars(jSandboxDir, dirChars);

    return gsi::LogcatBroker::getInstance().start(sDir) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeShutdownLogcatBroker(JNIEnv* /* env */, jobject /* this */) {
    gsi::LogcatBroker::getInstance().stop();
}

JNIEXPORT jstring JNICALL
Java_com_gsi_runtime_GsiEngine_nativeGetRecentLogs(JNIEnv* env, jobject /* this */, jint jBufferId, jint jMinPrio, jint jMaxLines) {
    std::string logs = gsi::LogcatBroker::getInstance().getRecentLogs(jBufferId, jMinPrio, jMaxLines);
    return env->NewStringUTF(logs.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_gsi_runtime_GsiEngine_nativeGetTombstoneReport(JNIEnv* env, jobject /* this */) {
    std::string report = gsi::LogcatBroker::getInstance().getLatestTombstone();
    return env->NewStringUTF(report.c_str());
}

JNIEXPORT void JNICALL
Java_com_gsi_runtime_GsiEngine_nativeClearLogs(JNIEnv* /* env */, jobject /* this */) {
    gsi::LogcatBroker::getInstance().clearLogs();
}

JNIEXPORT jstring JNICALL
Java_com_gsi_runtime_GsiEngine_nativeGetLogStats(JNIEnv* env, jobject /* this */) {
    std::string stats = gsi::LogcatBroker::getInstance().getStatsString();
    return env->NewStringUTF(stats.c_str());
}

JNIEXPORT jstring JNICALL
Java_com_gsi_runtime_GsiEngine_nativeInjectTestCrash(JNIEnv* env, jobject /* this */, jstring jReason) {
    const char* rChars = env->GetStringUTFChars(jReason, nullptr);
    std::string reason = rChars ? rChars : "Diagnostic null pointer dereference simulation";
    if (rChars) env->ReleaseStringUTFChars(jReason, rChars);

    std::string regDump =
        "  x0  0000000000000000  x1  0000007fe812a030  x2  0000000000000020  x3  0000007fe812a040\n"
        "  x4  0000000000000004  x5  0000000000000000  x6  0000000000000000  x7  0000007fe8129fd0\n"
        "  sp  0000007fe8129f80  lr  0000007d4321b0a8  pc  0000007d4321a004  pst 0000000060000000";

    std::string backtrace =
        "  #00 pc 000000000001a004  /system/lib64/libsurfaceflinger.so (android::SurfaceFlinger::init+48)\n"
        "  #01 pc 000000000002b0a8  /system/bin/surfaceflinger (main+120)\n"
        "  #02 pc 000000000004f120  /system/lib64/bionic/libc.so (__libc_init+108)";

    std::string tombstone = gsi::LogcatBroker::getInstance().dumpTombstone(
        getpid(), gettid(), 11, nullptr, reason, regDump, backtrace
    );

    return env->NewStringUTF(tombstone.c_str());
}

} // extern "C"
