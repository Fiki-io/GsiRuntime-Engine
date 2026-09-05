package com.gsi.runtime

import android.view.Surface
import android.util.Log

object GsiEngine {
    private const val TAG = "GSI-Engine-JNI"

    init {
        try {
            System.loadLibrary("gsi_core")
            Log.i(TAG, "Successfully loaded native library libgsi_core.so")
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "Failed to load libgsi_core.so", e)
        }
    }

    // Engine Info & Core
    external fun nativeGetEngineInfo(): String
    external fun nativeInit(): Boolean

    // Image Verification
    external fun nativeVerifyImageFd(fd: Int): GsiImageInfo?

    // User-Space Filesystem & EXT4
    external fun nativeOpenFilesystem(fd: Int): Boolean
    external fun nativeCloseFilesystem()
    external fun nativeGetBuildInfo(): GsiBuildInfo?
    external fun nativeListDirectory(path: String): Array<GsiFileEntry>?
    external fun nativeReadFileText(path: String): String?

    // Process Spawner & Sandbox
    external fun nativePrepareSandbox(sandboxDir: String): Boolean
    external fun nativeExtractBinaryFromVfs(vfsPath: String, targetName: String): Boolean
    external fun nativeExecuteCommand(cmd: String, sandboxDir: String): Boolean
    external fun nativePollProcessOutput(): String
    external fun nativeSendProcessInput(input: String): Boolean
    external fun nativeIsProcessActive(): Boolean
    external fun nativeKillProcess()

    // Virtual Framebuffer & Touch Input (Tahap 4)
    external fun nativeInitVfb(width: Int, height: Int, sandboxDir: String): Boolean
    external fun nativeSetVfbMode(enable: Boolean)
    external fun nativeIsVfbMode(): Boolean
    external fun nativeSendTouchEvent(action: Int, normX: Float, normY: Float, pointerId: Int)
    external fun nativeDrawTouchFeedback(x: Int, y: Int, color: Int, radius: Int)
    external fun nativeClearVfb(color: Int)

    // Virtual Binder & In-Process Hooking (Tahap 5)
    external fun nativeDeployHookLibrary(hostLibDir: String): Boolean
    external fun nativeRunBinderDiagnostic(): String

    // User-Space Property Service & Privilege Spoofing (Tahap 6 / Option A)
    external fun nativeStartPropertyService(sandboxDir: String, buildPropText: String?): Boolean
    external fun nativeStopPropertyService()
    external fun nativeSetProperty(key: String, value: String)
    external fun nativeGetProperty(key: String, defaultVal: String = ""): String
    external fun nativeGetAllProperties(): String
    external fun nativeGetPropertyCount(): Int
    external fun nativeRunPrivilegeDiagnostic(): String

    // Init Boot Sequence & Service Supervisor (Tahap 7 / Option B)
    external fun nativeLoadInitFromVfs(): Int
    external fun nativeStartBootSequence(sandboxDir: String): Boolean
    external fun nativeStopBootSequence()
    external fun nativeGetBootPhase(): String
    external fun nativeGetServicesList(): Array<GsiServiceInfo>?
    external fun nativeControlService(name: String, action: Int): Boolean

    // Synthetic Graphics & Gralloc Bridge (Tahap 8 / Option C)
    external fun nativeInitGralloc(sandboxDir: String): Boolean
    external fun nativeStartBootAnimation(enable: Boolean)
    external fun nativeIsBootAnimationActive(): Boolean
    external fun nativeGetGraphicsStats(): String

    // Virtual Network & DNS Bridge (Pilar 1)
    external fun nativeInitNetwork(sandboxDir: String, primaryDns: String = "8.8.8.8", secondaryDns: String = "1.1.1.1"): Boolean
    external fun nativeShutdownNetwork()
    external fun nativeTestNetworkConnectivity(host: String = "google.com", port: Int = 443): String
    external fun nativeGetNetworkStats(): String

    // Real-Time Logcat Broker & Tombstone Crash Analyzer (Pilar 2)
    external fun nativeInitLogcatBroker(sandboxDir: String): Boolean
    external fun nativeShutdownLogcatBroker()
    external fun nativeGetRecentLogs(bufferId: Int = -1, minPriority: Int = 2, maxLines: Int = 200): String
    external fun nativeGetTombstoneReport(): String
    external fun nativeClearLogs()
    external fun nativeGetLogStats(): String
    external fun nativeInjectTestCrash(reason: String = "Diagnostic simulation"): String

    // Virtual Audio HAL & PCM AudioTrack (Pilar 3)
    external fun nativeInitAudio(sandboxDir: String): Boolean
    external fun nativeShutdownAudio()
    external fun nativeReadAudioPcm(buffer: ShortArray): Int
    external fun nativeWriteAudioPcm(buffer: ShortArray): Int
    external fun nativePlayChime(type: Int = 1): Boolean
    external fun nativeGetAudioStats(): String

    // Display & Surface Pipeline
    external fun nativeSetSurface(surface: Surface)
    external fun nativeClearSurface()
    external fun nativeStartTestRender()
    external fun nativeStopTestRender()
    external fun nativeGetRenderedFrames(): Int
}
