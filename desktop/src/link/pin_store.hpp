#pragma once

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <map>
#include <memory>
#include <optional>

#include "pms/link/session.hpp"

namespace pms::desktop {

// One small blob that only this user can read back.
class SecretVault {
public:
    virtual ~SecretVault() = default;
    // nullopt: nothing stored yet. Throws std::runtime_error when something is stored but cannot be read.
    [[nodiscard]] virtual std::optional<QByteArray> read() = 0;
    virtual void write(const QByteArray& blob) = 0;  // throws std::runtime_error
};

// Windows: a DPAPI-protected file in directory. macOS: a Keychain item. Linux: a libsecret item, or, when the build has
// no libsecret or no secret service answers, a 0600 file in directory (logged as a warning).
[[nodiscard]] std::unique_ptr<SecretVault> platform_vault(const QString& directory);
#ifdef Q_OS_WIN
// CryptProtectData for the current user, CRYPTPROTECT_UI_FORBIDDEN.
[[nodiscard]] std::unique_ptr<SecretVault> dpapi_file_vault(const QString& path);
#endif
// The fallback: a plain file readable and writable by its owner only (0600).
[[nodiscard]] std::unique_ptr<SecretVault> owner_only_file_vault(const QString& path);

// The desktop's pinned phone keys (PROTOCOL.md section 4) in a SecretVault. It loads once; each change is written to the
// vault before it takes effect, and a failed write throws and changes nothing. Unreadable or malformed contents give an
// empty store and a warning, never a crash: the phones pair again by QR code.
class VaultPinStore final : public pms::link::PinStore {
public:
    explicit VaultPinStore(std::unique_ptr<SecretVault> vault);

    [[nodiscard]] std::optional<pms::link::Pinned> find(const pms::link::Key32& phone_key) const override;
    void pin(const pms::link::Pinned& p) override;
    void unpin(const pms::link::Key32& phone_key) override;
    [[nodiscard]] std::size_t size() const noexcept { return pins_.size(); }

private:
    void save(const std::map<pms::link::Key32, pms::link::Pinned>& pins);

    std::unique_ptr<SecretVault> vault_;
    std::map<pms::link::Key32, pms::link::Pinned> pins_;
};

}  // namespace pms::desktop
