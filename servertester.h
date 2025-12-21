#ifndef SERVERTESTER_H
#define SERVERTESTER_H

#include <QThread>
#include <QProcess>
#include <QTemporaryFile>
#include "vpntypes.h"

class ServerTesterThread : public QThread
{
    Q_OBJECT

public:
    explicit ServerTesterThread(const QString& serverIp, const QString& serverName, QObject *parent = nullptr);
    void setOvpnConfig(const QString& configBase64);
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
    QString ovpnConfigBase64;
    bool cancelled;

    bool testPing();
    bool testRealConnection();
    QString enhanceConfigForTest(const QString& configContent);
    void cleanup();
};

#endif // SERVERTESTER_H
