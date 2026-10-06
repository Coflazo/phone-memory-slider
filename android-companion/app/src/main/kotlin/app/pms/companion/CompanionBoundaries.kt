package app.pms.companion

import android.app.PendingIntent

interface MediaCatalogSource {
    fun permissionCoverage(): PermissionCoverage
    suspend fun page(cursor: CatalogCursor?, limit: Int): CatalogPage
    suspend fun asset(assetId: String): GalleryAsset?
    suspend fun thumbnail(assetId: String, maxEdge: Int): ByteArray
    suspend fun readRange(assetId: String, range: ByteSlice): ByteArray
    suspend fun sha256(assetId: String): String
}

data class PreparedTrash(val plan: TrashPlan, val confirmation: PendingIntent)

interface TrashCoordinator {
    fun prepare(assets: List<GalleryAsset>): PreparedTrash
}
