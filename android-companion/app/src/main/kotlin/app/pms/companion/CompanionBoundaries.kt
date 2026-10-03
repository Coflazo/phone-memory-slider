package app.pms.companion

import android.app.PendingIntent
import kotlinx.coroutines.flow.StateFlow

interface MediaCatalogSource {
    fun permissionCoverage(): PermissionCoverage
    suspend fun page(cursor: CatalogCursor?, limit: Int): CatalogPage
    suspend fun asset(assetId: String): GalleryAsset?
    suspend fun thumbnail(assetId: String, maxEdge: Int): ByteArray
    suspend fun readRange(assetId: String, range: ByteSlice): ByteArray
    suspend fun sha256(assetId: String): String
}

sealed interface PairingState {
    data object Idle : PairingState
    data object Starting : PairingState
    data class Advertising(
        val code: String,
        val addresses: List<String>,
        val port: Int,
    ) : PairingState
    data class Connected(
        val desktopName: String,
        val code: String,
        val addresses: List<String>,
        val port: Int,
    ) : PairingState
    data class Failure(val message: String) : PairingState
}

interface PairingSession {
    val state: StateFlow<PairingState>
    fun start()
    fun stop()
}

data class PreparedTrash(val plan: TrashPlan, val confirmation: PendingIntent)

interface TrashCoordinator {
    fun prepare(assets: List<GalleryAsset>): PreparedTrash
}
