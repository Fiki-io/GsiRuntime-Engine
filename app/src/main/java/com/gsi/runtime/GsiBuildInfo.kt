package com.gsi.runtime

data class GsiBuildInfo(
    val osVersion: String,
    val sdkVersion: String,
    val buildId: String,
    val securityPatch: String,
    val model: String,
    val fingerprint: String,
    val isTrebleEnabled: Boolean,
    val rawContent: String
) {
    val displayTitle: String
        get() = if (osVersion.isNotBlank()) "Android $osVersion (API $sdkVersion)" else "Android GSI"
}
