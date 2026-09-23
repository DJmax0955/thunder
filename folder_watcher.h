#pragma once

#include <QObject>
#include <QFileSystemWatcher>
#include <QString>
#include <qqmlintegration.h>

// Watches a folder (and every file directly inside it) for changes --
// files added, removed, or modified in place (e.g. re-saved from an
// external image editor) -- and emits changed() so QML can refresh.
//
// Watches individual files too, not just the directory, since on most
// platforms a directory-level watch alone only reliably reports
// add/remove, not in-place content modifications to existing files.
class FolderWatcher : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)

public:
    explicit FolderWatcher(QObject *parent = nullptr);

    QString path() const;
    void setPath(const QString &path);

signals:
    void pathChanged();
    // Emitted whenever the folder's contents change in any way: a file
    // added, removed, or modified in place.
    void changed();

private slots:
    void onDirectoryChanged(const QString &dir);
    void onFileChanged(const QString &file);

private:
    void rescan();

    QFileSystemWatcher m_watcher;
    QString m_path;
};
