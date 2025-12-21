#ifndef VPNMANAGER_H
#define VPNMANAGER_H

#include <QObject>
#include <QProcess>
#include <QPointer>
#include <QDateTime>
#include "vpntypes.h"

class VpnManager : public QObject {
    Q_OBJECT
public:
    explicit VpnManager(QObject *parent = nullptr);
    void connectToServer(const VpnServer& server);
    void disconnect();
    QPair<QString, QString> getStatus() const;
    QVariantMap getConnectionInfo() const;
    void setConnectionTimeout(int timeout) { connectionTimeout = timeout; }
    bool isConnected() const { return m_isConnected; }

signals:
    void connectionStatus(const QString& type, const QString& message);
    void connectionLog(const QString& message);
    void connected(const QString& serverName);
    void disconnected();
    void connectionEstablished();
    void connectionLost();

private slots:
    void readVpnOutput();
    void vpnProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    QPointer<QProcess> process;
    bool m_isConnected;
    VpnServer currentServer;
    QString configPath;
    int connectionTimeout;
    QDateTime m_lastConnectionTime;

    QString findOpenVPN();
    QString enhanceConfigForConnection(const QString& configContent, const VpnServer& server);
    void cleanup();
};

#endif // VPNMANAGER_H
