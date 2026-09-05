package com.gsi.runtime

data class GsiImageInfo(
    val isValid: Boolean,
    val formatName: String,
    val blockSize: Long,
    val totalBlocks: Long,
    val uncompressedSizeBytes: Long,
    val description: String,
    val volumeName: String
) {
    val uncompressedSizeMb: Long
        get() = uncompressedSizeBytes / (1024 * 1024)
}
