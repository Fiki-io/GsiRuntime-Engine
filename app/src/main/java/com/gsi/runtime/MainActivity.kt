package com.gsi.runtime

import android.annotation.SuppressLint
import android.app.ActivityManager
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import android.text.method.ScrollingMovementMethod
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.gsi.runtime.databinding.ActivityMainBinding
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class MainActivity : AppCompatActivity(), SurfaceHolder.Callback {

    private lateinit var binding: ActivityMainBinding
    private val handler = Handler(Looper.getMainLooper())
    private var isRendering = false
    private var lastFrameCount = 0
    private var lastFpsUpdateTime = 0L

    private var activePfd: ParcelFileDescriptor? = null
    private var currentBuildInfo: GsiBuildInfo? = null
    private lateinit var sandboxDirPath: String
    private var isVfbMode = false

    private val selectImageLauncher = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri != null) {
            handleImageSelected(uri)
        }
    }

    private val fpsUpdateRunnable = object : Runnable {
        override fun run() {
            if (isRendering) {
                val currentFrames = GsiEngine.nativeGetRenderedFrames()
                val now = System.currentTimeMillis()
                val deltaMs = now - lastFpsUpdateTime
                if (deltaMs > 0) {
                    val deltaFrames = currentFrames - lastFrameCount
                    val fps = (deltaFrames * 1000f) / deltaMs
                    binding.tvFpsCounter.text = String.format(
                        Locale.US,
                        "FPS: %.1f | Frames: %d",
                        fps,
                        currentFrames
                    )
                }
                lastFrameCount = currentFrames
                lastFpsUpdateTime = now
                handler.postDelayed(this, 500)
            }
        }
    }

    // Polling output from C++ ProcessSpawner
    private val processOutputPollRunnable = object : Runnable {
        override fun run() {
            val output = GsiEngine.nativePollProcessOutput()
            if (output.isNotEmpty()) {
                appendTerminalOutput(output)
            }

            val isRunning = GsiEngine.nativeIsProcessActive()
            if (isRunning) {
                binding.tvProcessStatus.text = "RUNNING"
                binding.tvProcessStatus.setTextColor(getColor(R.color.accent_yellow))
                handler.postDelayed(this, 150)
            } else {
                binding.tvProcessStatus.text = "IDLE"
                binding.tvProcessStatus.setTextColor(getColor(R.color.text_secondary))
            }
        }
    }

    // Polling Android Boot Phase from C++ BootManager
    private val bootPhaseRunnable = object : Runnable {
        override fun run() {
            val phase = GsiEngine.nativeGetBootPhase()
            binding.tvBootPhaseBadge.text = phase
            when (phase) {
                "COMPLETED" -> {
                    binding.tvBootPhaseBadge.setTextColor(getColor(R.color.accent_green))
                    binding.btnBootGsi.isEnabled = false
                    binding.btnStopBoot.isEnabled = true
                    logToConsole("Android GSI Boot Completed: sys.boot_completed=1")
                }
                "IDLE" -> {
                    binding.tvBootPhaseBadge.setTextColor(getColor(R.color.text_secondary))
                    binding.btnBootGsi.isEnabled = true
                    binding.btnStopBoot.isEnabled = false
                }
                else -> {
                    binding.tvBootPhaseBadge.setTextColor(getColor(R.color.accent_yellow))
                    binding.btnBootGsi.isEnabled = false
                    binding.btnStopBoot.isEnabled = true
                    handler.postDelayed(this, 300)
                }
            }
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.tvConsoleLogs.movementMethod = ScrollingMovementMethod()

        logToConsole("Initializing GSI Runtime Engine...")

        // 1. Initialize native core
        val initOk = GsiEngine.nativeInit()
        if (initOk) {
            logToConsole("Native C++ Core initialized successfully.")
            binding.tvStatusBadge.text = "CORE ONLINE"
        } else {
            logToConsole("ERROR: Failed to initialize native C++ core.")
            binding.tvStatusBadge.text = "CORE ERROR"
        }

        // 2. Display engine & system specs
        val engineInfo = GsiEngine.nativeGetEngineInfo()
        binding.tvEngineInfo.text = engineInfo
        displayHostRamSpecs()

        // 3. Setup Sandbox Directory
        val sandboxDir = File(filesDir, "sandbox")
        sandboxDirPath = sandboxDir.absolutePath
        val sandboxOk = GsiEngine.nativePrepareSandbox(sandboxDirPath)
        if (sandboxOk) {
            logToConsole("Sandbox prepared: $sandboxDirPath")

            // Tahap 4: Initialize Virtual Framebuffer & Touch Input
            val vfbOk = GsiEngine.nativeInitVfb(720, 1280, sandboxDirPath)
            if (vfbOk) {
                logToConsole("Virtual Framebuffer (720x1280) & Touch Input Pipe initialized!")
            }

            // Tahap 5: Deploy In-Process Hook Library for LD_PRELOAD
            val hookDeployed = GsiEngine.nativeDeployHookLibrary(applicationInfo.nativeLibraryDir)
            if (hookDeployed) {
                logToConsole("In-Process Hook Library (libgsi_hook.so) deployed to sandbox/lib64 (LD_PRELOAD ready)")
            }

            // Tahap 6: Initialize User-Space Property Service
            val propOk = GsiEngine.nativeStartPropertyService(sandboxDirPath, null)
            if (propOk) {
                logToConsole("User-Space Property Service active at $sandboxDirPath/dev/socket/property_service")
            }

            // Tahap 8 / Option C: Initialize Synthetic Graphics & Gralloc Bridge
            val grallocOk = GsiEngine.nativeInitGralloc(sandboxDirPath)
            if (grallocOk) {
                logToConsole("Synthetic Graphics Bridge & Virtual Gralloc Allocator active.")
            }

            // Pilar 1: Initialize Virtual Network & DNS Bridge
            val netOk = GsiEngine.nativeInitNetwork(sandboxDirPath, "8.8.8.8", "1.1.1.1")
            if (netOk) {
                logToConsole("Virtual Network & DNS Bridge active (resolv.conf, hosts, netd daemon online)")
            }

            // Pilar 2: Initialize Real-Time Logcat Broker & Tombstone Subsystem
            val logcatOk = GsiEngine.nativeInitLogcatBroker(sandboxDirPath)
            if (logcatOk) {
                logToConsole("Real-Time Logcat Broker & Tombstone Subsystem active (logdw/logdr online)")
            }

            // Pilar 3: Initialize Virtual Audio HAL & AudioTrack Bridge
            val audioOk = GsiEngine.nativeInitAudio(sandboxDirPath)
            if (audioOk) {
                GsiAudioPlayer.start()
                logToConsole("Virtual Audio HAL (48kHz Stereo 16-bit) & AudioTrack Bridge online")
            }

            // Pilar 5: Initialize Virtual Battery & Power Management Subsystem
            val batOk = GsiEngine.nativeInitBattery(sandboxDirPath)
            if (batOk) {
                GsiBatteryManager.start(this)
                GsiBatteryManager.onStateChangedListener = { state ->
                    runOnUiThread {
                        val plugStr = if (state.isPluggedAc) " [AC]" else if (state.isPluggedUsb) " [USB]" else ""
                        val syncStr = if (state.isAutoSync) " (Host-Synced)" else " (Spoofed)"
                        binding.tvBatteryStatus.text = String.format(
                            Locale.US,
                            "Virtual Battery & Power: ONLINE (%d%% %s%s, %s, %s%s | Sysfs)",
                            state.capacity, state.status, plugStr, state.displayVoltage, state.displayTemp, syncStr
                        )
                        if (state.capacity <= 15) {
                            binding.tvBatteryStatus.setTextColor(getColor(R.color.accent_red))
                        } else if (state.capacity <= 30) {
                            binding.tvBatteryStatus.setTextColor(getColor(R.color.accent_yellow))
                        } else {
                            binding.tvBatteryStatus.setTextColor(getColor(R.color.primary_cyan))
                        }
                    }
                }
                logToConsole("Virtual Battery & Power Management Subsystem online (sysfs power_supply active)")
            }

            // Pilar 6: Initialize Virtual Sensor Subsystem & Sensors HAL Bridge
            val sensorOk = GsiEngine.nativeInitSensors(sandboxDirPath)
            if (sensorOk) {
                GsiSensorManager.start(this)
                GsiSensorManager.onStateChangedListener = { state ->
                    runOnUiThread {
                        val syncStr = if (state.isAutoSync) " (Host-Motion)" else " (Spoofed)"
                        binding.tvSensorStatus.text = String.format(
                            Locale.US,
                            "Virtual Sensors HAL: ONLINE (%s | A:%.1f,%.1f,%.1f | %s)",
                            state.displayOrientation, state.accelX, state.accelY, state.accelZ, syncStr
                        )
                    }
                }
                logToConsole("Virtual Sensor Subsystem & Sensors HAL Bridge active (IIO sysfs online)")
            }
        }

        // 4. Setup SurfaceView for ANativeWindow pipeline
        binding.surfaceView.holder.addCallback(this)

        // Tahap 4: Setup Touch Listener on SurfaceView
        binding.surfaceView.setOnTouchListener { view, event ->
            val normX = (event.x / view.width.toFloat()).coerceIn(0f, 1f)
            val normY = (event.y / view.height.toFloat()).coerceIn(0f, 1f)
            val pointerId = event.getPointerId(event.actionIndex)

            // Inject to Linux input_event subsystem
            GsiEngine.nativeSendTouchEvent(event.actionMasked, normX, normY, pointerId)

            // Interactive visual brush/ripple directly on VFB
            if (isVfbMode) {
                val vfbX = (normX * 720).toInt()
                val vfbY = (normY * 1280).toInt()
                val color = when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> 0xFF00E5FF.toInt() // Neon Cyan
                    MotionEvent.ACTION_MOVE -> 0xFF00E676.toInt() // Neon Green
                    MotionEvent.ACTION_UP -> 0xFFFFD600.toInt()   // Yellow Spark
                    else -> 0xFF00E5FF.toInt()
                }
                GsiEngine.nativeDrawTouchFeedback(vfbX, vfbY, color, 14)
            }
            true
        }

        // Tahap 8 / Option C: 3-Mode Display Pipeline Switcher
        binding.btnModeBootSplash.setOnClickListener {
            if (!isRendering) startDisplayTest()
            isVfbMode = true
            GsiEngine.nativeSetVfbMode(true)
            GsiEngine.nativeStartBootAnimation(true)
            updateDisplayModeUi("BOOT_SPLASH")
            logToConsole("Display Pipeline: Android 60 FPS Boot Splash Compositor ACTIVE")
        }

        binding.btnModeVfb.setOnClickListener {
            if (!isRendering) startDisplayTest()
            isVfbMode = true
            GsiEngine.nativeStartBootAnimation(false)
            GsiEngine.nativeSetVfbMode(true)
            updateDisplayModeUi("VFB")
            logToConsole("Display Pipeline: VFB Guest Touch & Interactive Canvas ACTIVE")
        }

        binding.btnModeGradient.setOnClickListener {
            if (!isRendering) startDisplayTest()
            isVfbMode = false
            GsiEngine.nativeStartBootAnimation(false)
            GsiEngine.nativeSetVfbMode(false)
            updateDisplayModeUi("GRADIENT")
            logToConsole("Display Pipeline: Hardware Test Gradient ACTIVE")
        }

        binding.btnClearCanvas.setOnClickListener {
            GsiEngine.nativeClearVfb(0xFF141721.toInt())
            logToConsole("Virtual Framebuffer screen cleared.")
        }

        // Pilar 4: Virtual Hardware Navigation Bar
        binding.btnNavBack.setOnClickListener {
            injectHardwareKey(GsiEngine.KEY_BACK, "KEY_BACK (158)")
        }
        binding.btnNavHome.setOnClickListener {
            injectHardwareKey(GsiEngine.KEY_HOME, "KEY_HOME (172)")
        }
        binding.btnNavRecents.setOnClickListener {
            injectHardwareKey(GsiEngine.KEY_RECENTS, "KEY_RECENTS (580)")
        }
        binding.btnNavPower.setOnClickListener {
            injectHardwareKey(GsiEngine.KEY_POWER, "KEY_POWER (116)")
        }
        binding.btnNavVolUp.setOnClickListener {
            injectHardwareKey(GsiEngine.KEY_VOLUME_UP, "KEY_VOLUME_UP (115)")
        }
        binding.btnNavVolDown.setOnClickListener {
            injectHardwareKey(GsiEngine.KEY_VOLUME_DOWN, "KEY_VOLUME_DOWN (114)")
        }

        // 5. Setup Image Selector
        binding.btnSelectImage.setOnClickListener {
            selectImageLauncher.launch(arrayOf("*/*"))
        }

        // 6. Setup Rendering Test buttons
        binding.btnStartRender.setOnClickListener {
            startDisplayTest()
        }

        binding.btnStopRender.setOnClickListener {
            stopDisplayTest()
        }

        // 7. Tahap 2: VFS & build.prop exploration buttons
        binding.btnBrowseFilesystem.setOnClickListener {
            showFilesystemBrowserDialog("/")
        }

        binding.btnViewBuildProp.setOnClickListener {
            currentBuildInfo?.let { showBuildPropDialog(it) }
        }

        // 8. Tahap 7 / Option B: Init Boot Sequence & Services Controls
        binding.btnBootGsi.setOnClickListener {
            logToConsole("Starting Android GSI Boot Sequence...")
            val started = GsiEngine.nativeStartBootSequence(sandboxDirPath)
            if (started) {
                handler.removeCallbacks(bootPhaseRunnable)
                handler.post(bootPhaseRunnable)
            } else {
                logToConsole("Error: Could not start boot sequence.")
            }
        }

        binding.btnStopBoot.setOnClickListener {
            GsiEngine.nativeStopBootSequence()
            logToConsole("Android Boot Sequence stopped by user.")
            handler.removeCallbacks(bootPhaseRunnable)
            binding.tvBootPhaseBadge.text = "IDLE"
            binding.tvBootPhaseBadge.setTextColor(getColor(R.color.text_secondary))
            binding.btnBootGsi.isEnabled = true
            binding.btnStopBoot.isEnabled = false
        }

        binding.btnManageServices.setOnClickListener {
            showServicesManagerDialog()
        }

        // 9. Tahap 3: Interactive Sandbox Terminal
        binding.btnRunCommand.setOnClickListener {
            val cmd = binding.etCommandInput.text.toString().trim()
            if (cmd.isNotEmpty()) {
                executeSandboxCommand(cmd)
                binding.etCommandInput.setText("")
            }
        }

        binding.etCommandInput.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_SEND) {
                binding.btnRunCommand.performClick()
                true
            } else {
                false
            }
        }

        binding.btnKillProcess.setOnClickListener {
            GsiEngine.nativeKillProcess()
            logToConsole("Process terminated by user.")
        }

        // Quick Command Chips
        binding.chipBootGsi.setOnClickListener {
            binding.btnBootGsi.performClick()
        }
        binding.chipListServices.setOnClickListener {
            binding.btnManageServices.performClick()
        }
        binding.chipTestBootSplash.setOnClickListener {
            binding.btnModeBootSplash.performClick()
        }
        binding.chipGrallocStats.setOnClickListener {
            val stats = GsiEngine.nativeGetGraphicsStats()
            appendTerminalOutput("\n[Gralloc Stats] $stats\n")
        }
        binding.chipTestNetwork.setOnClickListener {
            appendTerminalOutput("\nTesting DNS resolution & TCP handshake to google.com:443...\n")
            Thread {
                val report = GsiEngine.nativeTestNetworkConnectivity("google.com", 443)
                runOnUiThread {
                    appendTerminalOutput("\n$report\n")
                }
            }.start()
        }
        binding.chipResolvConf.setOnClickListener {
            executeSandboxCommand("cat /etc/resolv.conf")
        }
        binding.chipPingDns.setOnClickListener {
            executeSandboxCommand("ping -c 3 8.8.8.8")
        }
        binding.chipLogcat.setOnClickListener {
            showLogcatViewerDialog()
        }
        binding.chipTombstones.setOnClickListener {
            showTombstoneDialog()
        }
        binding.chipSimulateCrash.setOnClickListener {
            val report = GsiEngine.nativeInjectTestCrash("Diagnostic simulation: SIGSEGV SEGV_MAPERR (libsurfaceflinger.so)")
            appendTerminalOutput("\n[CRASH INTERCEPTED & TOMBSTONE GENERATED]\n$report\n")
        }
        binding.chipPlayChime.setOnClickListener {
            val chimeOk = GsiAudioPlayer.playChime(1)
            if (chimeOk) {
                appendTerminalOutput("\n[AUDIO HAL] Synthesizing & playing Android Boot Chime (C-Maj-7th)...\n")
            } else {
                appendTerminalOutput("\n[AUDIO HAL] Audio pipeline error\n")
            }
        }
        binding.chipAudioStats.setOnClickListener {
            val stats = GsiEngine.nativeGetAudioStats()
            appendTerminalOutput("\n[Audio Stats] $stats\n")
        }
        binding.chipKeyBack.setOnClickListener { binding.btnNavBack.performClick() }
        binding.chipKeyHome.setOnClickListener { binding.btnNavHome.performClick() }
        binding.chipKeyRecents.setOnClickListener { binding.btnNavRecents.performClick() }
        binding.chipKeyPower.setOnClickListener { binding.btnNavPower.performClick() }
        binding.chipKeyVolUp.setOnClickListener { binding.btnNavVolUp.performClick() }
        binding.chipKeyVolDown.setOnClickListener { binding.btnNavVolDown.performClick() }
        binding.chipInputStats.setOnClickListener {
            val stats = GsiEngine.nativeGetInputStats()
            appendTerminalOutput("\n[Input Subsystem Stats] $stats\n")
        }
        binding.chipBatSync.setOnClickListener {
            GsiBatteryManager.setAutoSync(true, this)
            val state = GsiBatteryManager.currentState
            appendTerminalOutput("\n[Battery Sync] Synced with Host Android: ${state.capacity}% (${state.status}, ${state.displayVoltage}, ${state.displayTemp})\n")
        }
        binding.chipBat100.setOnClickListener {
            GsiBatteryManager.spoofBattery(100, "Full", "Good", 4200, 280, isPluggedAc = true, isPluggedUsb = false)
            appendTerminalOutput("\n[Battery Spoof] Set to 100% Full (AC Plugged, 4.20V, 28.0°C)\n")
        }
        binding.chipBat50.setOnClickListener {
            GsiBatteryManager.spoofBattery(50, "Discharging", "Good", 3850, 310, isPluggedAc = false, isPluggedUsb = false)
            appendTerminalOutput("\n[Battery Spoof] Set to 50% Discharging (3.85V, 31.0°C)\n")
        }
        binding.chipBat15.setOnClickListener {
            GsiBatteryManager.spoofBattery(15, "Discharging", "Good", 3600, 330, isPluggedAc = false, isPluggedUsb = false)
            appendTerminalOutput("\n[Battery Spoof] Set to 15% Low Battery (3.60V, 33.0°C)\n")
        }
        binding.chipBatPlug.setOnClickListener {
            val curr = GsiBatteryManager.currentState
            val newPlug = !curr.isPluggedAc
            val newStatus = if (newPlug) "Charging" else "Discharging"
            GsiBatteryManager.spoofBattery(curr.capacity, newStatus, curr.health, curr.voltageMv, curr.tempTenthsC, isPluggedAc = newPlug, isPluggedUsb = false)
            appendTerminalOutput("\n[Battery Charger] AC Charger toggled to: ${if (newPlug) "CONNECTED (Charging)" else "DISCONNECTED (Discharging)"}\n")
        }
        binding.chipBatStats.setOnClickListener {
            val stats = GsiEngine.nativeGetBatteryStats()
            appendTerminalOutput("\n$stats\n")
        }
        binding.chipSensorSync.setOnClickListener {
            GsiSensorManager.setAutoSync(true, this)
            appendTerminalOutput("\n[Sensor Sync] Real-time host motion synced with GSI runtime\n")
        }
        binding.chipOrientPortrait.setOnClickListener {
            GsiSensorManager.simulateOrientation(0)
            appendTerminalOutput("\n[Sensor Spoof] Orientation set to Portrait (Accel: 0, 9.8, 0)\n")
        }
        binding.chipOrientLandscape.setOnClickListener {
            GsiSensorManager.simulateOrientation(1)
            appendTerminalOutput("\n[Sensor Spoof] Orientation set to Landscape Left (Accel: 9.8, 0, 0)\n")
        }
        binding.chipShakeDevice.setOnClickListener {
            GsiSensorManager.simulateShake()
            appendTerminalOutput("\n[Sensor Spoof] Injected shake motion impulse (24.5 m/s² shockwave)!\n")
        }
        binding.chipProxNear.setOnClickListener {
            val currProx = GsiSensorManager.currentState.proximityCm
            val isNear = currProx < 3.0f
            GsiSensorManager.simulateProximity(!isNear)
            appendTerminalOutput("\n[Sensor Spoof] Proximity toggled to: ${if (!isNear) "NEAR (0.0 cm)" else "FAR (5.0 cm)"}\n")
        }
        binding.chipSensorStats.setOnClickListener {
            val stats = GsiEngine.nativeGetSensorStats()
            appendTerminalOutput("\n$stats\n")
        }
        binding.chipTestBinder.setOnClickListener {
            val report = GsiEngine.nativeRunBinderDiagnostic()
            appendTerminalOutput("\n" + report + "\n")
        }
        binding.chipTestPrivileges.setOnClickListener {
            val report = GsiEngine.nativeRunPrivilegeDiagnostic()
            appendTerminalOutput("\n" + report + "\n")
        }
        binding.chipListProps.setOnClickListener {
            val allProps = GsiEngine.nativeGetAllProperties()
            if (allProps.isNotBlank()) {
                showTextContentDialog("Android System Properties (${GsiEngine.nativeGetPropertyCount()})", allProps)
            } else {
                Toast.makeText(this, "No properties loaded yet", Toast.LENGTH_SHORT).show()
            }
        }
        binding.chipSetprop.setOnClickListener {
            executeSandboxCommand("setprop test.gsi.boot 1; getprop test.gsi.boot")
        }
        binding.chipToybox.setOnClickListener { executeSandboxCommand("toybox id") }
        binding.chipUname.setOnClickListener { executeSandboxCommand("uname -a") }
        binding.chipLs.setOnClickListener { executeSandboxCommand("ls -la /system/bin") }
        binding.chipEnv.setOnClickListener { executeSandboxCommand("echo PATH=\$PATH; echo ANDROID_ROOT=\$ANDROID_ROOT; echo LD_LIBRARY_PATH=\$LD_LIBRARY_PATH") }
        binding.chipGetprop.setOnClickListener { executeSandboxCommand("getprop ro.build.version.release") }

        // Start Foreground Service to keep runtime alive
        GsiForegroundService.startService(this)
    }

    private fun injectHardwareKey(keyCode: Int, keyName: String) {
        GsiEngine.nativeInjectKeyClick(keyCode)
        logToConsole("Hardware Key Injected: $keyName")
        Toast.makeText(this, "Injected $keyName", Toast.LENGTH_SHORT).show()
    }

    private fun executeSandboxCommand(command: String) {

        appendTerminalOutput("\n$ $command\n")
        val spawned = GsiEngine.nativeExecuteCommand(command, sandboxDirPath)
        if (spawned) {
            binding.tvProcessStatus.text = "RUNNING"
            binding.tvProcessStatus.setTextColor(getColor(R.color.accent_yellow))
            handler.removeCallbacks(processOutputPollRunnable)
            handler.post(processOutputPollRunnable)
        } else {
            appendTerminalOutput("Error: Failed to spawn process for command: $command\n")
        }
    }

    private fun displayHostRamSpecs() {
        val actManager = getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val memInfo = ActivityManager.MemoryInfo()
        actManager.getMemoryInfo(memInfo)

        val totalGb = memInfo.totalMem / (1024.0 * 1024.0 * 1024.0)
        val availGb = memInfo.availMem / (1024.0 * 1024.0 * 1024.0)

        binding.tvHostRam.text = String.format(
            Locale.US,
            "Host RAM: %.2f GB (Free: %.2f GB) | LowRAM: %b | ABI: %s",
            totalGb,
            availGb,
            memInfo.lowMemory,
            Build.SUPPORTED_ABIS.firstOrNull() ?: "unknown"
        )
    }

    private fun handleImageSelected(uri: Uri) {
        var fileName = "unknown.img"
        contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (nameIndex != -1 && cursor.moveToFirst()) {
                fileName = cursor.getString(nameIndex)
            }
        }

        binding.tvSelectedImageName.text = "Loaded: $fileName"
        logToConsole("Selected GSI file: $fileName. Reading headers via JNI...")

        try {
            activePfd?.close()
            activePfd = contentResolver.openFileDescriptor(uri, "r")

            val pfd = activePfd
            if (pfd == null) {
                logToConsole("Failed to open file descriptor for URI")
                return
            }

            val fd = pfd.fd
            val info = GsiEngine.nativeVerifyImageFd(fd)

            if (info != null && info.isValid) {
                binding.tvImageVerificationResult.text = String.format(
                    Locale.US,
                    "Format: %s | Size: %d MB (%d blocks)\nVolume: %s",
                    info.formatName,
                    info.uncompressedSizeMb,
                    info.totalBlocks,
                    if (info.volumeName.isNotBlank()) info.volumeName else "[Standard]"
                )
                binding.tvImageVerificationResult.setTextColor(getColor(R.color.accent_green))
                logToConsole("Image verified: ${info.description}")

                logToConsole("Mounting user-space VFS (EXT4 / Sparse)...")
                val fsMounted = GsiEngine.nativeOpenFilesystem(fd)
                if (fsMounted) {
                    logToConsole("User-space VFS mounted successfully!")
                    val buildProp = GsiEngine.nativeGetBuildInfo()
                    currentBuildInfo = buildProp

                    if (buildProp != null && buildProp.osVersion.isNotBlank()) {
                        binding.layoutRomMetadata.visibility = View.VISIBLE
                        binding.tvGsiRomTitle.text = String.format(
                            Locale.US,
                            "Android %s (API %s) | %s",
                            buildProp.osVersion,
                            buildProp.sdkVersion,
                            if (buildProp.model.isNotBlank()) buildProp.model else "Generic Treble GSI"
                        )
                        binding.tvGsiRomDetails.text = String.format(
                            Locale.US,
                            "Patch: %s | Build: %s | Treble: %b",
                            buildProp.securityPatch,
                            buildProp.buildId,
                            buildProp.isTrebleEnabled
                        )
                        logToConsole("Detected GSI ROM: Android ${buildProp.osVersion} (Build ${buildProp.buildId})")
                    } else {
                        binding.layoutRomMetadata.visibility = View.VISIBLE
                        binding.tvGsiRomTitle.text = "User-Space EXT4 Filesystem Active"
                        binding.tvGsiRomDetails.text = "Filesystem parsed successfully (build.prop awaiting inspection)"
                        logToConsole("Filesystem ready for exploration.")
                    }

                    // Extract core binaries to sandbox
                    logToConsole("Extracting guest binaries from VFS to sandbox...")
                    val toyboxOk = GsiEngine.nativeExtractBinaryFromVfs("/system/bin/toybox", "toybox")
                    val shOk = GsiEngine.nativeExtractBinaryFromVfs("/system/bin/sh", "sh")
                    if (toyboxOk || shOk) {
                        logToConsole("Guest binaries provisioned to sandbox (chmod 0755). Ready for execution!")
                    }

                    // Synchronize User-Space Property Service with GSI build.prop
                    if (buildProp != null && buildProp.rawContent.isNotBlank()) {
                        GsiEngine.nativeStartPropertyService(sandboxDirPath, buildProp.rawContent)
                        val propCount = GsiEngine.nativeGetPropertyCount()
                        logToConsole("Property Service synchronized: $propCount GSI properties active!")
                    }

                    // Tahap 7 / Option B: Parse Android init.rc scripts and services from VFS
                    val svcCount = GsiEngine.nativeLoadInitFromVfs()
                    binding.tvBootSummary.text = "Services: $svcCount loaded from VFS | Triggers: early-init, init, boot"
                    binding.btnManageServices.text = "Services ($svcCount)"
                    logToConsole("Init Engine: Loaded $svcCount Android services from GSI VFS!")

                    Toast.makeText(this, "GSI Filesystem & Sandbox Ready!", Toast.LENGTH_SHORT).show()
                } else {
                    binding.layoutRomMetadata.visibility = View.GONE
                    logToConsole("Could not mount EXT4 filesystem in memory.")
                }
            } else {
                binding.layoutRomMetadata.visibility = View.GONE
                binding.tvImageVerificationResult.text = info?.description ?: "Invalid or unrecognized image format."
                binding.tvImageVerificationResult.setTextColor(getColor(R.color.accent_red))
                logToConsole("Image verification failed: ${info?.description}")
            }
        } catch (e: Exception) {
            logToConsole("Exception opening image file: ${e.message}")
            binding.tvImageVerificationResult.text = "Error reading file: ${e.message}"
            binding.tvImageVerificationResult.setTextColor(getColor(R.color.accent_red))
        }
    }

    private fun showFilesystemBrowserDialog(path: String) {
        val entries = GsiEngine.nativeListDirectory(path)
        if (entries == null || entries.isEmpty()) {
            Toast.makeText(this, "Empty directory or cannot read: $path", Toast.LENGTH_SHORT).show()
            return
        }

        val itemNames = mutableListOf<String>()
        if (path != "/") {
            itemNames.add(".. [Go to Parent Folder]")
        }
        for (e in entries) {
            val prefix = if (e.isDirectory) "📁 " else if (e.isSymlink) "🔗 " else "📄 "
            itemNames.add("$prefix${e.name}  (${e.formattedSize})")
        }

        AlertDialog.Builder(this)
            .setTitle("VFS: $path")
            .setItems(itemNames.toTypedArray()) { _, which ->
                var selectedIndex = which
                if (path != "/") {
                    if (which == 0) {
                        val parent = path.substringBeforeLast('/', "")
                        showFilesystemBrowserDialog(if (parent.isEmpty()) "/" else parent)
                        return@setItems
                    }
                    selectedIndex -= 1
                }

                val selected = entries[selectedIndex]
                if (selected.isDirectory) {
                    showFilesystemBrowserDialog(selected.path)
                } else {
                    if (selected.name.endsWith(".prop") || selected.name.endsWith(".rc") ||
                        selected.name.endsWith(".xml") || selected.name.endsWith(".txt") ||
                        selected.name.endsWith(".sh") || selected.size < 64 * 1024) {
                        val text = GsiEngine.nativeReadFileText(selected.path)
                        if (text != null) {
                            showTextContentDialog(selected.name, text)
                        } else {
                            Toast.makeText(this, "Binary file or cannot read text", Toast.LENGTH_SHORT).show()
                        }
                    } else {
                        Toast.makeText(this, "${selected.name} (${selected.formattedSize})", Toast.LENGTH_SHORT).show()
                    }
                }
            }
            .setNegativeButton("Close", null)
            .show()
    }

    private fun showBuildPropDialog(info: GsiBuildInfo) {
        showTextContentDialog("build.prop Inspection", info.rawContent)
    }

    private fun showLogcatViewerDialog() {
        val logs = GsiEngine.nativeGetRecentLogs(-1, 2, 400)
        val stats = GsiEngine.nativeGetLogStats()
        showTextContentDialog("GSI Real-Time Logcat ($stats)", if (logs.isNotBlank()) logs else "No logs recorded in ring buffer yet.")
    }

    private fun showTombstoneDialog() {
        val tombstone = GsiEngine.nativeGetTombstoneReport()
        showTextContentDialog("Native Crash Tombstones (debuggerd)", tombstone)
    }

    private fun showTextContentDialog(title: String, content: String) {
        val textView = TextView(this).apply {
            text = content
            setPadding(32, 24, 32, 24)
            typeface = android.graphics.Typeface.MONOSPACE
            setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, 11f)
            setTextColor(getColor(R.color.terminal_green))
        }

        val scrollView = ScrollView(this).apply {
            addView(textView)
            setBackgroundColor(getColor(R.color.terminal_bg))
        }

        AlertDialog.Builder(this)
            .setTitle(title)
            .setView(scrollView)
            .setPositiveButton("Close", null)
            .show()
    }

    private fun startDisplayTest() {
        if (isRendering) return
        isRendering = true
        lastFpsUpdateTime = System.currentTimeMillis()
        lastFrameCount = 0

        GsiEngine.nativeStartTestRender()
        handler.post(fpsUpdateRunnable)

        binding.btnStartRender.isEnabled = false
        binding.btnStopRender.isEnabled = true
        binding.tvStatusBadge.text = "GPU RENDERING"
        logToConsole("Started native ANativeWindow hardware render loop (~60 FPS)")
    }

    private fun stopDisplayTest() {
        if (!isRendering) return
        isRendering = false
        handler.removeCallbacks(fpsUpdateRunnable)

        GsiEngine.nativeStopTestRender()

        binding.btnStartRender.isEnabled = true
        binding.btnStopRender.isEnabled = false
        binding.tvStatusBadge.text = "CORE READY"
        logToConsole("Stopped native display pipeline.")
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        logToConsole("SurfaceHolder created -> Binding to C++ ANativeWindow")
        GsiEngine.nativeSetSurface(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        logToConsole("Surface changed: ${width}x${height}, format: $format")
        GsiEngine.nativeSetSurface(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        logToConsole("Surface destroyed -> Releasing C++ ANativeWindow")
        if (isRendering) {
            stopDisplayTest()
        }
        GsiEngine.nativeClearSurface()
    }

    private fun logToConsole(message: String) {
        val time = SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(Date())
        val formatted = "[$time] $message\n"
        appendTerminalOutput(formatted)
    }

    private fun appendTerminalOutput(text: String) {
        binding.tvConsoleLogs.append(text)
        val scrollAmount = binding.tvConsoleLogs.layout?.let {
            it.getLineTop(binding.tvConsoleLogs.lineCount) - binding.tvConsoleLogs.height
        } ?: 0
        if (scrollAmount > 0) {
            binding.tvConsoleLogs.scrollTo(0, scrollAmount)
        }
    }

    private fun showServicesManagerDialog() {
        val services = GsiEngine.nativeGetServicesList()
        if (services == null || services.isEmpty()) {
            Toast.makeText(this, "No services loaded yet. Mount GSI first.", Toast.LENGTH_SHORT).show()
            return
        }

        val items = services.map { s ->
            val statusIcon = if (s.isRunning) "🟢 [RUNNING]" else "⚪ [STOPPED]"
            val pidStr = if (s.pid > 0) " (PID: ${s.pid})" else ""
            "$statusIcon ${s.name}$pidStr\n   ${s.displayDetails}"
        }.toTypedArray()

        AlertDialog.Builder(this)
            .setTitle("Android Services Manager (${services.size})")
            .setItems(items) { _, which ->
                val selected = services[which]
                showServiceControlDialog(selected)
            }
            .setPositiveButton("Close", null)
            .show()
    }

    private fun showServiceControlDialog(service: GsiServiceInfo) {
        val options = if (service.isRunning) {
            arrayOf("Stop Service", "Restart Service")
        } else {
            arrayOf("Start Service")
        }

        AlertDialog.Builder(this)
            .setTitle("Service: ${service.name}")
            .setItems(options) { _, which ->
                val choice = options[which]
                when (choice) {
                    "Start Service" -> {
                        GsiEngine.nativeControlService(service.name, 1)
                        logToConsole("Requested start: ${service.name}")
                    }
                    "Stop Service" -> {
                        GsiEngine.nativeControlService(service.name, 2)
                        logToConsole("Requested stop: ${service.name}")
                    }
                    "Restart Service" -> {
                        GsiEngine.nativeControlService(service.name, 3)
                        logToConsole("Requested restart: ${service.name}")
                    }
                }
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun updateDisplayModeUi(activeMode: String) {
        when (activeMode) {
            "BOOT_SPLASH" -> {
                binding.btnModeBootSplash.setTextColor(getColor(R.color.bg_dark))
                binding.btnModeBootSplash.setBackgroundColor(getColor(R.color.accent_green))
                binding.btnModeVfb.setTextColor(getColor(R.color.primary_cyan))
                binding.btnModeVfb.setBackgroundColor(getColor(R.color.surface_border))
                binding.btnModeGradient.setTextColor(getColor(R.color.text_secondary))
                binding.btnModeGradient.setBackgroundColor(getColor(R.color.surface_border))
            }
            "VFB" -> {
                binding.btnModeBootSplash.setTextColor(getColor(R.color.text_secondary))
                binding.btnModeBootSplash.setBackgroundColor(getColor(R.color.surface_border))
                binding.btnModeVfb.setTextColor(getColor(R.color.bg_dark))
                binding.btnModeVfb.setBackgroundColor(getColor(R.color.primary_cyan))
                binding.btnModeGradient.setTextColor(getColor(R.color.text_secondary))
                binding.btnModeGradient.setBackgroundColor(getColor(R.color.surface_border))
            }
            "GRADIENT" -> {
                binding.btnModeBootSplash.setTextColor(getColor(R.color.text_secondary))
                binding.btnModeBootSplash.setBackgroundColor(getColor(R.color.surface_border))
                binding.btnModeVfb.setTextColor(getColor(R.color.primary_cyan))
                binding.btnModeVfb.setBackgroundColor(getColor(R.color.surface_border))
                binding.btnModeGradient.setTextColor(getColor(R.color.bg_dark))
                binding.btnModeGradient.setBackgroundColor(getColor(R.color.primary_cyan))
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        stopDisplayTest()
        GsiEngine.nativeStartBootAnimation(false)
        handler.removeCallbacks(bootPhaseRunnable)
        GsiEngine.nativeStopBootSequence()
        GsiAudioPlayer.stop()
        GsiBatteryManager.stop(this)
        GsiSensorManager.stop(this)
        GsiEngine.nativeShutdownAudio()
        GsiEngine.nativeShutdownLogcatBroker()
        GsiEngine.nativeShutdownNetwork()
        GsiEngine.nativeStopPropertyService()
        GsiEngine.nativeKillProcess()
        GsiEngine.nativeCloseFilesystem()
        activePfd?.close()
        activePfd = null
        GsiForegroundService.stopService(this)
    }
}
