#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

class QCryptographicHash;
class QIODevice;

namespace pms::desktop {

struct VaultEntry {
    QString id; QString source_key; QString source_id; QString original_name;
    quint64 bytes{}; QByteArray sha256; qint64 archived_ms{};   // sha256: 32 raw bytes of the original file
};

// free_bytes >= bytes * 1.1, computed without overflow.
[[nodiscard]] bool has_room(quint64 free_bytes, quint64 bytes) noexcept;

// The recovery vault: every file the app removes is first copied here, encrypted, and checked by decrypting it.
// Files in `directory` (0700 on macOS/Linux, files 0600):
//   <id>.pmsv      magic "PMSVAULT" | u32 version 2 | u64 size | 12-byte nonce | 16-byte tag | ciphertext
//   manifest.pmsm  the same layout with magic "PMSMANIF"; the plaintext is the JSON list of entries
// AES-256-GCM, a fresh random nonce per file, AAD = the first 32 bytes (magic..nonce) || entry id (or "manifest"),
// so a changed header, a swapped payload or a changed manifest fails authentication. The key comes from
// KeyStore ("vault"). Plaintext is never written inside the vault: verification decrypts into a hash sink.
// Not thread-safe; use one instance from one thread.
class Vault final {
public:
    explicit Vault(QString directory);
    ~Vault();
    Vault(const Vault&) = delete;
    Vault& operator=(const Vault&) = delete;

    // Fails if the manifest does not authenticate; nothing in the vault is trusted or changed then.
    [[nodiscard]] bool open(QString* error);
    // Streams the file in, encrypts with AES-256-GCM using AAD = header || entry id, verifies by decrypting into a hash sink,
    // appends an entry to the manifest. The manifest is one AEAD-sealed blob rewritten atomically (QSaveFile).
    [[nodiscard]] std::optional<VaultEntry> archive(const QString& file, const QString& source_key, const QString& source_id,
                                                    const QString& name, QString* error);
    // Writes the original bytes to destination_dir/<original file name>, adding " 2", " 3"... before the extension
    // instead of overwriting. The name is reduced to a plain file name, so it cannot leave destination_dir.
    // The file only appears once the tag, size and SHA-256 all match; the entry stays in the vault.
    [[nodiscard]] bool restore(const QString& entry_id, const QString& destination_dir, QString* restored_path, QString* error);
    [[nodiscard]] QList<VaultEntry> entries() const;
    [[nodiscard]] quint64 bytesUsed() const;            // payload files on disk
    int purgeOlderThan(qint64 cutoff_ms);           // returns count removed
    [[nodiscard]] bool hasRoomFor(quint64 bytes) const;   // free space >= bytes * 1.1

private:
    [[nodiscard]] QString payloadPath(const QString& id) const;
    [[nodiscard]] bool writeManifest(const QList<VaultEntry>& entries, QString& error) const;
    [[nodiscard]] bool decryptPayload(const VaultEntry& entry, QIODevice* destination, QString& error) const;

    QString directory_;
    QByteArray key_;   // empty until open() succeeds
    QList<VaultEntry> entries_;
};

namespace detail {
inline constexpr qint64 gcm_chunk_bytes = 4 * 1024 * 1024;
// AES-256-GCM over a stream: BCrypt on Windows, OpenSSL EVP elsewhere. `aad` is authenticated before the data.
// Encrypting writes `tag`; decrypting checks it and returns false on any mismatch (output written before that
// point is unauthenticated, so callers send it to a hash sink or a QSaveFile they cancel). chunk_bytes must be a
// multiple of 16. Exposed for the known-answer test.
[[nodiscard]] bool aes_gcm_stream(QIODevice& input, QIODevice& output, const QByteArray& key, const QByteArray& nonce,
                                  const QByteArray& aad, QByteArray& tag, bool encrypt,
                                  qint64 chunk_bytes = gcm_chunk_bytes, QCryptographicHash* input_hash = nullptr);
}  // namespace detail

}  // namespace pms::desktop
