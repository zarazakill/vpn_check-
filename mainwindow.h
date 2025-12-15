#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QListWidgetItem>
#include <QProcess>
#include <QThread>
#include <QNetworkAccessManager>
#include <QSettings>
#include <QTimer>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

// Перенесем определение структур в отдельный файл
#include "vpntypes.h"

// Предварительные объявления классов
class ServerDownloaderThread;
class ServerTesterThread;
class VpnManager;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_refreshButton_clicked();
    void on_stopTestButton_clicked();
    void on_testSelectedButton_clicked();
    void on_connectButton_clicked();
    void on_disconnectButton_clicked();
    void on_clearLogButton_clicked();
    void on_saveLogButton_clicked();
    void on_serverList_itemSelectionChanged();

    void onServersDownloaded(const QList<VpnServer>& servers);
    void onDownloadError(const QString& error);
    void onDownloadProgress(int progress);
    void onDownloadLog(const QString& message);

    void onTestFinished(bool success, const QString& message, int pingMs);
    void onTestProgress(const QString& message);
    void onRealTestFinished(bool success, const QString& message);

    void onVpnStatus(const QString& type, const QString& message);
    void onVpnLog(const QString& message);
    void onVpnConnected(const QString& serverName);
    void onVpnDisconnected();

    void testNextServer();
    void onTestTimeout();

    void updateStats();

private:
    Ui::MainWindow *ui;

    QList<VpnServer> servers;
    QList<VpnServer> workingServers;

    ServerDownloaderThread* downloaderThread;
    ServerTesterThread* testerThread;
    VpnManager* vpnManager;

    bool isTestingAll;
    bool testInProgress;
    int currentTestIndex;
    int totalWorkingServers;
    int totalFailedServers;

    QSettings* settings;
    QStringList logMessages;

    void initUI();
    void updateServerList();
    void updateSelection();
    void startTestingAllServers();
    void finishTesting();
    void stopTesting();
    void cleanupOldProcesses();
    void addLog(const QString& message, const QString& level = "INFO");
    void saveLogs();
    void manualTestServer(const VpnServer& server);
};

#endif // MAINWINDOW_H
