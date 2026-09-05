package com.gsi.runtime

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.util.Log

object GsiBatteryManager {
    private const val TAG = "GsiBatteryManager"

    data class BatteryState(
        val capacity: Int = 100,
        val status: String = "Charging",
        val health: String = "Good",
        val voltageMv: Int = 4200,
        val tempTenthsC: Int = 300,
        val isPluggedAc: Boolean = true,
        val isPluggedUsb: Boolean = false,
        val isAutoSync: Boolean = true
    ) {
        val displayTemp: String get() = String.format("%.1f°C", tempTenthsC / 10.0)
        val displayVoltage: String get() = String.format("%.2fV", voltageMv / 1000.0)
    }

    var currentState: BatteryState = BatteryState()
        private set

    var onStateChangedListener: ((BatteryState) -> Unit)? = null

    private var receiverRegistered = false
    private var isAutoSync = true

    private val batteryReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent == null || !isAutoSync) return

            val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
            val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
            val capacity = if (level >= 0 && scale > 0) (level * 100) / scale else 100

            val statusInt = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
            val status = when (statusInt) {
                BatteryManager.BATTERY_STATUS_CHARGING -> "Charging"
                BatteryManager.BATTERY_STATUS_DISCHARGING -> "Discharging"
                BatteryManager.BATTERY_STATUS_FULL -> "Full"
                BatteryManager.BATTERY_STATUS_NOT_CHARGING -> "Not charging"
                else -> "Discharging"
            }

            val healthInt = intent.getIntExtra(BatteryManager.EXTRA_HEALTH, -1)
            val health = when (healthInt) {
                BatteryManager.BATTERY_HEALTH_GOOD -> "Good"
                BatteryManager.BATTERY_HEALTH_OVERHEAT -> "Overheat"
                BatteryManager.BATTERY_HEALTH_DEAD -> "Dead"
                BatteryManager.BATTERY_HEALTH_OVER_VOLTAGE -> "Over voltage"
                BatteryManager.BATTERY_HEALTH_COLD -> "Cold"
                else -> "Good"
            }

            val voltageMv = intent.getIntExtra(BatteryManager.EXTRA_VOLTAGE, 4200)
            val tempTenthsC = intent.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 300)

            val plugged = intent.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0)
            val isPluggedAc = (plugged and BatteryManager.BATTERY_PLUGGED_AC) != 0
            val isPluggedUsb = (plugged and BatteryManager.BATTERY_PLUGGED_USB) != 0

            val updated = BatteryState(
                capacity = capacity,
                status = status,
                health = health,
                voltageMv = voltageMv,
                tempTenthsC = tempTenthsC,
                isPluggedAc = isPluggedAc,
                isPluggedUsb = isPluggedUsb,
                isAutoSync = true
            )

            currentState = updated
            GsiEngine.nativeUpdateBattery(
                capacity, status, health, voltageMv, tempTenthsC, isPluggedAc, isPluggedUsb
            )
            onStateChangedListener?.invoke(updated)
            Log.d(TAG, "Host battery synced: $capacity% ($status, $voltageMv mV, ${tempTenthsC / 10.0}°C)")
        }
    }

    fun start(context: Context) {
        if (receiverRegistered) return
        val filter = IntentFilter(Intent.ACTION_BATTERY_CHANGED)
        val stickyIntent = context.registerReceiver(batteryReceiver, filter)
        receiverRegistered = true

        // Trigger immediate read from sticky intent
        stickyIntent?.let { batteryReceiver.onReceive(context, it) }
        Log.i(TAG, "GsiBatteryManager active: listening for host battery changes")
    }

    fun stop(context: Context) {
        if (!receiverRegistered) return
        try {
            context.unregisterReceiver(batteryReceiver)
        } catch (e: Exception) {
            Log.w(TAG, "Error unregistering battery receiver: ${e.message}")
        }
        receiverRegistered = false
    }

    fun setAutoSync(enable: Boolean, context: Context? = null) {
        isAutoSync = enable
        if (enable && context != null) {
            val filter = IntentFilter(Intent.ACTION_BATTERY_CHANGED)
            val stickyIntent = context.registerReceiver(null, filter)
            stickyIntent?.let { batteryReceiver.onReceive(context, it) }
        }
    }

    fun spoofBattery(
        capacity: Int,
        status: String = "Charging",
        health: String = "Good",
        voltageMv: Int = 4200,
        tempTenthsC: Int = 300,
        isPluggedAc: Boolean = true,
        isPluggedUsb: Boolean = false
    ) {
        isAutoSync = false
        val updated = BatteryState(
            capacity = capacity,
            status = status,
            health = health,
            voltageMv = voltageMv,
            tempTenthsC = tempTenthsC,
            isPluggedAc = isPluggedAc,
            isPluggedUsb = isPluggedUsb,
            isAutoSync = false
        )
        currentState = updated
        GsiEngine.nativeUpdateBattery(
            capacity, status, health, voltageMv, tempTenthsC, isPluggedAc, isPluggedUsb
        )
        onStateChangedListener?.invoke(updated)
        Log.i(TAG, "Spoofed battery state: $capacity% ($status, $health, $voltageMv mV)")
    }
}
