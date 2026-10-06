package app.pms.companion

enum class PermissionCoverage { Denied, Partial, Full }

data class GalleryAsset(
    val id: String,
    val uri: String,
    val bytes: Long,
    val mimeType: String,
    val modifiedEpochMs: Long,
    val width: Int,
    val height: Int,
    val durationMs: Long,
    val favorite: Boolean,
)

data class CatalogCursor(val revision: Long, val offset: Int)

data class CatalogPage(
    val revision: Long,
    val assets: List<GalleryAsset>,
    val nextCursor: CatalogCursor?,
)

fun resolvePermissionCoverage(
    hasImages: Boolean,
    hasVideos: Boolean,
    selectedOnly: Boolean,
): PermissionCoverage = when {
    hasImages && hasVideos -> PermissionCoverage.Full
    selectedOnly || hasImages || hasVideos -> PermissionCoverage.Partial
    else -> PermissionCoverage.Denied
}

class CatalogPager(
    private val revision: Long,
    private val assets: List<GalleryAsset>,
) {
    fun page(cursor: CatalogCursor?, limit: Int): CatalogPage {
        require(limit in 1..1_000) { "Catalog page size must be between 1 and 1000" }
        require(cursor == null || cursor.revision == revision) { "Catalog cursor is stale" }
        val offset = cursor?.offset ?: 0
        require(offset in 0..assets.size) { "Catalog cursor is outside the snapshot" }
        val end = minOf(offset + limit, assets.size)
        return CatalogPage(
            revision = revision,
            assets = assets.subList(offset, end),
            nextCursor = if (end < assets.size) CatalogCursor(revision, end) else null,
        )
    }
}

data class TrashPlan(val assetIds: List<String>, val totalBytes: Long)

object TrashPlanner {
    fun prepare(catalog: List<GalleryAsset>, requestedIds: List<String>): TrashPlan {
        val uniqueIds = requestedIds.toCollection(linkedSetOf())
        require(uniqueIds.isNotEmpty()) { "Trash batch cannot be empty" }
        val byId = catalog.associateBy(GalleryAsset::id)
        var bytes = 0L
        uniqueIds.forEach { id ->
            val asset = requireNotNull(byId[id]) { "Asset is not in the current catalog" }
            require(!asset.favorite) { "Favorites are protected" }
            require(asset.bytes >= 0) { "Asset size cannot be negative" }
            bytes = Math.addExact(bytes, asset.bytes)
        }
        return TrashPlan(uniqueIds.toList(), bytes)
    }
}
