/*
 * Copyright (C) by Dominik Schmidt <dev@dominik-schmidt.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */


#ifndef SOCKETAPI_H
#define SOCKETAPI_H

extern "C" {
#include <std/c_string.h>
}

#include <sqlite3.h>

#include <QWeakPointer>
#include <QTcpSocket>
#include <QTcpServer>
#include <QLocalServer>

#include "syncfileitem.h"
#include "syncjournalfilerecord.h"
#include "ownsql.h"

class QUrl;
class QLocalSocket;
class QStringList;

namespace OCC {

typedef QLocalSocket SocketType;

class SyncFileStatus;
class Folder;

class SocketApi : public QObject
{
Q_OBJECT

public:
    SocketApi(QObject* parent);
    virtual ~SocketApi();

public slots:
    void slotUpdateFolderView(const QString&);
    void slotUnregisterPath( const QString& alias );
    void slotRegisterPath( const QString& alias );
    void slotReadExcludes();
    void slotClearExcludesList();

signals:
    void shareCommandReceived(const QString &sharePath, const QString &localPath, bool resharingAllowed);

private slots:
    void slotNewConnection();
    void onLostConnection();
    void slotReadSocket();
    void slotJobCompleted(const QString &, const SyncFileItem &);
    void slotSyncItemDiscovered(const QString &, const SyncFileItem &);

private:
    SyncFileStatus fileStatus(Folder *folder, const QString& systemFileName, c_strlist_t *excludes );
    SyncJournalFileRecord dbFileRecord_capi( Folder *folder, QString fileName );
    SqlQuery *getSqlQuery( Folder *folder );

    void sendMessage(SocketType* socket, const QString& message, bool doWait = false);
    void broadcastMessage(const QString& verb, const QString &path, const QString &status = QString::null, bool doWait = false);

    Q_INVOKABLE void command_RETRIEVE_FOLDER_STATUS(const QString& argument, SocketType* socket);
    Q_INVOKABLE void command_RETRIEVE_FILE_STATUS(const QString& argument, SocketType* socket);
    Q_INVOKABLE void command_SHARE(const QString& localFile, SocketType* socket);

    Q_INVOKABLE void command_VERSION(const QString& argument, SocketType* socket);

    Q_INVOKABLE void command_SHARE_MENU_TITLE(const QString& argument, SocketType* socket);
    QString buildRegisterPathMessage(const QString& path);

#ifdef SOCKETAPI_TCP
    QTcpServer _localServer;
#else
    QLocalServer _localServer;
#endif
    QList<SocketType*> _listeners;
    c_strlist_t *_excludes;
    QHash<Folder*, SqlQuery*> _dbQueries;
    QHash<Folder*, SqlDatabase*> _openDbs;
};

}
#endif // SOCKETAPI_H
