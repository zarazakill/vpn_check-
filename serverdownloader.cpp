#include "serverdownloader.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QEventLoop>
#include <QTimer>
#include <QRegularExpression>

ServerDownloaderThread::ServerDownloaderThread(QObject *parent)
: QThread(parent) {
}

void ServerDownloaderThread::run() {
    emit logMessage("📥 Получение списка серверов с VPNGate...");

    QStringList urls = {
        "https://download.vpngate.jp/api/iphone/",
        "http://download.vpngate.jp/api/iphone/",
        "https://www.vpngate.net/api/iphone/"
    };

    QString data = downloadWithRetry(urls);
    if (data.isEmpty()) {
        emit downloadError("Не удалось загрузить данные с VPNGate");
        return;
    }

    QList<VpnServer> servers = parseServersData(data);
    emit downloadFinished(servers);
}

QString ServerDownloaderThread::downloadWithRetry(const QStringList& urls) {
    QNetworkAccessManager manager;

    for (const QString& url : urls) {
        emit logMessage(QString("Пробую подключиться к: %1").arg(url));

        QNetworkRequest request((QUrl(url)));
        request.setHeader(QNetworkRequest::UserAgentHeader,
                          "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");

        QNetworkReply* reply = manager.get(request);

        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);

        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

        timer.start(15000);
        loop.exec();

        if (timer.isActive()) {
            timer.stop();
            if (reply->error() == QNetworkReply::NoError) {
                QString data = QString::fromUtf8(reply->readAll());
                reply->deleteLater();
                emit logMessage(QString("✅ Успешно подключились к: %1").arg(url));
                return data;
            }
        } else {
            reply->abort();
        }

        reply->deleteLater();
    }

    return QString();
}

QList<VpnServer> ServerDownloaderThread::parseServersData(const QString& data) {
    QList<VpnServer> servers;
    QStringList lines = data.split('\n', Qt::SkipEmptyParts);

    for (int i = 2; i < lines.size(); ++i) {
        QString line = lines[i].trimmed();
        if (line.isEmpty() || line.startsWith('*')) {
            continue;
        }

        QStringList parts = line.split(',');
        if (parts.size() < 15) {
            continue;
        }

        VpnServer server;
        server.name = parts[0] + "_" + parts[5];
        server.filename = server.name + ".ovpn";
        server.configBase64 = parts[14];
        server.country = parts[6];
        server.ip = parts[1];
        server.port = 1194;
        server.protocol = "udp";
        server.score = parts[2].toInt();
        server.ping = parts[3].toInt();
        server.speedMbps = parts[4].toDouble() / 1000000.0;
        server.sessions = parts[7];
        server.uptime = parts[8];
        server.tested = false;
        server.available = true; // Все серверы считаем доступными без тестирования
        server.testPing = server.ping; // Используем пинг из данных
        server.realConnectionTested = false;

        try {
            QByteArray configData = QByteArray::fromBase64(server.configBase64.toLatin1());
            QString config = QString::fromUtf8(configData);

            QStringList configLines = config.split('\n');
            for (const QString& configLine : configLines) {
                QString trimmed = configLine.trimmed();
                if (trimmed.startsWith("proto ")) {
                    server.protocol = trimmed.mid(6).trimmed();
                } else if (trimmed.startsWith("remote ") && trimmed.split(' ').size() >= 3) {
                    server.port = trimmed.split(' ')[2].toInt();
                }
            }
        } catch (...) {
            // Используем значения по умолчанию
        }

        servers.append(server);

        if (lines.size() > 0) {
            emit downloadProgress(static_cast<int>((i * 100) / lines.size()));
        }
    }

    std::sort(servers.begin(), servers.end(),
              [](const VpnServer& a, const VpnServer& b) {
                  return a.speedMbps > b.speedMbps;
              });

    emit logMessage(QString("✅ Успешно распарсено %1 серверов").arg(servers.size()));
    return servers;
}
