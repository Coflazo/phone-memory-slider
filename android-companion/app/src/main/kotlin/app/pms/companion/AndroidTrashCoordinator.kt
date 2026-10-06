package app.pms.companion

import android.content.ContentResolver
import android.provider.MediaStore
import androidx.core.net.toUri
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

class AndroidTrashCoordinator(private val resolver: ContentResolver) : TrashCoordinator {
    override fun prepare(assets: List<GalleryAsset>): PreparedTrash {
        val plan = TrashPlanner.prepare(assets, assets.map(GalleryAsset::id))
        val uris = assets.map { asset ->
            require(!asset.favorite) { "Favorites are protected" }
            asset.uri.toUri().also(::requireStillTrashable)
        }
        return PreparedTrash(
            plan = plan,
            confirmation = MediaStore.createTrashRequest(resolver, uris, true),
        )
    }

    private fun requireStillTrashable(uri: android.net.Uri) {
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

data class PendingTrashConfirmation(val token: String, val prepared: PreparedTrash)

object TrashConfirmationBus {
    private val pendingState = MutableStateFlow<PendingTrashConfirmation?>(null)
    val pending = pendingState.asStateFlow()

    fun request(token: String, prepared: PreparedTrash) {
        pendingState.value = PendingTrashConfirmation(token, prepared)
    }

    fun finish(approved: Boolean) {
        val confirmation = pendingState.value ?: return
        TrashRequestRuntime.shared.finish(confirmation.token, approved)
        pendingState.value = null
    }
}
