package app.pms.companion

import java.net.Inet6Address
import java.net.InetAddress
import java.nio.charset.StandardCharsets
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Base64
import java.util.Locale
import java.util.Optional

class SessionAuthenticator(
    pairingCode: String,
    private val tokenSource: () -> ByteArray = {
        ByteArray(32).also(SecureRandom()::nextBytes)
    },
) {
    private val expectedCode = pairingCode.toByteArray(StandardCharsets.US_ASCII)
    private var failedAttempts = 0
    private var locked = false

    @Volatile
    private var currentToken: String? = null

    init {
        require(pairingCode.matches(Regex("[0-9]{6}"))) { "Pairing code must contain six digits" }
    }

    @Synchronized
    fun pair(candidate: String): Optional<String> {
        if (locked || currentToken != null) return Optional.empty()
        val supplied = candidate.toByteArray(StandardCharsets.US_ASCII)
        if (!MessageDigest.isEqual(expectedCode, supplied)) {
            failedAttempts += 1
            locked = failedAttempts >= MAX_ATTEMPTS
            return Optional.empty()
        }
        val bytes = tokenSource()
        require(bytes.size >= 32) { "Session tokens require at least 256 bits" }
        return Base64.getUrlEncoder().withoutPadding().encodeToString(bytes)
            .also { currentToken = it }
            .let(Optional<String>::of)
    }

    fun authorize(authorizationHeader: String?): Boolean {
        val token = currentToken ?: return false
        val expected = "Bearer $token".toByteArray(StandardCharsets.US_ASCII)
        val supplied = authorizationHeader.orEmpty().toByteArray(StandardCharsets.US_ASCII)
        return MessageDigest.isEqual(expected, supplied)
    }

    @Synchronized
    fun invalidate() {
        currentToken = null
    }

    companion object {
        private const val MAX_ATTEMPTS = 5

        fun newPairingCode(): String = String.format(Locale.ROOT, "%06d", SecureRandom().nextInt(1_000_000))

        fun requiresAuthentication(target: String): Boolean = target != "/v1/pair"
    }
}

object LocalPeerPolicy {
    fun isAllowed(address: InetAddress): Boolean {
        if (address.isAnyLocalAddress || address.isMulticastAddress) return false
        if (address.isLoopbackAddress || address.isLinkLocalAddress || address.isSiteLocalAddress) return true
        if (address is Inet6Address) {
            val first = address.address.first().toInt() and 0xFF
            return first and 0xFE == 0xFC
        }
        return false
    }
}
