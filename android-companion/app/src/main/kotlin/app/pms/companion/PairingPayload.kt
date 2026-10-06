package app.pms.companion

import java.net.URI
import java.net.URLDecoder
import java.nio.charset.StandardCharsets
import java.util.Base64
import java.util.UUID

data class PairingPayload(
    val address: String,
    val serviceUuid: UUID,
    val secret: String,
    val computerName: String,
) {
    companion object {
        val SERVICE_UUID: UUID = UUID.fromString("8f4d9d6a-0c84-4a7f-a36a-6fc12c4b34bf")

        fun parse(raw: String): PairingPayload {
            require(raw.length in 80..1_024) { "This is not a Phone Memory Slider code" }
            val uri = URI(raw)
            require(uri.scheme == "pms" && uri.host == "pair") { "This is not a Phone Memory Slider code" }
            val query = linkedMapOf<String, String>()
            uri.rawQuery.orEmpty().split('&').filter(String::isNotEmpty).forEach { field ->
                val parts = field.split('=', limit = 2)
                require(parts.size == 2) { "The pairing code is incomplete" }
                val key = decode(parts[0])
                require(query.put(key, decode(parts[1])) == null) { "The pairing code is ambiguous" }
            }
            require(query["v"] == "1") { "The computer uses an unsupported pairing version" }
            val address = requireNotNull(query["address"]).uppercase()
            require(address.matches(Regex("(?:[0-9A-F]{2}:){5}[0-9A-F]{2}"))) {
                "The computer Bluetooth address is invalid"
            }
            val uuid = UUID.fromString(requireNotNull(query["uuid"]))
            require(uuid == SERVICE_UUID) { "The pairing service is not recognized" }
            val secret = requireNotNull(query["secret"])
            val secretBytes = Base64.getUrlDecoder().decode(secret)
            require(secretBytes.size == 32) { "The pairing secret is invalid" }
            val computerName = requireNotNull(query["name"]).trim()
            require(computerName.length in 1..80) { "The computer name is invalid" }
            return PairingPayload(address, uuid, secret, computerName)
        }

        private fun decode(value: String): String =
            URLDecoder.decode(value, StandardCharsets.UTF_8.name())
    }
}
