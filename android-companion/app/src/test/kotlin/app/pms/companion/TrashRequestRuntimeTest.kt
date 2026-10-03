package app.pms.companion

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class TrashRequestRuntimeTest {
    @Test
    fun commitIsReplaySafeAndCancellationRemainsVisible() {
        val runtime = TrashRequestRuntime { "0123456789abcdef-token-a" }
        val prepared = runtime.prepare(TrashPlan(listOf("a", "b"), 30))
        assertEquals("0123456789abcdef-token-a", prepared.token)
        assertTrue(runtime.commit(prepared.token))
        assertFalse(runtime.commit(prepared.token))
        assertEquals(TrashStatus.Pending, runtime.result(prepared.token)!!.status)
        runtime.finish(prepared.token, approved = false)

        val result = runtime.result(prepared.token)!!
        assertEquals(TrashStatus.Cancelled, result.status)
        assertTrue(result.userCancelled)
        assertTrue(result.trashedIds.isEmpty())
    }

    @Test
    fun partialFailureAndDisconnectAreExplicit() {
        val runtime = TrashRequestRuntime { "0123456789abcdef-token-b" }
        val prepared = runtime.prepare(TrashPlan(listOf("a", "b", "c"), 60))
        runtime.commit(prepared.token)
        runtime.finish(prepared.token, approved = true, failedIds = setOf("b"))
        assertEquals(listOf("a", "c"), runtime.result(prepared.token)!!.trashedIds)
        assertEquals(listOf("b"), runtime.result(prepared.token)!!.failedIds)

        val uncommitted = TrashRequestRuntime { "0123456789abcdef-token-c" }
        uncommitted.prepare(TrashPlan(listOf("a"), 10))
        uncommitted.invalidateUncommitted()
        assertFalse(uncommitted.commit("0123456789abcdef-token-c"))
    }
}
