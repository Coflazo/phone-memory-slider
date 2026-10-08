#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace pms::desktop {

enum class KeyBackend : quint8 { None, Dpapi, Keychain, SecretService, File };

// A 32-byte random key per name, created on first use and never leaving this user account in the clear.
// Windows: DPAPI-protected file (<AppLocalData>/keys/<name>.dpapi). macOS: a generic-password Keychain item.
// Linux: a Secret Service item through libsecret. When no system store is reachable at all (a headless session,
// a Mac account without a keychain), a 0600 file (<AppLocalData>/keys/<name>.key) in a 0700 folder, logged and
// reported by backend(). Once a system store has held the key (<name>.system-store marks it), there is never a
// file fallback: an unreachable or locked store is an error, never a second key. New keys are written to a
// temporary file and renamed without replacing, so two first launches end up with the same key.
// `name` is 1-64 of [A-Za-z0-9_-]. QStandardPaths test mode keeps test keys apart from the app's.
class KeyStore {
public:
    [[nodiscard]] static std::optional<QByteArray> loadOrCreate(const QString& name, QString* error);
    // Where the last successful loadOrCreate() found or put its key; Settings shows File as "no system key store".
    [[nodiscard]] static KeyBackend backend() noexcept;
};

}  // namespace pms::desktop
