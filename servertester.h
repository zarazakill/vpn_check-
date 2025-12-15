#ifndef SERVERTESTER_H
#define SERVERTESTER_H

#include <QThread>
#include <QProcess>
#include <QMutex>
#include <QMutexLocker>
#include "vpntypes.h"

class ServerTesterThread : public QThread {
    Q_OBJECT

public:
    explicit ServerTesterThread(const QString& serverIp, const QString& serverName, QObject *parent = nullptr);
    ~ServerTesterThread();

    void setOvpnConfig(const QString& configBase64);

public slots:
    void cancel();

signals:
    void testFinished(bool success, const QString& message, int pingMs);
    void testProgress(const QString& message);
    void realConnectionTestFinished(bool success, const QString& message);

protected:
    void run() override;

private:
    QString serverIp;
    QString serverName;
    QString testOvpnConfig;
    QProcess* process;
    bool isCanceled;
    mutable QMutex mutex;  // Изменили на mutable

    QString findOpenvpn();
    void killAllOpenvpn();
    QPair<bool, QString> testRealOpenvpnConnection(int& connectTime);
    QString enhanceConfig(const QString& config);

    // Вспомогательные функции
    bool isProcessRunning() const;
    void safeCleanup();
};

#endif // SERVERTESTER_H
