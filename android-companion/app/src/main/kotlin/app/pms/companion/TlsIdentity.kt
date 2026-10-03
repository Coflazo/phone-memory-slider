package app.pms.companion

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.math.BigInteger
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.SecureRandom
import java.security.cert.X509Certificate
import java.util.Date
import javax.net.ssl.KeyManagerFactory
import javax.net.ssl.SSLContext
import javax.security.auth.x500.X500Principal

data class TlsIdentity(val sslContext: SSLContext)

object TlsIdentityStore {
    private const val STORE = "AndroidKeyStore"
    private const val ALIAS = "phone-memory-slider-local-tls-v1"

    fun loadOrCreate(): TlsIdentity {
        val keyStore = KeyStore.getInstance(STORE).apply { load(null) }
        if (!keyStore.containsAlias(ALIAS)) createKeyPair()
        check(keyStore.getCertificate(ALIAS) is X509Certificate) { "Local TLS certificate is unavailable" }
        val keyManagers = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm()).apply {
            init(keyStore, null)
        }
        val sslContext = SSLContext.getInstance("TLS").apply {
            init(keyManagers.keyManagers, null, SecureRandom())
        }
        return TlsIdentity(sslContext)
    }

    private fun createKeyPair() {
        val now = System.currentTimeMillis()
        val generator = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_RSA, STORE)
        generator.initialize(
            KeyGenParameterSpec.Builder(
                ALIAS,
                KeyProperties.PURPOSE_SIGN or KeyProperties.PURPOSE_VERIFY,
            )
                .setKeySize(2048)
                .setDigests(KeyProperties.DIGEST_SHA256)
                .setSignaturePaddings(KeyProperties.SIGNATURE_PADDING_RSA_PKCS1)
                .setCertificateSubject(X500Principal("CN=Phone Memory Slider Local"))
                .setCertificateSerialNumber(BigInteger(63, SecureRandom()).max(BigInteger.ONE))
                .setCertificateNotBefore(Date(now - 24L * 60 * 60 * 1_000))
                .setCertificateNotAfter(Date(now + 10L * 365 * 24 * 60 * 60 * 1_000))
                .setUserAuthenticationRequired(false)
                .build(),
        )
        generator.generateKeyPair()
    }
}
