#pragma once

#include <QString>

class QFileDevice;

namespace pms::desktop {

// Flushes Qt's buffer and asks the OS to put the file's data on disk (FlushFileBuffers / fsync).
[[nodiscard]] bool sync_file(QFileDevice& file);
// Makes a rename or a new file in `directory` durable (fsync of the directory on POSIX; a no-op on Windows,
// where NTFS journals the rename itself).
[[nodiscard]] bool sync_directory(const QString& directory);

}  // namespace pms::desktop
