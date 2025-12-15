#ifndef VPNMANAGER_H
#define VPNMANAGER_H

#include <QObject>
#include <QProcess>
#include <QMutex>
#include <QPointer>
#include "vpntypes.h"

class VpnManager : public QObject {
    Q_OBJECT

public:
    explicit VpnManager(QObject *parent = nullptr);

    void connectToServer(const VpnServer& server);
    void disconnect();
    QPair<QString, QString> getStatus() const;
    QVariantMap getConnectionInfo() const;

signals:
    void connectionStatus(const QString& type, const QString& message);
    void connectionLog(const QString& message);
    void connected(const QString& serverName);
    void disconnected();

private slots:
    void readVpnOutput();
    void vpnProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    mutable QMutex mutex;  // Для синхронизации доступа к данным
    QPointer<QProcess> process;
    bool isConnected;
    VpnServer currentServer;
    QString configPath;

    QString enhanceConfigForConnection(const QString& configContent, const VpnServer& server);
    void cleanup();
    void safeCleanup();  // Безопасная очистка процессов
};

#endif // VPNMANAGER_H
