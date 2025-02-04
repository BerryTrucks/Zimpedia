/*
  Copyright (C) 2016 Michal Kosciesza <michal@mkiol.net>

  This file is part of Zimpedia application.

  This Source Code Form is subject to the terms of
  the Mozilla Public License, v.2.0. If a copy of
  the MPL was not distributed with this file, You can
  obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <QDebug>
#include <QStringList>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QRegExp>
#include <QUrl>
#include <string>
#include <cctype>
#include <algorithm>

#include <zim/file.h>
#include <zim/fileiterator.h>
#include <zim/fileheader.h>
#include <zim/search.h>
#include <zim/error.h>
#include <zim/blob.h>

#include "zimserver.h"
#include "settings.h"

#ifdef SAILFISH
#include <sailfishapp.h>
#endif

#ifdef BB10
#define Q_NULLPTR 0
#endif

ZimServer* ZimServer::m_instance = nullptr;

ZimServer* ZimServer::instance(QObject *parent)
{
    if (ZimServer::m_instance == nullptr) {
        ZimServer::m_instance = new ZimServer(parent);
    }

    return ZimServer::m_instance;
}

ZimServer::ZimServer(QObject *parent) :
    QThread(parent), busy(false)
{
    zimfile = Q_NULLPTR;

    // Thread
    //QObject::connect(this, SIGNAL(finished()), this, SLOT(finishedHandler()));

    // Local HTTP server
    server = new QHttpServer;
    QObject::connect(server, SIGNAL(newRequest(QHttpRequest*, QHttpResponse*)),
            this, SLOT(requestHandler(QHttpRequest*, QHttpResponse*)));

    Settings* s = Settings::instance();

    isListening = server->listen(s->getPort());
    if (!isListening) {
        qWarning() << "Unable to start HTTP server";
    }

    emit listeningChanged();
}

bool ZimServer::getBusy()
{
    return this->busy;
}

void ZimServer::run()
{
    if (!getLoaded()) {
        qWarning() << "ZIM file not loaded";
        emit articleReady("");
        return;
    }

    QString zimUrl = urlToAsyncGet;

    QByteArray data;
    QString mimeType;
    if (!getArticle(zimUrl, data, mimeType)) {
        emit articleReady("");
        return;
    }

    if (mimeType == "text/html") {
        mimeType == "text/html; charset=utf-8";
        QString dd = QString::fromUtf8(data);
        filter(dd);
        emit articleReady(dd);
    } else {
        emit articleReady(QString(data));
    }
}

void ZimServer::openUrl(const QString &url, const QString &title)
{
    //qDebug() << "ZimServer::openUrl" << url;

    QStringList parts = url.split("/");
    if (parts.length()<4) {
        qWarning() << "Invalid Url";
        return;
    }

    QString uuid = parts.at(3);
    //qDebug() << "length" << parts.length();
    //qDebug() << "UUID:" << uuid;

    if (uuid.startsWith("uuid:")) {
        uuid = uuid.right(uuid.length()-5);
        if (loadZimFileByUuid(uuid)) {
            //qDebug() << "ZimServer::openUrl urlReady" << url << title;
            emit urlReady(url, title);
        } else {
            qWarning() << "Unable to load ZIM file with" << uuid << "UUID";
        }
    } else {
        qWarning() << "Invalid UUID";
    }
}

bool ZimServer::loadZimFileByUuid(const QString& uuid)
{
    if (!uuid.isEmpty() && uuid == this->metadata.checksum) {
        //qDebug() << "ZIM file with UUID" << uuid << "already loaded!";
        return true;
    }

    auto fm = FileModel::instance();
    ZimMetaData metadata = fm->files.value(uuid);
    if (metadata.isEmpty()) {
        qWarning() << "ZIM file with UUID" << uuid << "doesn't exist";
        return false;
    }

    Settings* s = Settings::instance();
    s->setZimFile(metadata.path);

    bool ok = loadZimPath(metadata.path);
    if (!ok)
        emit loadedChanged();

    return ok;
}

bool ZimServer::loadZimFile()
{
    Settings* s = Settings::instance();
    bool ok = loadZimPath(s->getZimFile());
    emit loadedChanged();
    return ok;
}

bool ZimServer::loadZimPath(const QString& path)
{
    if (!path.isEmpty() && path == this->metadata.path) {
        qWarning() << "ZIM file already loaded:" << path;
        return true;
    }

    // Cleating article model
    ArticleModel::instance()->setFilter(QString());

    auto s = Settings::instance();

    this->hasMainPage = false;
    this->metadata.clear();

    if (path.isEmpty()) {
        if (zimfile != Q_NULLPTR) {
            delete zimfile;
            zimfile = Q_NULLPTR;
        }
        emit zimChanged();
        return false;
    }

    if (!QFile::exists(path)) {
        qWarning() << "ZIM file doesn't exist:" << path;
        if (zimfile != Q_NULLPTR) {
            delete zimfile;
            zimfile = Q_NULLPTR;
        }
        s->setZimFile("");
        emit zimChanged();
        return false;
    }

    this->busy = true;
    emit busyChanged();

    this->metadata.path = path;
    this->metadata.fields =
            ZimMetaData::Title |
            ZimMetaData::Checksum |
            ZimMetaData::Language |
            ZimMetaData::Favicon |
            ZimMetaData::Tags;

    if (!FileModel::scanZimFile(this->metadata)) {
        if (zimfile != Q_NULLPTR) {
            delete zimfile;
            zimfile = Q_NULLPTR;
        }
        this->metadata.clear();
        this->busy = false;
        emit busyChanged();
        emit zimChanged();
        return false;
    }

    try
    {
        zimfile = new zim::File(this->metadata.path.toStdString());
    }
    catch (const std::exception& e)
    {
        qWarning() << "Unable to open ZIM file" << this->metadata.path << "!";
        qWarning() << "Details:" << e.what();
        if (zimfile != Q_NULLPTR) {
            delete zimfile;
            zimfile = Q_NULLPTR;
        }
        s->setZimFile("");
        this->metadata.clear();
        this->busy = false;
        emit busyChanged();
        emit zimChanged();
        return false;
    }

    QByteArray data; QString mimeType;
    // Main page
    if (!zimfile->getFileheader().hasMainPage()) {
        this->hasMainPage = false;
    } else {
        // Wikipedia ZIM returns true for hasMainPage but has empty main page
        this->hasMainPage = getArticle("A/mainpage", data, mimeType);
    }

    qDebug() << "ZIM file tags:" << metadata.tags;
    this->ftindex = metadata.tags.split(';').contains("_ftindex");
    qDebug() << "ZIM file has ftindex:" << this->ftindex;

    this->busy = false;
    emit busyChanged();
    emit zimChanged();
    return true;
}

bool ZimServer::getLoaded()
{
    return zimfile != Q_NULLPTR;
}

bool ZimServer::getListening()
{
    return isListening;
}

bool ZimServer::getResContent(const QString &filename, QByteArray &data)
{
#ifdef SAILFISH
    QFile file(SailfishApp::pathTo("res/" + filename).toLocalFile());
#elif BB10
    QFile file(QDir::currentPath()+"/app/native/res/" + filename);
#else
    return false;
#endif

    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Could not open" << filename << "for reading: " << file.errorString();
        file.close();
        return false;
    }

    data = file.readAll();
    file.close();

    return true;
}

QString ZimServer::getContentType(const QString & file)
{
    QStringList l = file.split(".");

    QString ext = "";
    if (l.length()>0)
        ext = l[l.length()-1];

    return ext == "css" ? "text/css; charset=utf-8" :
           ext == "gif" ? "image/gif" :
           ext == "png" ? "image/png" :
           ext == "js" ? "application/x-javascript; charset=utf-8" :
           "text/html; charset=utf-8";
}

bool ZimServer::getHasMainPage()
{
    return this->hasMainPage;
}

QString ZimServer::getTitle()
{
    return this->metadata.title;
}

QString ZimServer::getLanguage()
{
    return this->metadata.language;
}

QString ZimServer::getUuid()
{
    return this->metadata.checksum;
}

QString ZimServer::getFavicon()
{
    return this->metadata.favicon;
}

bool ZimServer::getArticle(const QString zimUrl, QByteArray &data, QString &mimeType)
{
    return ZimServer::getArticle(this->zimfile, zimUrl, data, mimeType);
}

bool ZimServer::getArticle(zim::File *zimfile, const QString zimUrl, QByteArray &data, QString &mimeType)
{
    //qDebug() << "getArticle static, zimUrl:" << zimUrl;

    zim::Article article;

    try {
        if (zimUrl == "A/mainpage") {
            if (zimfile->getFileheader().hasMainPage()) {
                zim::Fileheader header = zimfile->getFileheader();
                article = zimfile->getArticle(header.getMainPage());
            } else {
                qWarning() << "Main page not found!";
                return false;
            }
        } else {
            zim::File::const_iterator it = zimfile->find(zim::urldecode(ZimServer::stringQtoStd(zimUrl)));
            if (it == zimfile->end()) {
              qWarning() << "Article not found!";
              return false;
            }

            if (it->isDeleted()) {
              qWarning() << "Article deleted!";
              return false;
            }

            article = it->isRedirect() ? it->getRedirectArticle() : *it;
        }

        zim::Blob zimblob = article.getData();

        if (zimblob.size() == 0) {
            qWarning() << "Article empty!";
            return false;
        }

        data = QByteArray(zimblob.data(), zimblob.size());
    } catch (std::exception &e) {
        qWarning() << "Cannot get an article:" << e.what();
        return false;
    }

    try {
        mimeType = QString::fromStdString(article.getMimeType());
    } catch (std::runtime_error &e) {
        qWarning() << "Unable to get mimeType:" << e.what();
    }

    return true;
}

void ZimServer::getArticleAsync(const QString &zimUrl)
{
    //qDebug() << "getArticleAsync, zimUrl:" << zimUrl;
    QStringList parts = zimUrl.split("/");
    if (parts.length()<2) {
        qWarning() << "Bad url:" << zimUrl;
        return;
    }
    urlToAsyncGet = QStringList(parts.mid(parts.length()-2)).join("/");
    this->start(QThread::IdlePriority);
}

void ZimServer::requestHandler(QHttpRequest *req, QHttpResponse *resp)
{
    //qDebug() << "requestHandler";
    //qDebug() << "req->url():" << req->url().path();

    QStringList parts = req->url().path().split("/");

    QString uuid = parts.at(1);
    //qDebug() << "length" << parts.length();
    //qDebug() << "UUID:" << uuid;

    if (parts.length() < 4) {
        qWarning() << "Invalid Url:" << req->url();
        resp->setHeader("Content-Length", "0");
        resp->setHeader("Connection", "close");
        resp->writeHead(404);
        resp->end();
        return;
    }

    QString zimUrl;

    if (uuid.startsWith("uuid:")) {
        uuid = uuid.right(uuid.length()-5);
        //qDebug() << uuid;
        if (!loadZimFileByUuid(uuid)) {
            qWarning() << "Unable to load ZIM file with UUID:" << uuid;
            resp->setHeader("Content-Length", "0");
            resp->setHeader("Connection", "close");
            resp->writeHead(500);
            resp->end();
            return;
        }

        zimUrl = parts.last() == "mainpage" ?
                    "A/mainpage" : QStringList(parts.mid(2)).join("/");
    } else {
        qWarning() << "Invalid UUID!";

        if (!getLoaded()) {
            qWarning() << "ZIM file not loaded!";
            resp->setHeader("Content-Length", "0");
            resp->setHeader("Connection", "close");
            resp->writeHead(500);
            resp->end();
            return;
        }

        zimUrl = QStringList(parts.mid(1)).join("/");
    }

    //qDebug() << "zimUrl:" << zimUrl;

    QByteArray data;
    QString mimeType;
    if (!getArticle(zimUrl, data, mimeType)) {
        resp->setHeader("Content-Length", "0");
        resp->setHeader("Connection", "close");
        resp->writeHead(404);
        resp->end();
        return;
    }

    if (mimeType == "text/html") {
        mimeType == "text/html; charset=utf-8";
        QString dd = QString::fromUtf8(data);
        filter(dd);
        data = dd.toUtf8();
    }

    resp->setHeader("Content-Length", QString::number(data.size()));
    resp->setHeader("Content-Type", mimeType);
    resp->setHeader("Connection", "close");
    resp->writeHead(200);
    resp->end(data);
}

QString ZimServer::getLocalUrl(const QString &zimUrl)
{
    Settings* s = Settings::instance();

    return QString("http://localhost:%1/uuid:%2/%3")
            .arg(s->getPort())
            .arg(getUuid())
            .arg(zimUrl);
}

std::string ZimServer::stringQtoStd(const QString &s)
{
    QByteArray b = s.toUtf8();
    return std::string(b.data(), b.length());
}

QString ZimServer::stringStdToQ(const std::string &s)
{
    return QString::fromUtf8(s.data());
}

QString ZimServer::getTitleFromUrl(const QString &url)
{
    if (!getLoaded()) {
        qWarning() << "ZIM file not loaded";
        return "";
    }

    //qDebug() << "getTitleFromUrl:" << url;

    QStringList parts = url.split("/");
    if (parts.length() < 6) {
        qWarning() << "Url is invalid:" << url;
        return "";
    }

    QString zimuuid = parts.at(3);
    if (zimuuid.startsWith("uuid:")) {
        zimuuid = zimuuid.right(zimuuid.length()-5);
    } else {
        qWarning() << "Url UUID is invalid or missing";
        return "";
    }

    //QString _url = QStringList(parts.mid(parts.length()-2)).join("/");
    QString _url = QStringList(parts.mid(4)).join("/");
    if (_url == "A/mainpage") {
        return tr("Main page");
    }

    QString title;

    try {
        zim::File::const_iterator it = zimfile->find(zim::urldecode(ZimServer::stringQtoStd(_url)));
        title = ZimServer::stringStdToQ(it->getTitle());
    } catch (zim::ZimFileFormatError &e) {
        qWarning() << "Exception:" << e.what();
    }

    return title;
}

QList<SearchResult> ZimServer::search(const QString &value)
{
    //qDebug() << "search:" << value;

    QList<SearchResult> result;

    auto sm = Settings::instance()->getSearchMode();

    if (getLoaded()) {
        auto t = value.trimmed();
        if (!t.isEmpty()) {
            try {
                if (ftindex && sm == Settings::FullTextSearch) {
                    // Trying full text search
                    std::string st(t.toUtf8().constData());
                    auto search = zimfile->search(st, 0, maxSearch);
                    auto it = search->begin();

                    for(int i = 0; it != search->end() && i < maxSearch; ++it, ++i) {
                        result << SearchResult {
                                    ZimServer::stringStdToQ(it->getTitle()),
                                    getLocalUrl(ZimServer::stringStdToQ(it->getLongUrl()))
                                  };
                    }
                }

                if (result.isEmpty()) {
                    // Trying title search
                    QStringList urls;

                    // first letter is upper case
                    t[0] = t[0].toUpper();
                    std::string stu(t.toUtf8().constData());
                    auto itu = zimfile->findByTitle('A', stu);
                    for (int i = 0; itu != zimfile->end() && i < maxSearch; ++itu, ++i) {
                        auto url = getLocalUrl(ZimServer::stringStdToQ(itu->getLongUrl()));
                        result << SearchResult {
                                    ZimServer::stringStdToQ(itu->getTitle()),
                                    url
                                  };
                        urls << url;
                    }

                    // first letter is lower case
                    t[0] = t[0].toLower();
                    std::string stl(t.toUtf8().constData());
                    auto itl = zimfile->findByTitle('A', stl);
                    for (int i = 0; itl != zimfile->end() && i < maxSearch; ++itl, ++i) {
                        auto url = getLocalUrl(ZimServer::stringStdToQ(itl->getLongUrl()));
                        if (!urls.contains(url)) {
                            result << SearchResult {
                                        ZimServer::stringStdToQ(itl->getTitle()),
                                        url
                                      };
                        }
                    }

                    // sorting
                    std::sort(result.begin(), result.end(), [](SearchResult a, SearchResult b) {
                        return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
                    });

                    if (result.size() > maxSearch) {
                        result = result.mid(0, maxSearch);
                    }
                }
            } catch (zim::ZimFileFormatError &e) {
                qWarning() << "Exception:" << e.what();
            }
        }
    } else {
        qWarning() << "ZIM file not loaded";
    }

    return result;
}

QString ZimServer::serverUrl()
{
    Settings* s = Settings::instance();
    return QString("http://localhost:%1/uuid:%2/").arg(s->getPort()).arg(getUuid());
}

void ZimServer::fixUrl(QString &url)
{
    if (url.startsWith("..")) {
        url = url.right(url.length()-2);
    }
    if (url.startsWith("/")) {
        url = url.right(url.length()-1);
    }
}

void ZimServer::filter(QString &data)
{
    int pos;

    // <link href=\"../-/s/css_modules/ext.kartographer.link.css\" rel=\"stylesheet\" type=\"text/css\">
    QRegExp rxStyle("<link\\s[^>]*href\\s*=\\s*\"(\\S*)\"[^>]*>", Qt::CaseInsensitive);
    pos = 0;
    while (pos >= 0) {
        pos = rxStyle.indexIn(data, pos);
        if (pos >= 0) {
            QString url = rxStyle.cap(1); fixUrl(url);
            QByteArray emData; QString mimeType;
            if (getArticle(url, emData, mimeType)) {
                data.replace(pos, rxStyle.cap(0).length(), "<style type=\"text/css\">" + emData + "</style>");
            } else {
                qWarning() << "Cannot get embeded style content";
            }

            ++pos;
        }
    }

    // <script src=\"../-/j/js_modules/startup.js\"></script>
    QRegExp rxScript("<script\\s[^>]*src\\s*=\\s*\"(\\S*)\"[^>]*></script>", Qt::CaseInsensitive);
    pos = 0;
    while (pos >= 0) {
        pos = rxScript.indexIn(data, pos);
        if (pos >= 0) {
            QString url = rxScript.cap(1); fixUrl(url);
            QByteArray emData; QString mimeType;
            if (getArticle(url, emData, mimeType)) {
                data.replace(pos, rxScript.cap(0).length(), "<script type=\"text/javascript\">" + emData + "</script>");
            } else {
                qWarning() << "Cannot get embeded script content";
            }

            ++pos;
        }
    }

    // <img src=\"../I/m/AloraMountain.JPG\" data-file-width=\"3648\" data-file-height=\"2736\"
    // data-file-type=\"bitmap\" height=\"150\" width=\"200\" id=\"mwBQ\">
    QRegExp rxImg("<img\\s[^>]*src\\s*=\\s*\"(\\S*)\"[^>]*>", Qt::CaseInsensitive);
    pos = 0;
    while (pos >= 0) {
        pos = rxImg.indexIn(data, pos);
        if (pos >= 0) {
            QString url = rxImg.cap(1); fixUrl(url);
            QByteArray emData; QString mimeType;
            if (getArticle(url, emData, mimeType)) {
                QString imgTag = rxImg.cap(0);
                imgTag.replace(rxImg.cap(1),"data:"+mimeType+";base64,"+emData.toBase64());
                data.replace(pos, rxImg.cap(0).length(), imgTag);
            } else {
                qWarning() << "Cannot get embeded image content";
            }

            ++pos;
        }
    }
}
