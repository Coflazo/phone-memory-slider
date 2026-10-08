#pragma once

#include <QByteArray>
#include <QList>
#include <QLockFile>
#include <QString>

#include <memory>
#include <optional>

class QCryptographicHash;
class QIODevice;

namespace pms::desktop {

struct VaultEntry {
    QString id; QString source_key; QString source_id; QString original_name;
    quint64 bytes{}; QByteArray sha256; qint64 archived_ms{};   // sha256: 32 raw bytes of the original file
};

struct ArchiveRequest { QString file; QString source_key; QString source_id; QString name; };
struct ArchiveResult { std::optional<VaultEntry> entry; QString error; };   // exactly one of the two is set

// free_bytes >= bytes * 1.1, computed without overflow.
[[nodiscard]] bool has_room(quint64 free_bytes, quint64 bytes) noexcept;

// The recovery vault: every file the app removes is first copied here, encrypted, and checked by decrypting it.
// Files in `directory` (0700 on macOS/Linux, files 0600 before they get their final name):
//   <id>.pmsv          magic "PMSVAULT" | u32 version 2 | u64 size | 12-byte nonce | 16-byte tag | ciphertext;
//                      AAD = the first 32 bytes (magic..nonce) || entry id
//   journal.pmsj       the index: header "PMSJRNL2" | u32 version 2 | 16 random bytes, then one sealed record per
//                      entry: u32 ciphertext length | u8 36 | entry id | nonce | tag | ciphertext (the entry as JSON);
//                      AAD = journal header || u64 record index || entry id. Appended and fsync'd per batch; rewritten
//                      whole (QSaveFile) only by purge and recovery, which keep the one before as journal.pmsj.prev.
//   vault.lock         held while a Vault is open: a second window gets "open in another window".
// AES-256-GCM with a fresh random nonce per payload and per record; the key comes from KeyStore ("vault").
// A changed header byte, a swapped payload, a changed, reordered or cut journal record all fail authentication.
// Plaintext is never written inside the vault: verification decrypts into a hash sink.
// Not thread-safe; use one instance from one thread.
class Vault final {
public:
    explicit Vault(QString directory);
    ~Vault();
    Vault(const Vault&) = delete;
    Vault& operator=(const Vault&) = delete;

    // Fails if the folder is locked by another instance or the key is unavailable. Also fails, with
    // needsRecovery() true, when the journal is missing while encrypted files exist or a journal record does not
    // authenticate; the lock and key stay held so recoverIndex() can run. On success, unindexed payloads and
    // leftover temporary files older than 24 h are removed.
    [[nodiscard]] bool open(QString* error);
    [[nodiscard]] bool needsRecovery() const noexcept { return needs_recovery_; }
    // Keeps the journal's records up to the first bad one and indexes every other payload that authenticates
    // (its AAD binds its id), named as in journal.pmsj.prev when that copy matches, else "restored-<id>".
    // Then the journal is rewritten and the vault is open.
    [[nodiscard]] bool recoverIndex(int* recovered, QString* error);

    // Streams the file in, encrypts with AES-256-GCM using AAD = header || entry id, verifies by decrypting into a hash sink,
    // appends an entry to the journal.
    [[nodiscard]] std::optional<VaultEntry> archive(const QString& file, const QString& source_key, const QString& source_id,
                                                    const QString& name, QString* error);
    // archive() for many files: each is encrypted and verified, then all that passed go into the journal in one
    // append and one fsync. One result per request, in order; a failed journal write fails every item in it.
    [[nodiscard]] QList<ArchiveResult> archiveBatch(const QList<ArchiveRequest>& requests);
    // Writes the original bytes to destination_dir/<original file name>, adding " 2", " 3"... before the extension.
    // The name is reserved with an exclusive create, so an existing file is never replaced, and it is reduced to a
    // plain file name, so it cannot leave destination_dir. The file only gets its content once tag, size and
    // SHA-256 all match; the error says whether the archived copy failed its check or the destination could not
    // be written (a full disk, for example). The entry stays in the vault.
    [[nodiscard]] bool restore(const QString& entry_id, const QString& destination_dir, QString* restored_path, QString* error);
    [[nodiscard]] QList<VaultEntry> entries() const;
    [[nodiscard]] quint64 bytesUsed() const;            // payload files on disk
    int purgeOlderThan(qint64 cutoff_ms);           // returns count removed; a cutoff in the future means now
    [[nodiscard]] bool hasRoomFor(quint64 bytes) const;   // free space >= bytes * 1.1

private:
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] QString payloadPath(const QString& id) const;
    [[nodiscard]] QString journalPath() const;
    [[nodiscard]] ArchiveResult sealPayload(const ArchiveRequest& request) const;
    [[nodiscard]] bool appendJournal(const QList<VaultEntry>& entries, QString& error);
    [[nodiscard]] bool compact(const QList<VaultEntry>& entries, QString& error);
    [[nodiscard]] bool decryptPayload(const VaultEntry& entry, QIODevice* destination, QString& error) const;
    void sweepOrphans() const;

    QString directory_;
    std::unique_ptr<QLockFile> lock_;
    QByteArray key_;   // empty until open() got the key
    QList<VaultEntry> entries_;
    QByteArray journal_header_;
    quint64 journal_records_{};
    bool needs_recovery_{};
};

namespace detail {
inline constexpr qint64 gcm_chunk_bytes = 4 * 1024 * 1024;
// AES-256-GCM over a stream: BCrypt on Windows, OpenSSL EVP elsewhere. `aad` is authenticated before the data.
// Encrypting writes `tag`; decrypting checks it and returns false on any mismatch (output written before that
// point is unauthenticated, so callers send it to a hash sink or a file they discard). chunk_bytes must be a
// multiple of 16. Exposed for the known-answer test.
[[nodiscard]] bool aes_gcm_stream(QIODevice& input, QIODevice& output, const QByteArray& key, const QByteArray& nonce,
                                  const QByteArray& aad, QByteArray& tag, bool encrypt,
                                  qint64 chunk_bytes = gcm_chunk_bytes, QCryptographicHash* input_hash = nullptr);
// Writes a complete journal for `entries` (QSaveFile, fresh header). Used by compaction; exposed for tests.
[[nodiscard]] bool write_journal(const QString& path, const QByteArray& key, const QList<VaultEntry>& entries,
                                 QString& error, QByteArray* header = nullptr);
}  // namespace detail

}  // namespace pms::desktop
