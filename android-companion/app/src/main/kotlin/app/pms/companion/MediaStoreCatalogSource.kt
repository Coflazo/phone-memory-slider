package app.pms.companion

import android.Manifest
import android.content.ContentResolver
import android.content.ContentUris
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.provider.MediaStore
import android.util.Size
import androidx.core.content.ContextCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.security.MessageDigest
import java.util.concurrent.ConcurrentHashMap

class MediaStoreCatalogSource(private val context: Context) : MediaCatalogSource {
    private val resolver: ContentResolver = context.contentResolver
    private val collection = MediaStore.Files.getContentUri(MediaStore.VOLUME_EXTERNAL)
    private val assetsById = ConcurrentHashMap<String, GalleryAsset>()

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
        if (cursor != null && cursor.revision != revision) throw StaleCatalogCursorException()
        val offset = cursor?.offset ?: 0
        require(offset >= 0) { "Catalog offset cannot be negative" }
        require(offset <= Int.MAX_VALUE - limit) { "Catalog cursor is outside supported bounds" }
        // ponytail: rebuild the in-memory opaque-ID index once after a resumed service session;
        // replace with a persisted authenticated ID map only if real-device memory profiling requires it.
        val rebuildPrefix = offset > 0 && assetsById.isEmpty()
        val queryOffset = if (rebuildPrefix) 0 else offset
        val queryLimit = if (rebuildPrefix) offset + limit else limit

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
            putInt(ContentResolver.QUERY_ARG_OFFSET, queryOffset)
            putInt(ContentResolver.QUERY_ARG_LIMIT, queryLimit)
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
                var rowOffset = queryOffset
                while (rows.moveToNext()) {
                    val mediaId = rows.getLong(id)
                    val mediaUri = ContentUris.withAppendedId(collection, mediaId).toString()
                    val asset = GalleryAsset(
                        id = opaqueAssetId(mediaUri),
                        uri = mediaUri,
                        bytes = rows.getLong(size).coerceAtLeast(0),
                        mimeType = rows.getString(mime).orEmpty(),
                        modifiedEpochMs = rows.getLong(modified) * 1_000,
                        width = rows.getInt(width),
                        height = rows.getInt(height),
                        durationMs = rows.getLong(duration),
                        favorite = rows.getInt(favorite) != 0,
                    )
                    assetsById[asset.id] = asset
                    if (!rebuildPrefix || rowOffset >= offset) add(asset)
                    rowOffset += 1
                }
            } ?: error("MediaStore query failed")
        }
        CatalogPage(
            revision = revision,
            assets = assets,
            nextCursor = if (assets.size == limit) CatalogCursor(revision, offset + assets.size) else null,
        )
    }

    override suspend fun asset(assetId: String): GalleryAsset? = assetsById[assetId]

    override suspend fun thumbnail(assetId: String, maxEdge: Int): ByteArray = withContext(Dispatchers.IO) {
        require(maxEdge in 32..2_048) { "Thumbnail edge is outside supported bounds" }
        val asset = requireNotNull(asset(assetId)) { "Asset is not in the current catalog" }
        val bitmap = resolver.loadThumbnail(android.net.Uri.parse(asset.uri), Size(maxEdge, maxEdge), null)
        ByteArrayOutputStream().use { output ->
            check(bitmap.compress(android.graphics.Bitmap.CompressFormat.JPEG, 88, output)) { "Thumbnail encoding failed" }
            output.toByteArray().also { require(it.size <= 5 * 1024 * 1024) { "Thumbnail is too large" } }
        }
    }

    override suspend fun readRange(assetId: String, range: ByteSlice): ByteArray = withContext(Dispatchers.IO) {
        require(range.length in 1..8 * 1024 * 1024) { "Content range is outside supported bounds" }
        val asset = requireNotNull(asset(assetId)) { "Asset is not in the current catalog" }
        resolver.openInputStream(android.net.Uri.parse(asset.uri))!!.use { input ->
            var remainingSkip = range.offset
            while (remainingSkip > 0) {
                val skipped = input.skip(remainingSkip)
                if (skipped <= 0) check(input.read() >= 0) { "Content range is unavailable" } else remainingSkip -= skipped
                if (skipped <= 0) remainingSkip -= 1
            }
            val bytes = ByteArray(range.length)
            var offset = 0
            while (offset < bytes.size) {
                val read = input.read(bytes, offset, bytes.size - offset)
                check(read >= 0) { "Content range is truncated" }
                offset += read
            }
            bytes
        }
    }

    override suspend fun sha256(assetId: String): String = withContext(Dispatchers.IO) {
        val asset = requireNotNull(asset(assetId)) { "Asset is not in the current catalog" }
        val digest = MessageDigest.getInstance("SHA-256")
        resolver.openInputStream(android.net.Uri.parse(asset.uri))!!.use { input ->
            val buffer = ByteArray(256 * 1024)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
        }
        digest.digest().joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }

    private fun granted(permission: String): Boolean =
        ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED
}
