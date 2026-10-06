package app.pms.companion

import java.net.URLEncoder
import java.nio.charset.StandardCharsets
import java.util.Base64
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class PairingPayloadTest {
    private val secret = Base64.getUrlEncoder().withoutPadding().encodeToString(ByteArray(32) { it.toByte() })

    @Test
    fun parsesBoundedBluetoothPairingCode() {
        val raw = "pms://pair?v=1&address=AA%3ABB%3ACC%3ADD%3AEE%3AFF&uuid=${PairingPayload.SERVICE_UUID}" +
            "&secret=$secret&name=${URLEncoder.encode("Studio PC", StandardCharsets.UTF_8.name())}"

        val parsed = PairingPayload.parse(raw)

        assertEquals("AA:BB:CC:DD:EE:FF", parsed.address)
        assertEquals("Studio PC", parsed.computerName)
    }

    @Test
    fun rejectsShortSecret() {
        val raw = "pms://pair?v=1&address=AA%3ABB%3ACC%3ADD%3AEE%3AFF&uuid=${PairingPayload.SERVICE_UUID}" +
            "&secret=AA&name=PC"

        assertThrows(IllegalArgumentException::class.java) { PairingPayload.parse(raw) }
    }
}
