package com.gsi.runtime

data class GsiServiceInfo(
    val name: String,
    val binaryPath: String,
    val classes: String,
    val user: String,
    val status: String,
    val pid: Int
) {
    val isRunning: Boolean get() = status == "RUNNING"

    val displayDetails: String
        get() = "$binaryPath | Class: [$classes] | User: $user"
}
