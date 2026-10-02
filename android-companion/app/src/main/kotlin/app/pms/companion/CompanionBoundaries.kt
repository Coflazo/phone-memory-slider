package app.pms.companion

import android.app.PendingIntent
import kotlinx.coroutines.flow.StateFlow

interface MediaCatalogSource {
    fun permissionCoverage(): PermissionCoverage
    suspend fun page(cursor: CatalogCursor?, limit: Int): CatalogPage
}

sealed interface PairingState {
    data object Idle : PairingState
    data class Advertising(val code: String) : PairingState
    data class Connected(val desktopName: String) : PairingState
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
