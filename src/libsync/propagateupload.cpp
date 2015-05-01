/*
 * Copyright (C) by Olivier Goffart <ogoffart@owncloud.com>
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

#include "propagateupload.h"
#include "owncloudpropagator_p.h"
#include "networkjobs.h"
#include "account.h"
#include "syncjournaldb.h"
#include "syncjournalfilerecord.h"
#include "utility.h"
#include "filesystem.h"
#include "propagatorjobs.h"
#include <json.h>
#include <QNetworkAccessManager>
#include <QFileInfo>
#include <QDir>
#include <cmath>
#include <cstring>

#ifdef USE_NEON
#include "propagator_legacy.h"
#endif

namespace OCC {

/**
 * We do not want to upload files that are currently being modified.
 * To avoid that, we don't upload files that have a modification time
 * that is too close to the current time.
 *
 * This interacts with the msBetweenRequestAndSync delay in the folder
 * manager. If that delay between file-change notification and sync
 * has passed, we should accept the file for upload here.
 */
static bool fileIsStillChanging(const SyncFileItem & item)
{
    const QDateTime modtime = Utility::qDateTimeFromTime_t(item._modtime);
    const qint64 msSinceMod = modtime.msecsTo(QDateTime::currentDateTime());

    return msSinceMod < 2000
            // if the mtime is too much in the future we *do* upload the file
            && msSinceMod > -10000;
}

static qint64 chunkSize() {
    static uint chunkSize;
    if (!chunkSize) {
        chunkSize = qgetenv("OWNCLOUD_CHUNK_SIZE").toUInt();
        if (chunkSize == 0) {
            chunkSize = 5*1024*1024; // default to 5 MiB
        }
    }
    return chunkSize;
}

PUTFileJob::~PUTFileJob()
{
    // Make sure that we destroy the QNetworkReply before our _device of which it keeps an internal pointer.
    setReply(0);
}

void PUTFileJob::start() {
    QNetworkRequest req;
    for(QMap<QByteArray, QByteArray>::const_iterator it = _headers.begin(); it != _headers.end(); ++it) {
        req.setRawHeader(it.key(), it.value());
    }

    setReply(davRequest("PUT", path(), req, _device.data()));
    setupConnections(reply());

    if( reply()->error() != QNetworkReply::NoError ) {
        qWarning() << Q_FUNC_INFO << " Network error: " << reply()->errorString();
    }

    connect(reply(), SIGNAL(uploadProgress(qint64,qint64)), this, SIGNAL(uploadProgress(qint64,qint64)));
    connect(this, SIGNAL(networkActivity()), account().data(), SIGNAL(propagatorNetworkActivity()));

    AbstractNetworkJob::start();
}

void PUTFileJob::slotTimeout() {
    _errorString =  tr("Connection Timeout");
    reply()->abort();
}

void PollJob::start()
{
    setTimeout(120 * 1000);
    QUrl accountUrl = account()->url();
    QUrl finalUrl = QUrl::fromUserInput(accountUrl.scheme() + QLatin1String("://") +  accountUrl.authority()
        + (path().startsWith('/') ? QLatin1String("") : QLatin1String("/")) + path());
    setReply(getRequest(finalUrl));
    setupConnections(reply());
    connect(reply(), SIGNAL(downloadProgress(qint64,qint64)), this, SLOT(resetTimeout()));
    AbstractNetworkJob::start();
}

bool PollJob::finished()
{
    QNetworkReply::NetworkError err = reply()->error();
    if (err != QNetworkReply::NoError) {
        _item._httpErrorCode = reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        _item._status = classifyError(err, _item._httpErrorCode);
        _item._errorString = reply()->errorString();

        if (reply()->hasRawHeader("OC-ErrorString")) {
            _item._errorString = reply()->rawHeader("OC-ErrorString");
        }

        if (_item._status == SyncFileItem::FatalError || _item._httpErrorCode >= 400) {
            if (_item._status != SyncFileItem::FatalError
                    && _item._httpErrorCode != 503) {
                SyncJournalDb::PollInfo info;
                info._file = _item._file;
                // no info._url removes it from the database
                _journal->setPollInfo(info);
                _journal->commit("remove poll info");

            }
            emit finishedSignal();
            return true;
        }
        start();
        return false;
    }

    bool ok = false;
    QByteArray jsonData = reply()->readAll().trimmed();
    qDebug() << Q_FUNC_INFO << ">" << jsonData << "<" << reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QVariantMap status = QtJson::parse(QString::fromUtf8(jsonData), ok).toMap();
    if (!ok || status.isEmpty()) {
        _item._errorString = tr("Invalid JSON reply from the poll URL");
        _item._status = SyncFileItem::NormalError;
        emit finishedSignal();
        return true;
    }

    if (status["unfinished"].isValid()) {
        start();
        return false;
    }

    _item._errorString = status["error"].toString();
    _item._status = _item._errorString.isEmpty() ? SyncFileItem::Success : SyncFileItem::NormalError;
    _item._fileId = status["fileid"].toByteArray();
    _item._etag = status["etag"].toByteArray();
    _item._responseTimeStamp = responseTimestamp();

    SyncJournalDb::PollInfo info;
    info._file = _item._file;
    // no info._url removes it from the database
    _journal->setPollInfo(info);
    _journal->commit("remove poll info");

    emit finishedSignal();
    return true;
}


void PropagateUploadFileQNAM::start()
{
    if (_propagator->_abortRequested.fetchAndAddRelaxed(0)) {
        return;
    }

    const QString fullFilePath(_propagator->getFilePath(_item._file));

    if (!FileSystem::fileExists(fullFilePath)) {
        done(SyncFileItem::SoftError, tr("File Removed"));
        return;
    }

    // Update the mtime and size, it might have changed since discovery.
    _item._modtime = FileSystem::getModTime(fullFilePath);
    quint64 fileSize = FileSystem::getSize(fullFilePath);
    _item._size = fileSize;

    // But skip the file if the mtime is too close to 'now'!
    // That usually indicates a file that is still being changed
    // or not yet fully copied to the destination.
    if (fileIsStillChanging(_item)) {
        _propagator->_anotherSyncNeeded = true;
        done(SyncFileItem::SoftError, tr("Local file changed during sync."));
        return;
    }

    _chunkCount = std::ceil(fileSize/double(chunkSize()));
    _startChunk = 0;
    _transferId = qrand() ^ _item._modtime ^ (_item._size << 16);

    const SyncJournalDb::UploadInfo progressInfo = _propagator->_journal->getUploadInfo(_item._file);

    if (progressInfo._valid && Utility::qDateTimeToTime_t(progressInfo._modtime) == _item._modtime ) {
        _startChunk = progressInfo._chunk;
        _transferId = progressInfo._transferid;
        qDebug() << Q_FUNC_INFO << _item._file << ": Resuming from chunk " << _startChunk;
    }

    _currentChunk = 0;
    _duration.start();

    emit progress(_item, 0);
    this->startNextChunk();
}

UploadDevice::UploadDevice(BandwidthManager *bwm)
    : _read(0),
      _bandwidthManager(bwm),
      _bandwidthQuota(0),
      _readWithProgress(0),
      _bandwidthLimited(false), _choked(false)
{
    _bandwidthManager->registerUploadDevice(this);
}


UploadDevice::~UploadDevice() {
    if (_bandwidthManager) {
        _bandwidthManager->unregisterUploadDevice(this);
    }
}

bool UploadDevice::prepareAndOpen(const QString& fileName, qint64 start, qint64 size)
{
    _data.clear();
    _read = 0;

    QFile file(fileName);
    QString openError;
    if (!FileSystem::openAndSeekFileSharedRead(&file, &openError, start)) {
        setErrorString(openError);
        return false;
    }

    size = qBound(0ll, size, FileSystem::getSize(fileName) - start);
    _data.resize(size);
    auto read = file.read(_data.data(), size);
    if (read != size) {
        setErrorString(file.errorString());
        return false;
    }

    return QIODevice::open(QIODevice::ReadOnly);
}


qint64 UploadDevice::writeData(const char* , qint64 ) {
    Q_ASSERT(!"write to read only device");
    return 0;
}

qint64 UploadDevice::readData(char* data, qint64 maxlen) {
    //qDebug() << Q_FUNC_INFO << maxlen << _read << _size << _bandwidthQuota;
    if (_data.size() - _read <= 0) {
        // at end
        if (_bandwidthManager) {
            _bandwidthManager->unregisterUploadDevice(this);
        }
        return -1;
    }
    maxlen = qMin(maxlen, _data.size() - _read);
    if (maxlen == 0) {
        return 0;
    }
    if (isChoked()) {
        return 0;
    }
    if (isBandwidthLimited()) {
        maxlen = qMin(maxlen, _bandwidthQuota);
        if (maxlen <= 0) {  // no quota
            qDebug() << "no quota";
            return 0;
        }
        _bandwidthQuota -= maxlen;
    }
    std::memcpy(data, _data.data()+_read, maxlen);
    _read += maxlen;
    return maxlen;
}

void UploadDevice::slotJobUploadProgress(qint64 sent, qint64 t)
{
    //qDebug() << Q_FUNC_INFO << sent << _read << t << _size << _bandwidthQuota;
    if (sent == 0 || t == 0) {
        return;
    }
    _readWithProgress = sent;
}

bool UploadDevice::atEnd() const {
    return _read >= _data.size();
}

qint64 UploadDevice::size() const{
//    qDebug() << this << Q_FUNC_INFO << _size;
    return _data.size();
}

qint64 UploadDevice::bytesAvailable() const
{
//    qDebug() << this << Q_FUNC_INFO << _size << _read << QIODevice::bytesAvailable()
//             <<   _size - _read + QIODevice::bytesAvailable();
    return _data.size() - _read + QIODevice::bytesAvailable();
}

// random access, we can seek
bool UploadDevice::isSequential() const{
    return false;
}

bool UploadDevice::seek ( qint64 pos ) {
    if (! QIODevice::seek(pos)) {
        return false;
    }
    if (pos < 0 || pos > _data.size()) {
        return false;
    }
    _read = pos;
    return true;
}

void UploadDevice::giveBandwidthQuota(qint64 bwq) {
    if (!atEnd()) {
        _bandwidthQuota = bwq;
        QMetaObject::invokeMethod(this, "readyRead", Qt::QueuedConnection); // tell QNAM that we have quota
    }
}

void UploadDevice::setBandwidthLimited(bool b) {
    _bandwidthLimited = b;
    QMetaObject::invokeMethod(this, "readyRead", Qt::QueuedConnection);
}

void UploadDevice::setChoked(bool b) {
    _choked = b;
    if (!_choked) {
        QMetaObject::invokeMethod(this, "readyRead", Qt::QueuedConnection);
    }
}

void PropagateUploadFileQNAM::startNextChunk()
{
    if (_propagator->_abortRequested.fetchAndAddRelaxed(0))
        return;

    if (! _jobs.isEmpty() &&  _currentChunk + _startChunk >= _chunkCount - 1) {
        // Don't do parallel upload of chunk if this might be the last chunk because the server cannot handle that
        // https://github.com/owncloud/core/issues/11106
        // We return now and when the _jobs will be finished we will proceed the last chunk
        // NOTE: Some other part of the code such as slotUploadProgress assume also that the last chunk
        // is sent last.
        return;
    }
    quint64 fileSize = _item._size;
    QMap<QByteArray, QByteArray> headers;
    headers["OC-Total-Length"] = QByteArray::number(fileSize);
    headers["OC-Async"] = "1";
    headers["OC-Chunk-Size"]= QByteArray::number(quint64(chunkSize()));
    headers["Content-Type"] = "application/octet-stream";
    headers["X-OC-Mtime"] = QByteArray::number(qint64(_item._modtime));
    if (!_item._etag.isEmpty() && _item._etag != "empty_etag" &&
            _item._instruction != CSYNC_INSTRUCTION_NEW  // On new files never send a If-Match
            ) {
        // We add quotes because the owncloud server always add quotes around the etag, and
        //  csync_owncloud.c's owncloud_file_id always strip the quotes.
        headers["If-Match"] = '"' + _item._etag + '"';
    }

    QString path = _item._file;

    UploadDevice *device = new UploadDevice(&_propagator->_bandwidthManager);
    qint64 chunkStart = 0;
    qint64 currentChunkSize = fileSize;
    if (_chunkCount > 1) {
        int sendingChunk = (_currentChunk + _startChunk) % _chunkCount;
        // XOR with chunk size to make sure everything goes well if chunk size change between runs
        uint transid = _transferId ^ chunkSize();
        path +=  QString("-chunking-%1-%2-%3").arg(transid).arg(_chunkCount).arg(sendingChunk);

        headers["OC-Chunked"] = "1";

        chunkStart = chunkSize() * quint64(sendingChunk);
        currentChunkSize = chunkSize();
        if (sendingChunk == _chunkCount - 1) { // last chunk
            currentChunkSize = (fileSize % chunkSize());
            if( currentChunkSize == 0 ) { // if the last chunk pretents to be 0, its actually the full chunk size.
                currentChunkSize = chunkSize();
            }
        }
    }

    if (! device->prepareAndOpen(_propagator->getFilePath(_item._file), chunkStart, currentChunkSize)) {
        qDebug() << "ERR: Could not prepare upload device: " << device->errorString();
        // Soft error because this is likely caused by the user modifying his files while syncing
        abortWithError( SyncFileItem::SoftError, device->errorString() );
        delete device;
        return;
    }

    // job takes ownership of device via a QScopedPointer. Job deletes itself when finishing
    PUTFileJob* job = new PUTFileJob(_propagator->account(), _propagator->_remoteFolder + path, device, headers, _currentChunk);
    _jobs.append(job);
    connect(job, SIGNAL(finishedSignal()), this, SLOT(slotPutFinished()));
    connect(job, SIGNAL(uploadProgress(qint64,qint64)), this, SLOT(slotUploadProgress(qint64,qint64)));
    connect(job, SIGNAL(uploadProgress(qint64,qint64)), device, SLOT(slotJobUploadProgress(qint64,qint64)));
    connect(job, SIGNAL(destroyed(QObject*)), this, SLOT(slotJobDestroyed(QObject*)));
    job->start();
    _propagator->_activeJobs++;
    _currentChunk++;

    bool parallelChunkUpload = true;
    QByteArray env = qgetenv("OWNCLOUD_PARALLEL_CHUNK");
    if (!env.isEmpty()) {
        parallelChunkUpload = env != "false" && env != "0";
    } else {
        auto version = _propagator->account()->serverVersion();
        auto components = version.split('.');
        int versionNum = (components.value(0).toInt() << 16)
                       + (components.value(1).toInt() << 8)
                       + components.value(2).toInt();
        if (versionNum < 0x080003) {
            // Disable parallel chunk upload severs older than 8.0.3 to avoid too many
            // internal sever errors (#2743, #2938)
            parallelChunkUpload = false;
        }
    }

    if (_currentChunk + _startChunk >= _chunkCount - 1) {
        // Don't do parallel upload of chunk if this might be the last chunk because the server cannot handle that
        // https://github.com/owncloud/core/issues/11106
        parallelChunkUpload = false;
    }

    if (parallelChunkUpload && (_propagator->_activeJobs < _propagator->maximumActiveJob())
            && _currentChunk < _chunkCount ) {
        startNextChunk();
    }
    if (!parallelChunkUpload || _chunkCount - _currentChunk <= 0) {
        emit ready();
    }
}

void PropagateUploadFileQNAM::slotPutFinished()
{
    PUTFileJob *job = qobject_cast<PUTFileJob *>(sender());
    Q_ASSERT(job);
    slotJobDestroyed(job); // remove it from the _jobs list

    qDebug() << Q_FUNC_INFO << job->reply()->request().url() << "FINISHED WITH STATUS"
             << job->reply()->error()
             << (job->reply()->error() == QNetworkReply::NoError ? QLatin1String("") : job->reply()->errorString())
             << job->reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute)
             << job->reply()->attribute(QNetworkRequest::HttpReasonPhraseAttribute);

    _propagator->_activeJobs--;

    if (_finished) {
        // We have send the finished signal already. We don't need to handle any remaining jobs
        return;
    }

    QNetworkReply::NetworkError err = job->reply()->error();
    if (err != QNetworkReply::NoError) {
        _item._httpErrorCode = job->reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if(checkForProblemsWithShared(_item._httpErrorCode,
            tr("The file was edited locally but is part of a read only share. "
               "It is restored and your edit is in the conflict file."))) {
            return;
        }
        QString errorString = job->errorString();

        QByteArray replyContent = job->reply()->readAll();
        qDebug() << replyContent; // display the XML error in the debug
        QRegExp rx("<s:message>(.*)</s:message>"); // Issue #1366: display server exception
        if (rx.indexIn(QString::fromUtf8(replyContent)) != -1) {
            errorString += QLatin1String(" (") + rx.cap(1) + QLatin1Char(')');
        }

        if (job->reply()->hasRawHeader("OC-ErrorString")) {
            errorString = job->reply()->rawHeader("OC-ErrorString");
        }

        if (_item._httpErrorCode == 412) {
            // Precondition Failed:   Maybe the bad etag is in the database, we need to clear the
            // parent folder etag so we won't read from DB next sync.
            _propagator->_journal->avoidReadFromDbOnNextSync(_item._file);
            _propagator->_anotherSyncNeeded = true;
        }

        abortWithError(classifyError(err, _item._httpErrorCode), errorString);
        return;
    }

    _item._httpErrorCode = job->reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // The server needs some time to process the request and provide with a poll URL
    if (_item._httpErrorCode == 202) {
        _finished = true;
        QString path =  QString::fromUtf8(job->reply()->rawHeader("OC-Finish-Poll"));
        if (path.isEmpty()) {
            done(SyncFileItem::NormalError, tr("Poll URL missing"));
            return;
        }
        startPollJob(path);
        return;
    }

    // Check the file again post upload.
    // Two cases must be considered separately: If the upload is finished,
    // the file is on the server and has a changed ETag. In that case,
    // the etag has to be properly updated in the client journal, and because
    // of that we can bail out here with an error. But we can reschedule a
    // sync ASAP.
    // But if the upload is ongoing, because not all chunks were uploaded
    // yet, the upload can be stopped and an error can be displayed, because
    // the server hasn't registered the new file yet.
    bool finished = job->reply()->hasRawHeader("ETag")
            || job->reply()->hasRawHeader("OC-ETag");

    // Check if the file still exists
    const QString fullFilePath(_propagator->getFilePath(_item._file));
    if( !FileSystem::fileExists(fullFilePath) ) {
        if (!finished) {
            abortWithError(SyncFileItem::SoftError, tr("The local file was removed during sync."));
            return;
        } else {
            _propagator->_anotherSyncNeeded = true;
        }
    }

    // compare expected and real modification time of the file and size
    const time_t new_mtime = FileSystem::getModTime(fullFilePath);
    const quint64 new_size = static_cast<quint64>(FileSystem::getSize(fullFilePath));
    QFileInfo fi(_propagator->getFilePath(_item._file));
    if (new_mtime != _item._modtime || new_size != _item._size) {
        qDebug() << "The local file has changed during upload:"
                 << "mtime: " << _item._modtime << "<->" << new_mtime
                 << ", size: " << _item._size << "<->" << new_size
                 << ", QFileInfo: " << Utility::qDateTimeToTime_t(fi.lastModified()) << fi.lastModified();
        _propagator->_anotherSyncNeeded = true;
        if( !finished ) {
            abortWithError(SyncFileItem::SoftError, tr("Local file changed during sync."));
            // FIXME:  the legacy code was retrying for a few seconds.
            //         and also checking that after the last chunk, and removed the file in case of INSTRUCTION_NEW
            return;
        }
    }

    if (!finished) {
        // Proceed to next chunk.
        if (_currentChunk >= _chunkCount) {
            if (!_jobs.empty()) {
                // just wait for the other job to finish.
                return;
            }
            _finished = true;
            done(SyncFileItem::NormalError, tr("The server did not acknowledge the last chunk. (No e-tag were present)"));
            return;
        }

        SyncJournalDb::UploadInfo pi;
        pi._valid = true;
        auto currentChunk = job->_chunk;
        foreach (auto *job, _jobs) {
            // Take the minimum finished one
            currentChunk = qMin(currentChunk, job->_chunk - 1);
        }
        pi._chunk = (currentChunk + _startChunk + 1) % _chunkCount ; // next chunk to start with
        pi._transferid = _transferId;
        pi._modtime =  Utility::qDateTimeFromTime_t(_item._modtime);
        _propagator->_journal->setUploadInfo(_item._file, pi);
        _propagator->_journal->commit("Upload info");
        startNextChunk();
        return;
    }

    // the following code only happens after all chunks were uploaded.
    _finished = true;
    // the file id should only be empty for new files up- or downloaded
    QByteArray fid = job->reply()->rawHeader("OC-FileID");
    if( !fid.isEmpty() ) {
        if( !_item._fileId.isEmpty() && _item._fileId != fid ) {
            qDebug() << "WARN: File ID changed!" << _item._fileId << fid;
        }
        _item._fileId = fid;
    }

    QByteArray etag = getEtagFromReply(job->reply());
    _item._etag = etag;

    _item._responseTimeStamp = job->responseTimestamp();

    if (job->reply()->rawHeader("X-OC-MTime") != "accepted") {
        // X-OC-MTime is supported since owncloud 5.0.   But not when chunking.
        // Normaly Owncloud 6 always put X-OC-MTime
        qWarning() << "Server do not support X-OC-MTime" << job->reply()->rawHeader("X-OC-MTime");
#ifdef USE_NEON
        PropagatorJob *newJob = new UpdateMTimeAndETagJob(_propagator, _item);
        QObject::connect(newJob, SIGNAL(completed(SyncFileItem)), this, SLOT(finalize(SyncFileItem)));
        QMetaObject::invokeMethod(newJob, "start");
        return;
#else
        // Well, the mtime was not set
#endif
    }
    finalize(_item);
}

void PropagateUploadFileQNAM::finalize(const SyncFileItem &copy)
{
    // Normally, copy == _item,   but when it comes from the UpdateMTimeAndETagJob, we need to do
    // some updates
    _item._etag = copy._etag;
    _item._fileId = copy._fileId;

    _item._requestDuration = _duration.elapsed();

    _propagator->_journal->setFileRecord(SyncJournalFileRecord(_item, _propagator->getFilePath(_item._file)));
    // Remove from the progress database:
    _propagator->_journal->setUploadInfo(_item._file, SyncJournalDb::UploadInfo());
    _propagator->_journal->commit("upload file start");

    _finished = true;
    done(SyncFileItem::Success);
}

void PropagateUploadFileQNAM::slotUploadProgress(qint64 sent, qint64 total)
{
    if (sent == 0 && total == 0) {
        return; // QNAM bug https://bugreports.qt.io/browse/QTBUG-44782
    }
    int progressChunk = _currentChunk + _startChunk - 1;
    if (progressChunk >= _chunkCount)
        progressChunk = _currentChunk - 1;

    // amount is the number of bytes already sent by all the other chunks that were sent
    // not including this one.
    // FIXME: this assume all chunks have the same size, which is true only if the last chunk
    // has not been finished (which should not happen because the last chunk is sent sequentially)
    quint64 amount = progressChunk * chunkSize();

    sender()->setProperty("byteWritten", sent);
    if (_jobs.count() > 1) {
        amount -= (_jobs.count() -1) * chunkSize();
        foreach (QObject *j, _jobs) {
            amount += j->property("byteWritten").toULongLong();
        }
    } else {
        // sender() is the only current job, no need to look at the byteWritten properties
        amount += sent;
    }
    emit progress(_item, amount);
}

void PropagateUploadFileQNAM::startPollJob(const QString& path)
{
    PollJob* job = new PollJob(_propagator->account(), path, _item,
                               _propagator->_journal, _propagator->_localDir, this);
    connect(job, SIGNAL(finishedSignal()), SLOT(slotPollFinished()));
    SyncJournalDb::PollInfo info;
    info._file = _item._file;
    info._url = path;
    info._modtime = _item._modtime;
    _propagator->_journal->setPollInfo(info);
    _propagator->_journal->commit("add poll info");
    _propagator->_activeJobs++;
    job->start();
}

void PropagateUploadFileQNAM::slotPollFinished()
{
    PollJob *job = qobject_cast<PollJob *>(sender());
    Q_ASSERT(job);

    _propagator->_activeJobs--;

    if (job->_item._status != SyncFileItem::Success) {
        _finished = true;
        done(job->_item._status, job->_item._errorString);
        return;
    }

    finalize(job->_item);
}

void PropagateUploadFileQNAM::slotJobDestroyed(QObject* job)
{
    _jobs.erase(std::remove(_jobs.begin(), _jobs.end(), job) , _jobs.end());
}

void PropagateUploadFileQNAM::abort()
{
    foreach(auto *job, _jobs) {
        if (job->reply()) {
            qDebug() << Q_FUNC_INFO << job << this->_item._file;
            job->reply()->abort();
        }
    }
}

// This function is used whenever there is an error occuring and jobs might be in progress
void PropagateUploadFileQNAM::abortWithError(SyncFileItem::Status status, const QString &error)
{
    _finished = true;
    abort();
    done(status, error);
}


}
