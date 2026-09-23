#include "folder_watcher.h"
#include <QDir>
#include <QFileInfo>

FolderWatcher::FolderWatcher(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &FolderWatcher::onDirectoryChanged);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &FolderWatcher::onFileChanged);
}

QString FolderWatcher::path() const
{
    return m_path;
}

void FolderWatcher::setPath(const QString &path)
{
    if (m_path == path)
        return;
    m_path = path;
    emit pathChanged();
    rescan();
}

void FolderWatcher::onDirectoryChanged(const QString &dir)
{
    Q_UNUSED(dir);
    rescan();
    emit changed();
}

void FolderWatcher::onFileChanged(const QString &file)
{
    Q_UNUSED(file);
    rescan();
    emit changed();
}

void FolderWatcher::rescan()
{
    const QStringList currentFiles = m_watcher.files();
    if (!currentFiles.isEmpty())
        m_watcher.removePaths(currentFiles);
    const QStringList currentDirs = m_watcher.directories();
    if (!currentDirs.isEmpty())
        m_watcher.removePaths(currentDirs);

    if (m_path.isEmpty())
        return;

    QDir dir(m_path);
    if (!dir.exists())
        return;

    m_watcher.addPath(m_path);
    const auto entries = dir.entryInfoList(QDir::Files, QDir::Name);
    for (const auto &info : entries)
        m_watcher.addPath(info.absoluteFilePath());
}
