package app.pms.companion

import android.content.ContentResolver
import android.net.Uri
import android.provider.MediaStore

class AndroidTrashCoordinator(private val resolver: ContentResolver) : TrashCoordinator {
    override fun prepare(assets: List<GalleryAsset>): PreparedTrash {
        val plan = TrashPlanner.prepare(assets, assets.map(GalleryAsset::id))
        val uris = assets.map { asset ->
            require(!asset.favorite) { "Favorites are protected" }
            Uri.parse(asset.uri).also(::requireStillTrashable)
        }
        return PreparedTrash(
            plan = plan,
            confirmation = MediaStore.createTrashRequest(resolver, uris, true),
        )
    }

    private fun requireStillTrashable(uri: Uri) {
        val result = resolver.query(
            uri,
            arrayOf(MediaStore.MediaColumns.IS_FAVORITE),
            null,
            null,
            null,
        )
        result.use { rows ->
            require(rows != null && rows.moveToFirst()) { "Asset is no longer accessible" }
            require(rows.getInt(0) == 0) { "Favorites are protected" }
        }
    }
}
