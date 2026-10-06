package app.pms.companion

import java.nio.charset.StandardCharsets
import java.security.MessageDigest

data class ByteSlice(val offset: Long, val length: Int)

class StaleCatalogCursorException : IllegalArgumentException("Catalog cursor is stale")
class ByteRangeException(message: String) : IllegalArgumentException(message)

object ByteRange {
    fun parse(header: String?, totalBytes: Long, maxBytes: Int): ByteSlice {
        require(totalBytes >= 0 && maxBytes > 0) { "Invalid content bounds" }
        if (header == null) return ByteSlice(0, minOf(totalBytes, maxBytes.toLong()).toInt())
        if (!header.startsWith("bytes=") || header.contains(',')) {
            throw ByteRangeException("Only one byte range is supported")
        }
        val parts = header.removePrefix("bytes=").split('-', limit = 2)
        if (parts.size != 2 || parts[0].isEmpty()) throw ByteRangeException("Invalid byte range")
        val start = parts[0].toLongOrNull() ?: throw ByteRangeException("Invalid byte range")
        val inclusiveEnd = parts[1].takeIf(String::isNotEmpty)?.toLongOrNull()
            ?: if (parts[1].isEmpty()) totalBytes - 1 else throw ByteRangeException("Invalid byte range")
        if (start < 0 || start >= totalBytes || inclusiveEnd < start || inclusiveEnd >= totalBytes) {
            throw ByteRangeException("Byte range is outside the asset")
        }
        val length = Math.addExact(Math.subtractExact(inclusiveEnd, start), 1)
        if (length > maxBytes) throw ByteRangeException("Requested range is too large")
        return ByteSlice(start, length.toInt())
    }
}

fun opaqueAssetId(uri: String): String = MessageDigest.getInstance("SHA-256")
    .digest(("phone-memory-slider-v1\u0000$uri").toByteArray(StandardCharsets.UTF_8))
    .take(16)
    .joinToString("") { "%02x".format(it.toInt() and 0xff) }

object CatalogJson {
    fun encode(page: CatalogPage): String = buildString {
        append("{\"revision\":").append(page.revision).append(",\"assets\":[")
        page.assets.forEachIndexed { index, asset ->
            if (index > 0) append(',')
            append("{\"asset_id\":\"").append(escape(asset.id)).append("\",\"bytes\":").append(asset.bytes)
            append(",\"mime_type\":\"").append(escape(asset.mimeType)).append("\",\"modified_epoch_ms\":")
                .append(asset.modifiedEpochMs)
            append(",\"width\":").append(asset.width).append(",\"height\":").append(asset.height)
            append(",\"duration_ms\":").append(asset.durationMs).append(",\"favorite\":").append(asset.favorite)
            append('}')
        }
        val cursor = page.nextCursor
        append("],\"next_cursor\":")
        if (cursor == null) append("null") else append('"').append(cursor.revision).append(':').append(cursor.offset).append('"')
        append(",\"complete\":").append(cursor == null).append('}')
    }

    fun escape(value: String): String = buildString(value.length) {
        value.forEach { character ->
            when (character) {
                '\\' -> append("\\\\")
                '"' -> append("\\\"")
                '\b' -> append("\\b")
                '\u000c' -> append("\\f")
                '\n' -> append("\\n")
                '\r' -> append("\\r")
                '\t' -> append("\\t")
                else -> if (character.code < 0x20) append("\\u%04x".format(character.code)) else append(character)
            }
        }
    }
}
