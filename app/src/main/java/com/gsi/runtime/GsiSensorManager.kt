package com.gsi.runtime

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Handler
import android.os.Looper
import android.util.Log

object GsiSensorManager : SensorEventListener {
    private const val TAG = "GsiSensorManager"

    data class SensorState(
        val accelX: Float = 0.0f,
        val accelY: Float = 9.81f,
        val accelZ: Float = 0.0f,
        val gyroX: Float = 0.0f,
        val gyroY: Float = 0.0f,
        val gyroZ: Float = 0.0f,
        val lightLux: Float = 300.0f,
        val proximityCm: Float = 5.0f,
        val isAutoSync: Boolean = true
    ) {
        val displayOrientation: String
            get() {
                return if (Math.abs(accelX) > Math.abs(accelY)) {
                    if (accelX > 0) "Landscape (Left)" else "Landscape (Right)"
                } else {
                    if (accelY > 0) "Portrait" else "Upside-Down"
                }
            }
    }

    var currentState: SensorState = SensorState()
        private set

    var onStateChangedListener: ((SensorState) -> Unit)? = null

    private var sensorManager: SensorManager? = null
    private var isListening = false
    private var isAutoSync = true
    private val mainHandler = Handler(Looper.getMainLooper())

    fun start(context: Context) {
        if (isListening) return
        sensorManager = context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
        val sm = sensorManager ?: return

        val accel = sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        val gyro = sm.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
        val light = sm.getDefaultSensor(Sensor.TYPE_LIGHT)
        val prox = sm.getDefaultSensor(Sensor.TYPE_PROXIMITY)

        accel?.let { sm.registerListener(this, it, SensorManager.SENSOR_DELAY_UI) }
        gyro?.let { sm.registerListener(this, it, SensorManager.SENSOR_DELAY_UI) }
        light?.let { sm.registerListener(this, it, SensorManager.SENSOR_DELAY_UI) }
        prox?.let { sm.registerListener(this, it, SensorManager.SENSOR_DELAY_UI) }

        isListening = true
        Log.i(TAG, "GsiSensorManager started: host sensors registered (Accel, Gyro, Light, Prox)")
    }

    fun stop(context: Context) {
        if (!isListening) return
        sensorManager?.unregisterListener(this)
        isListening = false
        Log.i(TAG, "GsiSensorManager stopped")
    }

    fun setAutoSync(enable: Boolean, context: Context? = null) {
        isAutoSync = enable
        if (enable && context != null && !isListening) {
            start(context)
        }
    }

    override fun onSensorChanged(event: SensorEvent?) {
        if (event == null || !isAutoSync) return

        var ax = currentState.accelX
        var ay = currentState.accelY
        var az = currentState.accelZ
        var gx = currentState.gyroX
        var gy = currentState.gyroY
        var gz = currentState.gyroZ
        var lux = currentState.lightLux
        var prox = currentState.proximityCm

        when (event.sensor.type) {
            Sensor.TYPE_ACCELEROMETER -> {
                ax = event.values[0]
                ay = event.values[1]
                az = event.values[2]
            }
            Sensor.TYPE_GYROSCOPE -> {
                gx = event.values[0]
                gy = event.values[1]
                gz = event.values[2]
            }
            Sensor.TYPE_LIGHT -> {
                lux = event.values[0]
            }
            Sensor.TYPE_PROXIMITY -> {
                prox = event.values[0]
            }
        }

        val updated = SensorState(
            accelX = ax, accelY = ay, accelZ = az,
            gyroX = gx, gyroY = gy, gyroZ = gz,
            lightLux = lux, proximityCm = prox,
            isAutoSync = true
        )
        currentState = updated

        GsiEngine.nativeUpdateSensors(ax, ay, az, gx, gy, gz, lux, prox)
        onStateChangedListener?.invoke(updated)
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    fun simulateOrientation(orientation: Int) {
        isAutoSync = false
        val (ax, ay, az) = when (orientation) {
            0 -> Triple(0.0f, 9.81f, 0.0f)     // Portrait
            1 -> Triple(9.81f, 0.0f, 0.0f)     // Landscape Left
            2 -> Triple(-9.81f, 0.0f, 0.0f)    // Landscape Right
            else -> Triple(0.0f, -9.81f, 0.0f)  // Upside down
        }

        val updated = currentState.copy(
            accelX = ax, accelY = ay, accelZ = az,
            gyroX = 0.0f, gyroY = 0.0f, gyroZ = 0.0f,
            isAutoSync = false
        )
        currentState = updated

        GsiEngine.nativeUpdateSensors(ax, ay, az, 0f, 0f, 0f, updated.lightLux, updated.proximityCm)
        onStateChangedListener?.invoke(updated)
    }

    fun simulateShake() {
        isAutoSync = false
        // Sudden violent impulse
        val jolt = currentState.copy(
            accelX = 24.5f, accelY = 18.2f, accelZ = 6.4f,
            gyroX = 3.5f, gyroY = -2.1f, gyroZ = 4.2f,
            isAutoSync = false
        )
        currentState = jolt
        GsiEngine.nativeUpdateSensors(jolt.accelX, jolt.accelY, jolt.accelZ, jolt.gyroX, jolt.gyroY, jolt.gyroZ, jolt.lightLux, jolt.proximityCm)
        onStateChangedListener?.invoke(jolt)

        // Settle back to portrait gravity after 300 ms
        mainHandler.postDelayed({
            simulateOrientation(0)
        }, 300)
    }

    fun simulateProximity(near: Boolean) {
        val dist = if (near) 0.0f else 5.0f
        val updated = currentState.copy(proximityCm = dist, isAutoSync = false)
        currentState = updated
        GsiEngine.nativeUpdateSensors(updated.accelX, updated.accelY, updated.accelZ, updated.gyroX, updated.gyroY, updated.gyroZ, updated.lightLux, dist)
        onStateChangedListener?.invoke(updated)
    }
}
