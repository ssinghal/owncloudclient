/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include <QtCore>
#include <QNetworkReply>
#include <QNetworkProxyFactory>

#include "connectionvalidator.h"
#include "theme.h"
#include "account.h"
#include "networkjobs.h"
#include "clientproxy.h"
#include <creds/abstractcredentials.h>

namespace OCC {

ConnectionValidator::ConnectionValidator(AccountPtr account, QObject *parent)
    : QObject(parent),
      _account(account),
      _isCheckingServerAndAuth(false)
{
}

QString ConnectionValidator::statusString( Status stat )
{
    switch( stat ) {
    case Undefined:
        return QLatin1String("Undefined");
    case Connected:
        return QLatin1String("Connected");
    case NotConfigured:
        return QLatin1String("NotConfigured");
    case ServerVersionMismatch:
        return QLatin1String("Server Version Mismatch");
    case CredentialsWrong:
        return QLatin1String("Credentials Wrong");
    case StatusNotFound:
        return QLatin1String("Status not found");
    case UserCanceledCredentials:
        return QLatin1String("User canceled credentials");
    case ServerMaintenance:
        return QLatin1String("Server in maintenance mode");
    case Timeout:
        return QLatin1String("Timeout");
    }
    return QLatin1String("status undeclared.");
}

void ConnectionValidator::checkServerAndAuth()
{
    if( !_account ) {
        _errors << tr("No ownCloud account configured");
        reportResult( NotConfigured );
        return;
    }
    _isCheckingServerAndAuth = true;

    // Lookup system proxy in a thread https://github.com/owncloud/client/issues/2993
    if (ClientProxy::isUsingSystemDefault()) {
        qDebug() << "Trying to look up system proxy";
        ClientProxy::lookupSystemProxyAsync(_account->url(),
                                            this, SLOT(systemProxyLookupDone(QNetworkProxy)));
    } else {
        // We want to reset the QNAM proxy so that the global proxy settings are used (via ClientProxy settings)
        _account->networkAccessManager()->setProxy(QNetworkProxy(QNetworkProxy::DefaultProxy));
        // use a queued invocation so we're as asynchronous as with the other code path
        QMetaObject::invokeMethod(this, "slotCheckServerAndAuth", Qt::QueuedConnection);
    }
}

void ConnectionValidator::systemProxyLookupDone(const QNetworkProxy &proxy) {
    if (!_account) {
        qDebug() << "Bailing out, Account had been deleted";
        return;
    }

    if (proxy.type() != QNetworkProxy::DefaultProxy) {
        qDebug() << Q_FUNC_INFO << "Setting QNAM proxy to be system proxy" << printQNetworkProxy(proxy);
        _account->networkAccessManager()->setProxy(proxy);
    }

    slotCheckServerAndAuth();
}

// The actual check
void ConnectionValidator::slotCheckServerAndAuth()
{
    CheckServerJob *checkJob = new CheckServerJob(_account, this);
    checkJob->setIgnoreCredentialFailure(true);
    connect(checkJob, SIGNAL(instanceFound(QUrl,QVariantMap)), SLOT(slotStatusFound(QUrl,QVariantMap)));
    connect(checkJob, SIGNAL(networkError(QNetworkReply*)), SLOT(slotNoStatusFound(QNetworkReply*)));
    connect(checkJob, SIGNAL(timeout(QUrl)), SLOT(slotJobTimeout(QUrl)));
    checkJob->start();
}

void ConnectionValidator::slotStatusFound(const QUrl&url, const QVariantMap &info)
{
    // status.php was found.
    qDebug() << "** Application: ownCloud found: "
             << url << " with version "
             << CheckServerJob::versionString(info)
             << "(" << CheckServerJob::version(info) << ")";

    QString version = CheckServerJob::version(info);
    _account->setServerVersion(version);

    if (version.contains('.') && version.split('.')[0].toInt() < 5) {
        _errors.append( tr("The configured server for this client is too old") );
        _errors.append( tr("Please update to the latest server and restart the client.") );
        reportResult( ServerVersionMismatch );
        return;
    }

    // now check the authentication
    AbstractCredentials *creds = _account->credentials();
    if (creds->ready()) {
        QTimer::singleShot( 0, this, SLOT( checkAuthentication() ));
    } else {
        // We can't proceed with the auth check because we don't have credentials.
        // Fetch them now! Once fetched, a new connectivity check will be
        // initiated anyway.
        creds->fetch();

        // no result is reported
        deleteLater();
    }
}

// status.php could not be loaded (network or server issue!).
void ConnectionValidator::slotNoStatusFound(QNetworkReply *reply)
{
    if( reply && ! _account->credentials()->stillValid(reply)) {
        _errors.append(tr("Authentication error: Either username or password are wrong."));
    }  else {
        _errors.append(tr("Unable to connect to %1").arg(_account->url().toString()));
        _errors.append( reply->errorString() );
    }
    reportResult( StatusNotFound );
}

void ConnectionValidator::slotJobTimeout(const QUrl &url)
{
    _errors.append(tr("Unable to connect to %1").arg(url.toString()));
    _errors.append(tr("timeout"));
    reportResult( Timeout );
}


void ConnectionValidator::checkAuthentication()
{
    AbstractCredentials *creds = _account->credentials();

    if (!creds->ready()) { // The user canceled
        reportResult(UserCanceledCredentials);
    }

    // simply GET the webdav root, will fail if credentials are wrong.
    // continue in slotAuthCheck here :-)
    qDebug() << "# Check whether authenticated propfind works.";
    PropfindJob *job = new PropfindJob(_account, "/", this);
    job->setProperties(QList<QByteArray>() << "getlastmodified");
    connect(job, SIGNAL(result(QVariantMap)), SLOT(slotAuthSuccess()));
    connect(job, SIGNAL(networkError(QNetworkReply*)), SLOT(slotAuthFailed(QNetworkReply*)));
    job->start();
}

void ConnectionValidator::slotAuthFailed(QNetworkReply *reply)
{
    Status stat = Timeout;

    if( reply->error() == QNetworkReply::AuthenticationRequiredError ||
             !_account->credentials()->stillValid(reply)) {
        qDebug() <<  reply->error() << reply->errorString();
        qDebug() << "******** Password is wrong!";
        _errors << tr("The provided credentials are not correct");
        stat = CredentialsWrong;

    } else if( reply->error() != QNetworkReply::NoError ) {
        _errors << reply->errorString();

        const int httpStatus =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if ( httpStatus == 503 ) {
            // Is this a maintenance mode reply from the server
            // or a regular 503 from somewhere else?
            QByteArray body = reply->readAll();
            if ( body.contains("Sabre\\DAV\\Exception\\ServiceUnavailable") ) {
                _errors.clear();
                stat = ServerMaintenance;
            }
        }
    }

    reportResult( stat );
}

void ConnectionValidator::slotAuthSuccess()
{
    _errors.clear();
    if (!_isCheckingServerAndAuth) {
        reportResult(Connected);
        return;
    }
    checkServerCapabilities();
}

void ConnectionValidator::checkServerCapabilities()
{
    JsonApiJob *job = new JsonApiJob(_account, QLatin1String("ocs/v1.php/cloud/capabilities"), this);
    QObject::connect(job, SIGNAL(jsonRecieved(QVariantMap)), this, SLOT(slotCapabilitiesRecieved(QVariantMap)));
    job->start();
}

void ConnectionValidator::slotCapabilitiesRecieved(const QVariantMap &json)
{
    auto caps = json.value("ocs").toMap().value("data").toMap().value("capabilities");
    qDebug() << "Server capabilities" << caps;
    _account->setCapabilities(caps.toMap());
    reportResult(Connected);
    return;
}


void ConnectionValidator::reportResult(Status status)
{
    emit connectionResult(status, _errors);
    deleteLater();
}

} // namespace OCC
