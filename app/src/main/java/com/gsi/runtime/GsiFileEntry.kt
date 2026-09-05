package com.gsi.runtime

data class GsiFileEntry(
    val name: String,
    val path: String,
    val inode: Long,
    val size: Long,
    val isDirectory: Boolean,
    val isSymlink: Boolean
) {
    val formattedSize: String
        get() = when {
            isDirectory -> "[DIR]"
            isSymlink -> "[LINK]"
            size >= 1024 * 1024 -> String.format("%.2f MB", size / (1024.0 * 1024.0))
            size >= 1024 -> String.format("%.1f KB", size / 1024.0)
            else -> "$size B"
        }
}
