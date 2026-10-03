package app.pms.companion

import java.security.SecureRandom
import java.util.Base64
import java.util.concurrent.ConcurrentHashMap

enum class TrashStatus { Prepared, Pending, Complete, Cancelled }

data class PreparedTrashBatch(val token: String, val plan: TrashPlan)

data class TrashOutcome(
    val token: String,
    val status: TrashStatus,
    val trashedIds: List<String> = emptyList(),
    val failedIds: List<String> = emptyList(),
    val userCancelled: Boolean = false,
)

class TrashRequestRuntime(
    private val tokenFactory: () -> String = {
        ByteArray(24).also(SecureRandom()::nextBytes).let(Base64.getUrlEncoder().withoutPadding()::encodeToString)
    },
) {
    private data class Entry(val plan: TrashPlan, var outcome: TrashOutcome)
    private val entries = ConcurrentHashMap<String, Entry>()

    fun prepare(plan: TrashPlan): PreparedTrashBatch {
        require(plan.assetIds.isNotEmpty()) { "Trash batch cannot be empty" }
        val token = tokenFactory()
        require(token.length in 16..256 && entries.putIfAbsent(token, Entry(plan, TrashOutcome(token, TrashStatus.Prepared))) == null) {
            "Could not allocate a trash token"
        }
        return PreparedTrashBatch(token, plan)
    }

    fun commit(token: String): Boolean {
        val entry = entries[token] ?: return false
        synchronized(entry) {
            if (entry.outcome.status != TrashStatus.Prepared) return false
            entry.outcome = TrashOutcome(token, TrashStatus.Pending)
            return true
        }
    }

    fun finish(token: String, approved: Boolean, failedIds: Set<String> = emptySet()) {
        val entry = entries[token] ?: return
        synchronized(entry) {
            if (entry.outcome.status != TrashStatus.Pending) return
            val validFailures = entry.plan.assetIds.filter(failedIds::contains)
            entry.outcome = if (approved) {
                TrashOutcome(
                    token,
                    TrashStatus.Complete,
                    trashedIds = entry.plan.assetIds.filterNot(failedIds::contains),
                    failedIds = validFailures,
                )
            } else {
                TrashOutcome(token, TrashStatus.Cancelled, userCancelled = true)
            }
        }
    }

    fun result(token: String): TrashOutcome? = entries[token]?.outcome

    fun plan(token: String): TrashPlan? = entries[token]?.plan

    fun invalidateUncommitted() {
        entries.entries.removeIf { it.value.outcome.status == TrashStatus.Prepared }
    }

    companion object {
        val shared = TrashRequestRuntime()
    }
}
