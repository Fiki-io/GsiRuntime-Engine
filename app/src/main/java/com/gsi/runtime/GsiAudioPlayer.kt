package com.gsi.runtime

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Build
import android.util.Log
import java.util.concurrent.atomic.AtomicBoolean

object GsiAudioPlayer {
    private const val TAG = "GSI-AudioPlayer"
    private const val SAMPLE_RATE = 48000
    private const val BUFFER_FRAMES = 2048 // 2048 frames = 4096 samples

    private var audioTrack: AudioTrack? = null
    private var playbackThread: Thread? = null
    private val isRunning = AtomicBoolean(false)

    fun start() {
        if (isRunning.get()) return

        try {
            val minBufferSize = AudioTrack.getMinBufferSize(
                SAMPLE_RATE,
                AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_16BIT
            )
            val bufferSize = maxOf(minBufferSize, BUFFER_FRAMES * 4)

            val track = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                AudioTrack.Builder()
                    .setAudioAttributes(
                        AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                            .build()
                    )
                    .setAudioFormat(
                        AudioFormat.Builder()
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setSampleRate(SAMPLE_RATE)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                            .build()
                    )
                    .setBufferSizeInBytes(bufferSize)
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .build()
            } else {
                @Suppress("DEPRECATION")
                AudioTrack(
                    AudioManager.STREAM_MUSIC,
                    SAMPLE_RATE,
                    AudioFormat.CHANNEL_OUT_STEREO,
                    AudioFormat.ENCODING_PCM_16BIT,
                    bufferSize,
                    AudioTrack.MODE_STREAM
                )
            }

            track.play()
            audioTrack = track
            isRunning.set(true)

            playbackThread = Thread({
                val pcmBuffer = ShortArray(BUFFER_FRAMES * 2) // Stereo = 2 samples per frame
                while (isRunning.get()) {
                    val readSamples = GsiEngine.nativeReadAudioPcm(pcmBuffer)
                    if (readSamples > 0) {
                        track.write(pcmBuffer, 0, readSamples)
                    } else {
                        try {
                            Thread.sleep(10)
                        } catch (_: InterruptedException) {
                            break
                        }
                    }
                }
            }, "GsiAudioPlaybackThread").apply {
                priority = Thread.MAX_PRIORITY
                start()
            }

            Log.i(TAG, "Virtual Audio Track output stream started at $SAMPLE_RATE Hz Stereo")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start AudioTrack", e)
            isRunning.set(false)
        }
    }

    fun stop() {
        if (!isRunning.get()) return
        isRunning.set(false)

        try {
            playbackThread?.interrupt()
            playbackThread?.join(500)
            playbackThread = null

            audioTrack?.apply {
                if (state == AudioTrack.STATE_INITIALIZED) {
                    stop()
                    release()
                }
            }
            audioTrack = null
            Log.i(TAG, "AudioTrack stopped")
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping AudioTrack", e)
        }
    }

    fun playChime(type: Int = 1): Boolean {
        // Ensure player is running
        if (!isRunning.get()) {
            start()
        }
        return GsiEngine.nativePlayChime(type)
    }

    fun isPlaying(): Boolean = isRunning.get()
}
