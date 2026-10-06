package app.pms.companion

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class GalleryProtocolTest {
    @Test
    fun opaqueIdsAreStableAndDoNotExposeMediaStoreIds() {
        val first = opaqueAssetId("content://media/external/file/42")
        assertEquals(first, opaqueAssetId("content://media/external/file/42"))
        assertFalse(first.contains("42"))
        assertTrue(first.matches(Regex("[0-9a-f]{32}")))
    }

    @Test
    fun catalogJsonHasStableBoundedFields() {
        val page = CatalogPage(
            revision = 7,
            assets = listOf(GalleryAsset("opaque", "private", 123, "image/jpeg", 4, 40, 30, 0, true)),
            nextCursor = CatalogCursor(7, 1),
        )
        assertEquals(
            "{\"revision\":7,\"assets\":[{\"asset_id\":\"opaque\",\"bytes\":123,\"mime_type\":\"image/jpeg\",\"modified_epoch_ms\":4,\"width\":40,\"height\":30,\"duration_ms\":0,\"favorite\":true}],\"next_cursor\":\"7:1\",\"complete\":false}",
            CatalogJson.encode(page),
        )
    }

    @Test
    fun rangeParserAllowsOneBoundedByteRange() {
        assertEquals(ByteSlice(10, 20), ByteRange.parse("bytes=10-29", 100, 32))
        assertEquals(ByteSlice(90, 10), ByteRange.parse("bytes=90-", 100, 32))
        assertThrows(IllegalArgumentException::class.java) { ByteRange.parse("bytes=0-2,4-5", 100, 32) }
        assertThrows(IllegalArgumentException::class.java) { ByteRange.parse("bytes=0-99", 100, 32) }
    }
}
