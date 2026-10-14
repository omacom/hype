#pragma once
#include <QFile>
#include <QThread>
#include <QtGlobal>

// Sizes parallel work to this machine: a big workstation uses its cores, while a small
// laptop stays inside the memory it has free rather than swapping or being killed.
namespace budget {

// Memory the kernel could hand out right now without swapping, or -1 when unknown. A
// container or systemd slice with its own limit counts only the room left inside it.
inline qint64 availableBytes() {
    qint64 free = -1;
    // Files under /proc report a size of zero, so QFile::atEnd() is true before the first
    // read; read them whole instead of line by line.
    QFile meminfo("/proc/meminfo");
    if (meminfo.open(QIODevice::ReadOnly))
        for (const QByteArray &line : meminfo.readAll().split('\n'))
            if (line.startsWith("MemAvailable:"))
                free = line.mid(13).trimmed().split(' ').first().toLongLong() * 1024;
    // cgroup v2: the unified hierarchy's entry reads "0::/path/to/group".
    QFile cgroup("/proc/self/cgroup");
    if (cgroup.open(QIODevice::ReadOnly)) {
        QByteArray group;
        for (const QByteArray &line : cgroup.readAll().split('\n'))
            if (line.startsWith("0::"))
                group = line.mid(3).trimmed();
        // Limits nest, so the tightest one on the way up to the root applies.
        for (QByteArray path = group; !path.isEmpty(); path = path.left(qMax(0, int(path.lastIndexOf('/'))))) {
            QFile limit("/sys/fs/cgroup" + path + "/memory.max"), used("/sys/fs/cgroup" + path + "/memory.current");
            if (limit.open(QIODevice::ReadOnly) && used.open(QIODevice::ReadOnly)) {
                bool ok = false;
                const qint64 max = limit.readAll().trimmed().toLongLong(&ok); // "max" means unlimited.
                if (ok) {
                    const qint64 room = qMax<qint64>(0, max - used.readAll().trimmed().toLongLong());
                    free = free < 0 ? room : qMin(free, room);
                }
            }
            if (path == "/")
                break;
        }
    }
    return free;
}

// The arithmetic behind workers(), kept pure so it can be tested for any machine.
inline int fit(int cores, qint64 free, qint64 perWorker, int cap, double share, qint64 overhead, int minimum) {
    int count = qBound(minimum, cores, cap);
    if (free > 0)
        count = int(qMin<qint64>(count, qMax<qint64>(minimum, qint64(free * share - overhead) / perWorker)));
    return count;
}

// How many workers to run when each costs `perWorker` bytes on top of a fixed `overhead`,
// using at most `share` of the free memory and never more than `cap` or the core count.
inline int workers(qint64 perWorker, int cap, double share, qint64 overhead = 0, int minimum = 1) {
    return fit(QThread::idealThreadCount(), availableBytes(), perWorker, cap, share, overhead, minimum);
}

} // namespace budget
