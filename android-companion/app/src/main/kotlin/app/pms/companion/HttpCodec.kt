package app.pms.companion

import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.nio.charset.StandardCharsets

data class HttpRequest(
    val method: String,
    val target: String,
    val headers: Map<String, String>,
    val body: ByteArray,
)

class HttpProtocolException(val status: Int, message: String) : Exception(message)

class HttpRequestParser(
    private val maxHeaderBytes: Int = 16 * 1024,
    private val maxBodyBytes: Int = 1024 * 1024,
) {
    init {
        require(maxHeaderBytes >= 32) { "Header limit is too small" }
        require(maxBodyBytes >= 0) { "Body limit cannot be negative" }
    }

    fun parse(input: InputStream): HttpRequest {
        val headerBytes = readHeaders(input)
        val headerText = headerBytes.toString(StandardCharsets.ISO_8859_1)
        val lines = headerText.removeSuffix("\r\n\r\n").split("\r\n")
        if (lines.isEmpty()) throw HttpProtocolException(400, "Missing request line")

        val requestParts = lines.first().split(' ')
        if (requestParts.size != 3 || requestParts.any(String::isEmpty)) {
            throw HttpProtocolException(400, "Malformed request line")
        }
        val (method, target, version) = requestParts
        if (!method.matches(METHOD) || !target.startsWith('/') || target.any { it == '\u0000' }) {
            throw HttpProtocolException(400, "Unsupported request target")
        }
        if (version != "HTTP/1.1") throw HttpProtocolException(505, "HTTP/1.1 is required")

        val headers = linkedMapOf<String, String>()
        lines.drop(1).forEach { line ->
            if (line.isEmpty() || line.first().isWhitespace()) {
                throw HttpProtocolException(400, "Malformed header")
            }
            val separator = line.indexOf(':')
            if (separator <= 0) throw HttpProtocolException(400, "Malformed header")
            val name = line.substring(0, separator).lowercase()
            if (!name.matches(HEADER_NAME) || headers.containsKey(name)) {
                throw HttpProtocolException(400, "Duplicate or invalid header")
            }
            val value = line.substring(separator + 1).trim()
            if (value.any { it == '\r' || it == '\n' || it == '\u0000' }) {
                throw HttpProtocolException(400, "Invalid header value")
            }
            headers[name] = value
        }
        if (headers.containsKey("transfer-encoding")) {
            throw HttpProtocolException(400, "Transfer encoding is unsupported")
        }

        val contentLength = headers["content-length"]?.toLongOrNull()
            ?: if (headers.containsKey("content-length")) {
                throw HttpProtocolException(400, "Invalid content length")
            } else {
                0L
            }
        if (contentLength < 0) throw HttpProtocolException(400, "Invalid content length")
        if (contentLength > maxBodyBytes) throw HttpProtocolException(413, "Request body is too large")

        val body = ByteArray(contentLength.toInt())
        var offset = 0
        while (offset < body.size) {
            val count = input.read(body, offset, body.size - offset)
            if (count < 0) throw HttpProtocolException(400, "Truncated request body")
            offset += count
        }
        return HttpRequest(method, target, headers, body)
    }

    private fun readHeaders(input: InputStream): ByteArray {
        val output = ByteArrayOutputStream(minOf(maxHeaderBytes, 1024))
        var matched = 0
        while (true) {
            val next = input.read()
            if (next < 0) throw HttpProtocolException(400, "Truncated request headers")
            output.write(next)
            if (output.size() > maxHeaderBytes) throw HttpProtocolException(431, "Request headers are too large")
            matched = when {
                next == HEADER_END[matched].toInt() -> matched + 1
                next == HEADER_END[0].toInt() -> 1
                else -> 0
            }
            if (matched == HEADER_END.size) return output.toByteArray()
        }
    }

    companion object {
        private val HEADER_END = "\r\n\r\n".toByteArray(StandardCharsets.US_ASCII)
        private val METHOD = Regex("[A-Z]{1,16}")
        private val HEADER_NAME = Regex("[!#$%&'*+.^_`|~0-9a-z-]+")
    }
}

data class HttpResponse(
    val status: Int,
    val contentType: String = "application/json; charset=utf-8",
    val body: ByteArray = ByteArray(0),
    val headers: Map<String, String> = emptyMap(),
) {
    fun writeTo(output: OutputStream) {
        val reason = REASONS[status] ?: "Response"
        val prefix = buildString {
            append("HTTP/1.1 $status $reason\r\n")
            append("Content-Type: $contentType\r\n")
            append("Content-Length: ${body.size}\r\n")
            append("Connection: close\r\n")
            headers.forEach { (name, value) ->
                require(!name.contains('\r') && !name.contains('\n'))
                require(!value.contains('\r') && !value.contains('\n'))
                append("$name: $value\r\n")
            }
            append("\r\n")
        }
        output.write(prefix.toByteArray(StandardCharsets.US_ASCII))
        output.write(body)
        output.flush()
    }

    companion object {
        private val REASONS = mapOf(
            200 to "OK",
            201 to "Created",
            202 to "Accepted",
            206 to "Partial Content",
            400 to "Bad Request",
            401 to "Unauthorized",
            403 to "Forbidden",
            404 to "Not Found",
            409 to "Conflict",
            413 to "Payload Too Large",
            416 to "Range Not Satisfiable",
            431 to "Request Header Fields Too Large",
            500 to "Internal Server Error",
            505 to "HTTP Version Not Supported",
        )
    }
}
