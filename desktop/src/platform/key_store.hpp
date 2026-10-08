#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace pms::desktop {

// A 32-byte random key per name, created on first use and never leaving this user account in the clear.
// Windows: DPAPI-protected file (<AppLocalData>/keys/<name>.dpapi). macOS: a generic-password Keychain item
// (service "Phone Memory Slider"). Elsewhere, or on a Mac with no keychain at all: a 0600 file
// (<AppLocalData>/keys/<name>.key) in a 0700 folder, with a logged warning. A key is only created when none
// exists; a store that exists but cannot be read (locked keychain, DPAPI failure) is an error, never a new key.
// `name` is 1-64 of [A-Za-z0-9_-]. QStandardPaths test mode keeps test keys apart from the app's.
class KeyStore {
public:
    [[nodiscard]] static std::optional<QByteArray> loadOrCreate(const QString& name, QString* error);
};

}  // namespace pms::desktop
