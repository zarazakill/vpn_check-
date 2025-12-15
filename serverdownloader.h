#ifndef SERVERDOWNLOADER_H
#define SERVERDOWNLOADER_H

#include <QThread>
#include <QMutex>
#include "vpntypes.h"

class ServerDownloaderThread : public QThread {
    Q_OBJECT

public:
    explicit ServerDownloaderThread(QObject *parent = nullptr);

signals:
    void downloadFinished(const QList<VpnServer>& servers);
    void downloadError(const QString& error);
    void downloadProgress(int progress);
    void logMessage(const QString& message);

protected:
    void run() override;

private:
    QList<VpnServer> parseServersData(const QString& data);
    QString downloadWithRetry(const QStringList& urls);
    mutable QMutex mutex;  // Защита общих данных
};

#endif // SERVERDOWNLOADER_H
