#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "library/library.hpp"

using pms::desktop::Decision;
using pms::desktop::ItemRow;
using pms::desktop::Library;

namespace {

ItemRow row(const QString& source_id, const quint64 bytes = 1000, const qint64 modified_s = 1'700'000'000) {
    ItemRow item;
    item.source_id = source_id;
    item.relative_path = QStringLiteral("DCIM/Camera/") + source_id + QStringLiteral(".jpg");
    item.mime = QStringLiteral("image/jpeg");
    item.format = pms::MediaFormat::Jpeg;
    item.kind = pms::MediaKind::Photo;
    item.bytes = bytes;
    item.width = 4032;
    item.height = 3024;
    item.captured_ms = 1'699'999'999'000;
    item.modified_s = modified_s;
    item.favorite = source_id == QStringLiteral("fav");
    item.unit_id = 7;
    return item;
}

std::vector<float> vec(const float a, const float b, const float c) {
    return {a, b, c};
}

void write_halves(const QString& path, const qint64 offset, const std::vector<quint16>& halves) {
    QFile file{path};
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.seek(offset));
    const auto bytes = static_cast<qint64>(halves.size() * sizeof(quint16));
    QCOMPARE(file.write(reinterpret_cast<const char*>(halves.data()), bytes), bytes);
}

ItemRow find(const QList<ItemRow>& items, const QString& source_id) {
    for (const auto& item : items) {
        if (item.source_id == source_id) {
            return item;
        }
    }
    return {};
}

}  // namespace

class LibraryTests final : public QObject {
    Q_OBJECT

private slots:
    void upsertTwiceIsIdempotent() {
        QTemporaryDir directory;
        Library library{directory.path()};
        QString error;
        QVERIFY2(library.open(&error), qPrintable(error));

        const QList<ItemRow> rows{row(QStringLiteral("a")), row(QStringLiteral("fav"))};
        const auto first = library.upsertItems(QStringLiteral("usb:pixel"), rows, true);
        QCOMPARE(first.size(), 2);
        QVERIFY(first[0] != first[1]);
        const auto second = library.upsertItems(QStringLiteral("usb:pixel"), rows, true);
        QCOMPARE(second, first);

        const auto items = library.items(QStringLiteral("usb:pixel"));
        QCOMPARE(items.size(), 2);
        const auto a = find(items, QStringLiteral("a"));
        QCOMPARE(a.id, first[0]);
        QCOMPARE(a.relative_path, QStringLiteral("DCIM/Camera/a.jpg"));
        QCOMPARE(a.format, pms::MediaFormat::Jpeg);
        QCOMPARE(a.bytes, quint64{1000});
        QCOMPARE(a.width, quint32{4032});
        QCOMPARE(a.captured_ms, qint64{1'699'999'999'000});
        QCOMPARE(a.unit_id, qint64{7});
        QCOMPARE(a.embedding_row, -1);
        QVERIFY(!a.favorite);
        QVERIFY(find(items, QStringLiteral("fav")).favorite);
        QVERIFY(library.items(QStringLiteral("folder:other")).isEmpty());
    }

    void fullSyncWithoutAnItemMarksItRemoved() {
        QTemporaryDir directory;
        Library library{directory.path()};
        QVERIFY(library.open(nullptr));
        const auto source = QStringLiteral("usb:pixel");
        const auto ids = library.upsertItems(source, {row(QStringLiteral("a")), row(QStringLiteral("b"))}, true);
        library.setDecision(ids[1], Decision::Delete, 10);

        library.upsertItems(source, {row(QStringLiteral("a"))}, false);
        QCOMPARE(library.items(source).size(), 2);  // a partial sync removes nothing

        library.upsertItems(source, {row(QStringLiteral("a"))}, true);
        const auto items = library.items(source);
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].source_id, QStringLiteral("a"));
        QCOMPARE(library.decisions().value(ids[1]), Decision::Delete);  // history is kept

        const auto back = library.upsertItems(source, {row(QStringLiteral("a")), row(QStringLiteral("b"))}, true);
        QCOMPARE(back, ids);  // the same row comes back
        QCOMPARE(library.items(source).size(), 2);
    }

    void changedFileDropsStaleAnalysis() {
        QTemporaryDir directory;
        Library library{directory.path()};
        QVERIFY(library.open(nullptr));
        const auto source = QStringLiteral("folder:x");
        const auto id = library.upsertItems(source, {row(QStringLiteral("a"))}, true).front();
        library.setAnalysis(id, 0.5F, 0.25F, 0.125F, pms::PdqHash{{1, 2, 3, 0xFFFF'FFFF'FFFF'FFFFULL}, 90});
        const auto vector = vec(1, 0, 0);
        QCOMPARE(library.appendEmbedding(id, vector), 0);
        library.setError(id, QStringLiteral("Truncated JPEG"));

        auto item = library.items(source).front();
        QCOMPARE(item.sharpness, 0.5F);
        QCOMPARE(item.clipped, 0.25F);
        QCOMPARE(item.noise, 0.125F);
        QCOMPARE(item.pdq[3], quint64{0xFFFF'FFFF'FFFF'FFFFULL});
        QCOMPARE(item.pdq_quality, 90);
        QCOMPARE(item.embedding_row, 0);
        QCOMPARE(item.error, QStringLiteral("Truncated JPEG"));

        library.upsertItems(source, {row(QStringLiteral("a"))}, true);  // unchanged: analysis stays
        QCOMPARE(library.items(source).front().embedding_row, 0);

        library.upsertItems(source, {row(QStringLiteral("a"), 2000)}, true);  // edited on the phone
        item = library.items(source).front();
        QCOMPARE(item.bytes, quint64{2000});
        QCOMPARE(item.embedding_row, -1);
        QCOMPARE(item.sharpness, 0.0F);
        QCOMPARE(item.pdq_quality, 0);
        QVERIFY(item.error.isEmpty());
    }

    void embeddingsSurviveReopen() {
        QTemporaryDir directory;
        QList<qint64> ids;
        float cosine{};
        {
            Library library{directory.path()};
            QVERIFY(library.open(nullptr));
            ids = library.upsertItems(
                QStringLiteral("s"), {row(QStringLiteral("a")), row(QStringLiteral("b")), row(QStringLiteral("c"))}, true);
            QCOMPARE(library.appendEmbedding(ids[0], vec(1, 2, 3)), 0);
            QCOMPARE(library.appendEmbedding(ids[1], vec(3, 2, 1)), 1);
            QCOMPARE(library.appendEmbedding(ids[2], vec(0, 0, 0)), -1);    // not a direction
            QCOMPARE(library.appendEmbedding(ids[2], std::vector<float>{1, 2}), -1);  // wrong dimension
            QCOMPARE(library.appendEmbedding(9999, vec(1, 1, 1)), -1);      // unknown item
            const auto matrix = library.loadEmbeddings();
            QCOMPARE(matrix.rows(), std::size_t{2});
            cosine = matrix.cosine(0, 1);
        }
        Library library{directory.path()};
        QVERIFY(library.open(nullptr));
        const auto matrix = library.loadEmbeddings();
        QCOMPARE(matrix.rows(), std::size_t{2});
        QCOMPARE(matrix.dim(), std::size_t{3});
        QCOMPARE(matrix.cosine(0, 1), cosine);
        QVERIFY(std::abs(cosine - 10.0F / 14.0F) < 1e-3F);
        QCOMPARE(find(library.items(QStringLiteral("s")), QStringLiteral("b")).embedding_row, 1);
        QCOMPARE(find(library.items(QStringLiteral("s")), QStringLiteral("c")).embedding_row, -1);
    }

    void crashMidAppendKeepsWholeRows() {
        QTemporaryDir directory;
        QList<qint64> ids;
        {
            Library library{directory.path()};
            QVERIFY(library.open(nullptr));
            ids = library.upsertItems(
                QStringLiteral("s"), {row(QStringLiteral("a")), row(QStringLiteral("b")), row(QStringLiteral("c"))}, true);
            for (int index = 0; index < 3; ++index) {
                QCOMPARE(library.appendEmbedding(ids[index], vec(1, static_cast<float>(index), 0)), index);
            }
        }
        // 12-byte header + 3 rows of 3 halves; cut the last row in half.
        QFile file{directory.filePath(QStringLiteral("embeddings.f16"))};
        QCOMPARE(file.size(), qint64{12 + 3 * 6});
        QVERIFY(file.resize(12 + 2 * 6 + 3));

        Library library{directory.path()};
        QString error;
        QVERIFY2(library.open(&error), qPrintable(error));
        QCOMPARE(library.loadEmbeddings().rows(), std::size_t{2});
        const auto items = library.items(QStringLiteral("s"));
        QCOMPARE(find(items, QStringLiteral("b")).embedding_row, 1);
        QCOMPARE(find(items, QStringLiteral("c")).embedding_row, -1);  // re-embedded on resume
        QCOMPARE(library.appendEmbedding(ids[2], vec(0, 1, 0)), 2);
        const auto matrix = library.loadEmbeddings();
        QCOMPARE(matrix.rows(), std::size_t{3});
        QVERIFY(std::abs(matrix.cosine(2, 0) - 0.0F) < 1e-3F);  // row 2 is the new (0,1,0), not the cut one
    }

    void undoWalksBackAndStopsAtTheFirst() {
        QTemporaryDir directory;
        Library library{directory.path()};
        QVERIFY(library.open(nullptr));
        const auto ids = library.upsertItems(
            QStringLiteral("s"), {row(QStringLiteral("a")), row(QStringLiteral("b")), row(QStringLiteral("c"))}, true);
        QVERIFY(!library.undoLast().has_value());

        library.setDecision(ids[0], Decision::Keep, 1);
        library.setDecision(ids[1], Decision::Delete, 2);
        library.setDecision(ids[2], Decision::Keep, 3);
        library.setDecision(ids[1], Decision::Keep, 4);  // changed its mind
        QCOMPARE(library.decisions().value(ids[1]), Decision::Keep);

        using Step = std::pair<qint64, Decision>;
        QCOMPARE(library.undoLast(), std::optional<Step>(Step{ids[1], Decision::Keep}));
        QCOMPARE(library.decisions().value(ids[1]), Decision::Delete);
        QCOMPARE(library.undoLast(), std::optional<Step>(Step{ids[2], Decision::Keep}));
        QVERIFY(!library.decisions().contains(ids[2]));
        QCOMPARE(library.undoLast(), std::optional<Step>(Step{ids[1], Decision::Delete}));
        QCOMPARE(library.undoLast(), std::optional<Step>(Step{ids[0], Decision::Keep}));
        QVERIFY(!library.undoLast().has_value());
        QVERIFY(!library.undoLast().has_value());
        QVERIFY(library.decisions().isEmpty());

        library.setDecision(ids[2], Decision::Delete, 5);
        QCOMPARE(library.decisions().size(), 1);
        QCOMPARE(library.undoLast(), std::optional<Step>(Step{ids[2], Decision::Delete}));
    }

    void sourceCursorRoundTrips() {
        QTemporaryDir directory;
        {
            Library library{directory.path()};
            QVERIFY(library.open(nullptr));
            QCOMPARE(library.sourceCursor(QStringLiteral("bt:phone")), (std::pair<QString, quint64>{QString{}, 0}));
            library.setSourceCursor(QStringLiteral("bt:phone"), QStringLiteral("store-v1"), 41);
            library.setSourceCursor(QStringLiteral("bt:phone"), QStringLiteral("store-v2"), 42);
        }
        Library library{directory.path()};
        QVERIFY(library.open(nullptr));
        QCOMPARE(library.sourceCursor(QStringLiteral("bt:phone")),
                 (std::pair<QString, quint64>{QStringLiteral("store-v2"), 42}));
    }

    void movesTheV1CatalogAside() {
        QTemporaryDir directory;
        QFile v1{directory.filePath(QStringLiteral("catalog.sqlite"))};
        QVERIFY(v1.open(QIODevice::WriteOnly));
        v1.write("v1");
        v1.close();
        Library library{directory.path()};
        QVERIFY(library.open(nullptr));
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("catalog.sqlite"))));
        QVERIFY(QFile::exists(directory.filePath(QStringLiteral("catalog-v1.db.bak"))));
        QVERIFY(QFile::exists(directory.filePath(QStringLiteral("library.db"))));
    }

    void rejectsInvalidRowsAsABatch() {
        QTemporaryDir directory;
        Library library{directory.path()};
        QVERIFY(library.open(nullptr));
        auto bad = row(QStringLiteral("b"));
        bad.bytes = quint64{1} << 63;  // above SQLite's INTEGER range
        QVERIFY(library.upsertItems(QStringLiteral("s"), {row(QStringLiteral("a")), bad}, true).isEmpty());
        QVERIFY(library.upsertItems(QStringLiteral("s"), {row(QString{})}, true).isEmpty());
        QVERIFY(library.upsertItems(QString{}, {row(QStringLiteral("a"))}, true).isEmpty());
        QVERIFY(library.items(QStringLiteral("s")).isEmpty());
    }

    void secondInstanceIsRefusedUntilTheFirstCloses() {
        QTemporaryDir directory;
        auto first = std::make_unique<Library>(directory.path());
        QVERIFY(first->open(nullptr));
        Library second{directory.path()};
        QString error;
        QVERIFY(!second.open(&error));
        QVERIFY2(error.contains(QStringLiteral("another window")), qPrintable(error));
        first.reset();
        QVERIFY2(second.open(&error), qPrintable(error));
    }

    void decisionOnChangedContentIsStale() {
        QTemporaryDir directory;
        Library library{directory.path()};
        QVERIFY(library.open(nullptr));
        const auto source = QStringLiteral("usb:pixel");
        auto a = row(QStringLiteral("a"));
        a.sha256 = QByteArray(32, '\x11');
        const auto ids = library.upsertItems(source, {a, row(QStringLiteral("b"))}, true);
        library.setDecision(ids[0], Decision::Delete, 1);
        library.setDecision(ids[1], Decision::Delete, 2);

        library.upsertItems(source, {a, row(QStringLiteral("b"))}, true);  // nothing changed
        QCOMPARE(library.decisions().value(ids[0]), Decision::Delete);
        QVERIFY(library.staleDecisions().isEmpty());

        auto edited = a;
        edited.sha256 = QByteArray(32, '\x22');  // same size and time, other content
        library.upsertItems(source, {edited, row(QStringLiteral("b"), 2000)}, true);
        QVERIFY(!library.decisions().contains(ids[0]));
        QVERIFY(!library.decisions().contains(ids[1]));
        QCOMPARE(library.staleDecisions(), (QList<qint64>{ids[0], ids[1]}));

        library.setDecision(ids[0], Decision::Keep, 3);  // reviewed again
        QCOMPARE(library.decisions().value(ids[0]), Decision::Keep);
        QCOMPARE(library.staleDecisions(), QList<qint64>{ids[1]});
    }

    void undoStaysInTheSessionAndOnPresentItems() {
        QTemporaryDir directory;
        QList<qint64> ids;
        {
            Library library{directory.path()};
            QVERIFY(library.open(nullptr));
            ids = library.upsertItems(QStringLiteral("s"), {row(QStringLiteral("a")), row(QStringLiteral("b"))}, true);
            library.setDecision(ids[0], Decision::Keep, 1);
        }
        Library library{directory.path()};
        QVERIFY(library.open(nullptr));
        QVERIFY(!library.undoLast().has_value());  // made in an earlier session
        QCOMPARE(library.decisions().value(ids[0]), Decision::Keep);

        library.setDecision(ids[0], Decision::Delete, 2);
        library.setDecision(ids[1], Decision::Delete, 3);
        library.upsertItems(QStringLiteral("s"), {row(QStringLiteral("a"))}, true);  // b is gone from the phone
        using Step = std::pair<qint64, Decision>;
        QCOMPARE(library.undoLast(), std::optional<Step>(Step{ids[0], Decision::Delete}));  // skips b
        QVERIFY(!library.undoLast().has_value());
        QCOMPARE(library.decisions().value(ids[0]), Decision::Keep);
    }

    void damagedRowsInTheLastBatchAreRequeuedOnOpen() {
        QTemporaryDir directory;
        QList<qint64> ids;
        {
            Library library{directory.path()};
            QVERIFY(library.open(nullptr));
            ids = library.upsertItems(
                QStringLiteral("s"), {row(QStringLiteral("a")), row(QStringLiteral("b")), row(QStringLiteral("c"))}, true);
            for (int index = 0; index < 3; ++index) {
                QCOMPARE(library.appendEmbedding(ids[index], vec(1, static_cast<float>(index), 0.5F)), index);
            }
        }
        const auto path = directory.filePath(QStringLiteral("embeddings.f16"));
        write_halves(path, 12 + 1 * 6, {0x7C00, 0x3C00, 0x3C00});  // row 1: +inf
        write_halves(path, 12 + 2 * 6, {0, 0, 0});                 // row 2: never reached the disk
        Library library{directory.path()};
        QVERIFY(library.open(nullptr));
        const auto items = library.items(QStringLiteral("s"));
        QCOMPARE(find(items, QStringLiteral("a")).embedding_row, 0);
        QCOMPARE(find(items, QStringLiteral("b")).embedding_row, -1);
        QCOMPARE(find(items, QStringLiteral("c")).embedding_row, -1);
    }

    void aNonFiniteOlderRowRequeuesOnlyItsItem() {
        QTemporaryDir directory;
        constexpr int count = 300;  // more than the 256 rows open() checks
        QList<ItemRow> rows;
        for (int index = 0; index < count; ++index) {
            rows.push_back(row(QStringLiteral("i%1").arg(index)));
        }
        QList<qint64> ids;
        {
            Library library{directory.path()};
            QVERIFY(library.open(nullptr));
            ids = library.upsertItems(QStringLiteral("s"), rows, true);
            for (int index = 0; index < count; ++index) {
                QCOMPARE(library.appendEmbedding(ids[index], vec(1, static_cast<float>(index), 0)), index);
            }
        }
        write_halves(directory.filePath(QStringLiteral("embeddings.f16")), 12 + 10 * 6, {0x7E00, 0, 0});  // row 10: NaN
        Library library{directory.path()};
        QVERIFY(library.open(nullptr));
        QCOMPARE(find(library.items(QStringLiteral("s")), QStringLiteral("i10")).embedding_row, 10);  // not checked yet
        const auto matrix = library.loadEmbeddings();
        QCOMPARE(matrix.rows(), std::size_t{count});
        const auto items = library.items(QStringLiteral("s"));
        QCOMPARE(find(items, QStringLiteral("i10")).embedding_row, -1);
        QCOMPARE(find(items, QStringLiteral("i9")).embedding_row, 9);
        QCOMPARE(find(items, QStringLiteral("i11")).embedding_row, 11);
        QVERIFY(std::abs(matrix.cosine(9, 11) - matrix.cosine(11, 9)) < 1e-6F);
        QVERIFY(matrix.cosine(9, 11) > 0.9F);
    }
};

QTEST_MAIN(LibraryTests)
#include "library_tests.moc"
