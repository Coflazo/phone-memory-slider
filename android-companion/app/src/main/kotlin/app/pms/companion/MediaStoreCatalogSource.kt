package app.pms.companion

import android.Manifest
import android.content.ContentResolver
import android.content.ContentUris
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.provider.MediaStore
import androidx.core.content.ContextCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

class MediaStoreCatalogSource(private val context: Context) : MediaCatalogSource {
    private val resolver: ContentResolver = context.contentResolver
    private val collection = MediaStore.Files.getContentUri(MediaStore.VOLUME_EXTERNAL)

    override fun permissionCoverage(): PermissionCoverage {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            return if (granted(Manifest.permission.READ_EXTERNAL_STORAGE)) {
                PermissionCoverage.Full
            } else {
                PermissionCoverage.Denied
            }
        }
        val selectedOnly = Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE &&
            granted(Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED)
        return resolvePermissionCoverage(
            hasImages = granted(Manifest.permission.READ_MEDIA_IMAGES),
            hasVideos = granted(Manifest.permission.READ_MEDIA_VIDEO),
            selectedOnly = selectedOnly,
        )
    }

    override suspend fun page(cursor: CatalogCursor?, limit: Int): CatalogPage = withContext(Dispatchers.IO) {
        require(limit in 1..1_000) { "Catalog page size must be between 1 and 1000" }
        val revision = MediaStore.getVersion(context).hashCode().toUInt().toLong()
        require(cursor == null || cursor.revision == revision) { "Catalog cursor is stale" }
        val offset = cursor?.offset ?: 0
        require(offset >= 0) { "Catalog offset cannot be negative" }

        val projection = arrayOf(
            MediaStore.Files.FileColumns._ID,
            MediaStore.MediaColumns.SIZE,
            MediaStore.MediaColumns.MIME_TYPE,
            MediaStore.MediaColumns.DATE_MODIFIED,
            MediaStore.MediaColumns.WIDTH,
            MediaStore.MediaColumns.HEIGHT,
            MediaStore.MediaColumns.DURATION,
            MediaStore.MediaColumns.IS_FAVORITE,
        )
        val query = Bundle().apply {
            putString(
                ContentResolver.QUERY_ARG_SQL_SELECTION,
                "${MediaStore.Files.FileColumns.MEDIA_TYPE} IN (?, ?)",
            )
            putStringArray(
                ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS,
                arrayOf(
                    MediaStore.Files.FileColumns.MEDIA_TYPE_IMAGE.toString(),
                    MediaStore.Files.FileColumns.MEDIA_TYPE_VIDEO.toString(),
                ),
            )
            putStringArray(
                ContentResolver.QUERY_ARG_SORT_COLUMNS,
                arrayOf(MediaStore.MediaColumns.DATE_MODIFIED, MediaStore.Files.FileColumns._ID),
            )
            putInt(ContentResolver.QUERY_ARG_SORT_DIRECTION, ContentResolver.QUERY_SORT_DIRECTION_DESCENDING)
            putInt(ContentResolver.QUERY_ARG_OFFSET, offset)
            putInt(ContentResolver.QUERY_ARG_LIMIT, limit)
        }

        val assets = buildList {
            resolver.query(collection, projection, query, null)?.use { rows ->
                val id = rows.getColumnIndexOrThrow(MediaStore.Files.FileColumns._ID)
                val size = rows.getColumnIndexOrThrow(MediaStore.MediaColumns.SIZE)
                val mime = rows.getColumnIndexOrThrow(MediaStore.MediaColumns.MIME_TYPE)
                val modified = rows.getColumnIndexOrThrow(MediaStore.MediaColumns.DATE_MODIFIED)
                val width = rows.getColumnIndexOrThrow(MediaStore.MediaColumns.WIDTH)
                val height = rows.getColumnIndexOrThrow(MediaStore.MediaColumns.HEIGHT)
                val duration = rows.getColumnIndexOrThrow(MediaStore.MediaColumns.DURATION)
                val favorite = rows.getColumnIndexOrThrow(MediaStore.MediaColumns.IS_FAVORITE)
                while (rows.moveToNext()) {
                    val mediaId = rows.getLong(id)
                    add(
                        GalleryAsset(
                            id = mediaId.toString(),
                            uri = ContentUris.withAppendedId(collection, mediaId).toString(),
                            bytes = rows.getLong(size).coerceAtLeast(0),
                            mimeType = rows.getString(mime).orEmpty(),
                            modifiedEpochMs = rows.getLong(modified) * 1_000,
                            width = rows.getInt(width),
                            height = rows.getInt(height),
                            durationMs = rows.getLong(duration),
                            favorite = rows.getInt(favorite) != 0,
                        ),
                    )
                }
            } ?: error("MediaStore query failed")
        }
        CatalogPage(
            revision = revision,
            assets = assets,
            nextCursor = if (assets.size == limit) CatalogCursor(revision, offset + assets.size) else null,
        )
    }

    private fun granted(permission: String): Boolean =
        ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED
}
