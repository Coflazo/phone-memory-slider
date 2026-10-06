package app.pms.companion

import java.net.URI
import java.net.URLDecoder
import java.nio.charset.StandardCharsets
import kotlinx.coroutines.runBlocking
import org.json.JSONArray
import org.json.JSONObject

class GalleryApi(
    private val catalog: MediaCatalogSource,
    private val deviceId: String,
    private val deviceName: String,
    private val trashCoordinator: TrashCoordinator,
    private val trashRuntime: TrashRequestRuntime = TrashRequestRuntime.shared,
    private val requestConfirmation: (String, PreparedTrash) -> Unit,
) {
    fun handle(request: HttpRequest): HttpResponse = try {
        val uri = URI(request.target)
        val query = parseQuery(uri.rawQuery)
        when {
            request.method == "GET" && uri.path == "/v1/capabilities" -> capabilities()
            request.method == "GET" && uri.path == "/v1/catalog" -> catalog(query)
            request.method == "GET" && uri.path == "/v1/thumbnail" -> thumbnail(query)
            request.method == "GET" && uri.path == "/v1/content" -> content(query, request.headers["range"])
            request.method == "GET" && uri.path == "/v1/hash" -> hash(query)
            request.method == "POST" && uri.path == "/v1/trash/prepare" -> prepareTrash(request.body)
            request.method == "POST" && uri.path == "/v1/trash/commit" -> commitTrash(request.body)
            request.method == "GET" && uri.path == "/v1/trash/result" -> trashResult(query)
            else -> json(404, "error" to "not_found")
        }
    } catch (_: StaleCatalogCursorException) {
        json(409, "error" to "catalog_changed")
    } catch (_: ByteRangeException) {
        json(416, "error" to "range_not_satisfiable")
    } catch (_: IllegalArgumentException) {
        json(400, "error" to "invalid_request")
    } catch (_: SecurityException) {
        json(403, "error" to "forbidden")
    } catch (_: Exception) {
        json(500, "error" to "local_operation_failed")
    }

    private fun capabilities(): HttpResponse = json(
        200,
        "protocol_version" to 1,
        "device_id" to deviceId,
        "device_name" to deviceName,
        "permission_coverage" to catalog.permissionCoverage().name.lowercase(),
        "supports_favorites" to true,
        "supports_recoverable_trash" to true,
    )

    private fun catalog(query: Map<String, String>): HttpResponse {
        val limit = query["limit"]?.toIntOrNull() ?: 1_000
        val cursor = query["cursor"]?.takeIf(String::isNotEmpty)?.let(::parseCursor)
        val page = runBlocking { catalog.page(cursor, limit) }
        return HttpResponse(200, body = CatalogJson.encode(page).toByteArray(StandardCharsets.UTF_8))
    }

    private fun thumbnail(query: Map<String, String>): HttpResponse {
        val assetId = required(query, "asset_id")
        val maxEdge = required(query, "max_edge").toIntOrNull() ?: error("Invalid thumbnail edge")
        val bytes = runBlocking { catalog.thumbnail(assetId, maxEdge) }
        return HttpResponse(200, contentType = "image/jpeg", body = bytes)
    }

    private fun content(query: Map<String, String>, rangeHeader: String?): HttpResponse {
        val assetId = required(query, "asset_id")
        val asset = runBlocking { requireNotNull(catalog.asset(assetId)) }
        val slice = ByteRange.parse(rangeHeader, asset.bytes, 8 * 1024 * 1024)
        val bytes = runBlocking { catalog.readRange(assetId, slice) }
        val status = if (slice.offset == 0L && slice.length.toLong() == asset.bytes) 200 else 206
        return HttpResponse(
            status,
            contentType = asset.mimeType.ifEmpty { "application/octet-stream" },
            body = bytes,
            headers = mapOf(
                "Accept-Ranges" to "bytes",
                "Content-Range" to "bytes ${slice.offset}-${slice.offset + slice.length - 1}/${asset.bytes}",
            ),
        )
    }

    private fun hash(query: Map<String, String>): HttpResponse = json(
        200,
        "asset_id" to required(query, "asset_id"),
        "sha256" to runBlocking { catalog.sha256(required(query, "asset_id")) },
    )

    private fun prepareTrash(body: ByteArray): HttpResponse {
        val values = JSONObject(String(body, StandardCharsets.UTF_8)).getJSONArray("asset_ids")
        require(values.length() in 1..500) { "Trash batch is outside supported bounds" }
        val requestedIds = buildList(values.length()) {
            repeat(values.length()) { add(values.getString(it).also { id -> require(id.length in 1..512) }) }
        }
        val assets = runBlocking { requestedIds.map { requireNotNull(catalog.asset(it)) } }
        val prepared = trashRuntime.prepare(TrashPlanner.prepare(assets, requestedIds))
        return json(
            200,
            "token" to prepared.token,
            "asset_ids" to JSONArray(prepared.plan.assetIds),
            "total_bytes" to prepared.plan.totalBytes,
        )
    }

    private fun commitTrash(body: ByteArray): HttpResponse {
        val token = JSONObject(String(body, StandardCharsets.UTF_8)).getString("token")
        require(token.length in 16..256) { "Trash token is invalid" }
        val started = trashRuntime.commit(token)
        val outcome = requireNotNull(trashRuntime.result(token)) { "Trash token is invalid" }
        require(outcome.status == TrashStatus.Pending || outcome.status == TrashStatus.Complete) {
            "Trash token is invalid"
        }
        if (started) {
            val plan = requireNotNull(trashRuntime.plan(token))
            try {
                val assets = runBlocking { plan.assetIds.map { requireNotNull(catalog.asset(it)) } }
                requestConfirmation(token, trashCoordinator.prepare(assets))
            } catch (failure: Exception) {
                trashRuntime.finish(token, approved = true, failedIds = plan.assetIds.toSet())
                throw failure
            }
        }
        return json(
            if (outcome.status == TrashStatus.Complete) 200 else 202,
            "token" to token,
            "status" to outcome.status.name.lowercase(),
        )
    }

    private fun trashResult(query: Map<String, String>): HttpResponse {
        val result = requireNotNull(trashRuntime.result(required(query, "token")))
        return json(
            200,
            "token" to result.token,
            "status" to result.status.name.lowercase(),
            "trashed_ids" to JSONArray(result.trashedIds),
            "failed_ids" to JSONArray(result.failedIds),
            "user_cancelled" to result.userCancelled,
        )
    }

    private fun parseCursor(value: String): CatalogCursor {
        val parts = value.split(':', limit = 2)
        require(parts.size == 2)
        return CatalogCursor(parts[0].toLong(), parts[1].toInt())
    }

    private fun required(query: Map<String, String>, key: String): String =
        requireNotNull(query[key]).also { require(it.isNotEmpty()) }

    private fun parseQuery(raw: String?): Map<String, String> = raw?.split('&')?.filter(String::isNotEmpty)?.associate { part ->
        val pieces = part.split('=', limit = 2)
        require(pieces.size == 2)
        URLDecoder.decode(pieces[0], StandardCharsets.UTF_8.name()) to
            URLDecoder.decode(pieces[1], StandardCharsets.UTF_8.name())
    } ?: emptyMap()

    private fun json(status: Int, vararg fields: Pair<String, Any>): HttpResponse {
        val objectValue = JSONObject()
        fields.forEach { (key, value) -> objectValue.put(key, value) }
        return HttpResponse(status, body = objectValue.toString().toByteArray(StandardCharsets.UTF_8))
    }
}
