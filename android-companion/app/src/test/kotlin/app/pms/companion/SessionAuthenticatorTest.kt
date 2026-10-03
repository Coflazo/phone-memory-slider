package app.pms.companion

import java.net.InetAddress
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SessionAuthenticatorTest {
    @Test
    fun exactCodePairsAndWrongCodesDoNot() {
        var seed = 1
        val authenticator = SessionAuthenticator("042719") { ByteArray(32) { seed++.toByte() } }

        assertFalse(authenticator.pair("042718").isPresent)
        assertFalse(authenticator.pair("42719").isPresent)
        val token = authenticator.pair("042719").orElseThrow()
        assertTrue(authenticator.authorize("Bearer $token"))
        assertFalse(authenticator.authorize("Bearer ${token}x"))
    }

    @Test
    fun successfulPairingCannotBeReplacedWithinTheSession() {
        var value = 1
        val authenticator = SessionAuthenticator("123456") { ByteArray(32) { value.toByte() }.also { value++ } }
        val first = authenticator.pair("123456").orElseThrow()

        assertFalse(authenticator.pair("123456").isPresent)
        assertTrue(authenticator.authorize("Bearer $first"))
        authenticator.invalidate()
        assertFalse(authenticator.authorize("Bearer $first"))
    }

    @Test
    fun fiveFailuresLockThePairingSession() {
        val authenticator = SessionAuthenticator("654321") { ByteArray(32) { 7 } }
        repeat(5) { assertFalse(authenticator.pair("000000").isPresent) }
        assertFalse(authenticator.pair("654321").isPresent)
        assertTrue(SessionAuthenticator.newPairingCode().matches(Regex("[0-9]{6}")))
    }

    @Test
    fun onlyPairRouteIsPublic() {
        assertFalse(SessionAuthenticator.requiresAuthentication("/v1/pair"))
        assertTrue(SessionAuthenticator.requiresAuthentication("/v1/catalog"))
        assertTrue(SessionAuthenticator.requiresAuthentication("/v1/pair/extra"))
    }

    @Test
    fun peerPolicyAllowsLocalNetworksAndRejectsPublicOrWildcardAddresses() {
        assertTrue(LocalPeerPolicy.isAllowed(InetAddress.getByName("127.0.0.1")))
        assertTrue(LocalPeerPolicy.isAllowed(InetAddress.getByName("192.168.1.10")))
        assertTrue(LocalPeerPolicy.isAllowed(InetAddress.getByName("10.0.0.8")))
        assertTrue(LocalPeerPolicy.isAllowed(InetAddress.getByName("fd12:3456::1")))
        assertFalse(LocalPeerPolicy.isAllowed(InetAddress.getByName("8.8.8.8")))
        assertFalse(LocalPeerPolicy.isAllowed(InetAddress.getByName("0.0.0.0")))
        assertFalse(LocalPeerPolicy.isAllowed(InetAddress.getByName("ff02::1")))
    }
}
