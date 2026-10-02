package app.pms.companion

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class GalleryRulesTest {
    private val assets = listOf(
        GalleryAsset("a", "content://media/a", 100, "image/jpeg", 1, 10, 10, 0, false),
        GalleryAsset("favorite", "content://media/favorite", 200, "image/jpeg", 2, 10, 10, 0, true),
        GalleryAsset("c", "content://media/c", 300, "video/mp4", 3, 10, 10, 2_000, false),
    )

    @Test
    fun permissionCoverageDistinguishesDeniedPartialAndFull() {
        assertEquals(PermissionCoverage.Denied, resolvePermissionCoverage(false, false, false))
        assertEquals(PermissionCoverage.Partial, resolvePermissionCoverage(true, false, false))
        assertEquals(PermissionCoverage.Partial, resolvePermissionCoverage(false, false, true))
        assertEquals(PermissionCoverage.Full, resolvePermissionCoverage(true, true, false))
    }

    @Test
    fun catalogPagerRejectsStaleCursorAndKeepsStableOrder() {
        val pager = CatalogPager(7, assets)
        val first = pager.page(null, 2)
        assertEquals(listOf("a", "favorite"), first.assets.map(GalleryAsset::id))
        assertEquals(CatalogCursor(7, 2), first.nextCursor)
        assertThrows(IllegalArgumentException::class.java) { pager.page(CatalogCursor(6, 2), 2) }
        assertThrows(IllegalArgumentException::class.java) { pager.page(null, 0) }
    }

    @Test
    fun trashPlannerDeduplicatesAndNeverIncludesFavorites() {
        val plan = TrashPlanner.prepare(assets, listOf("a", "a", "c"))
        assertEquals(listOf("a", "c"), plan.assetIds)
        assertEquals(400, plan.totalBytes)
        assertThrows(IllegalArgumentException::class.java) {
            TrashPlanner.prepare(assets, listOf("favorite"))
        }
        assertThrows(IllegalArgumentException::class.java) {
            TrashPlanner.prepare(assets, listOf("missing"))
        }
    }
}
