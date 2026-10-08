#include "platform/durable.hpp"

#include <QFileDevice>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <io.h>
#else
#include <QFile>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pms::desktop {

bool sync_file(QFileDevice& file) {
    if (!file.isOpen() || !file.flush()) {
        return false;
    }
#ifdef Q_OS_WIN
    return FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()))) != FALSE;
#else
    return ::fsync(file.handle()) == 0;
#endif
}

bool sync_directory(const QString& directory) {
#ifdef Q_OS_WIN
    Q_UNUSED(directory);
    return true;
#else
    const auto fd = ::open(QFile::encodeName(directory).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const auto synced = ::fsync(fd) == 0;
    ::close(fd);
    return synced;
#endif
}

}  // namespace pms::desktop
