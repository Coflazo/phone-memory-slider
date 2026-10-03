package app.pms.companion

import java.io.ByteArrayInputStream
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class HttpCodecTest {
    @Test
    fun parsesBoundedOriginFormRequest() {
        val raw = "POST /v1/pair HTTP/1.1\r\nHost: phone\r\nContent-Length: 6\r\nX-Test: Value\r\n\r\n123456"
        val request = HttpRequestParser().parse(ByteArrayInputStream(raw.toByteArray()))

        assertEquals("POST", request.method)
        assertEquals("/v1/pair", request.target)
        assertEquals("Value", request.headers["x-test"])
        assertArrayEquals("123456".toByteArray(), request.body)
    }

    @Test
    fun rejectsOversizedHeadersAndBodies() {
        val headerHeavy = "GET / HTTP/1.1\r\nLong: ${"x".repeat(80)}\r\n\r\n"
        assertEquals(
            431,
            assertThrows(HttpProtocolException::class.java) {
                HttpRequestParser(maxHeaderBytes = 64).parse(ByteArrayInputStream(headerHeavy.toByteArray()))
            }.status,
        )

        val bodyHeavy = "POST / HTTP/1.1\r\nContent-Length: 9\r\n\r\n123456789"
        assertEquals(
            413,
            assertThrows(HttpProtocolException::class.java) {
                HttpRequestParser(maxBodyBytes = 8).parse(ByteArrayInputStream(bodyHeavy.toByteArray()))
            }.status,
        )
    }

    @Test
    fun rejectsMalformedOrAmbiguousRequests() {
        val malformed = listOf(
            "GET http://outside.example/ HTTP/1.1\r\n\r\n",
            "GET / HTTP/2\r\n\r\n",
            "POST / HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nxx",
            "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
            "GET / HTTP/1.1\r\n folded\r\n\r\n",
        )
        malformed.forEach { raw ->
            assertThrows(HttpProtocolException::class.java) {
                HttpRequestParser().parse(ByteArrayInputStream(raw.toByteArray()))
            }
        }
    }
}
