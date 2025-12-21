#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QListWidgetItem>
#include <QProcess>
#include <QThread>
#include <QNetworkAccessManager>
#include <QSettings>
#include <QTimer>
#include <QMenu>
#include <QDialog>
#include <QSet>
#include <QElapsedTimer>
#include <QMap>
#include <QPair>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

#include "vpntypes.h"

// Предварительные объявления классов
class ServerDownloaderThread;
class VpnManager;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    // UI слоты
    void on_refreshButton_clicked();
    void on_connectButton_clicked();
    void on_disconnectButton_clicked();
    void on_clearLogButton_clicked();
    void on_saveLogButton_clicked();
    void on_serverList_itemSelectionChanged();
    void on_autoReconnectCheckbox_stateChanged(int state);
    void on_timeoutSpinBox_valueChanged(int value);
    void on_autoRefreshCheckbox_stateChanged(int state);
    void on_autoRefreshIntervalSpinBox_valueChanged(int value);
    void on_exportConfigButton_clicked();
    void on_shareVPNButton_clicked();
    void on_gatewayStartButton_clicked();
    void on_gatewayStopButton_clicked();

    // Новые слоты для обновленного UI
    void on_resetFailedButton_clicked();
    void on_sortBySpeedButton_clicked();
    void on_sortByPingButton_clicked();
    void on_sortByCountryButton_clicked();
    void on_quickConnectFastButton_clicked();
    void on_quickConnectStableButton_clicked();
    void on_quickConnectRandomButton_clicked();
    void on_createGatewayConfigButton_clicked();

    // Слоты загрузки серверов
    void onServersDownloaded(const QList<VpnServer>& servers);
    void onDownloadError(const QString& error);
    void onDownloadProgress(int progress);
    void onDownloadLog(const QString& message);

    // Слоты VPN подключения
    void onVpnStatus(const QString& type, const QString& message);
    void onVpnLog(const QString& message);
    void onVpnConnected(const QString& serverName);
    void onVpnDisconnected();

    // Слоты таймеров
    void checkConnectionAndReconnect();
    void autoRefreshServers();
    void startAutoReconnect();
    void tryAutoConnect();

    // Новые таймеры
    void updateLogCounter();
    void updateConnectionTimerDisplay();

    // Статистика
    void updateStats();
    void resetFailedServers();

    // Контекстное меню
    void onServerListContextMenu(const QPoint& pos);

    // Управление странами
    void showCountryManager();
    void clearAllBlockedCountries();
    void showBlockedCountries(bool show);

    // Слоты для кнопок управления странами
    void on_countryFilterButton_clicked();

    // Слоты для VPN Gateway
    void onGatewayProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

    // Дополнительные методы для нового функционала
    void updateGatewayInfo();

private:
    Ui::MainWindow *ui;

    // Данные серверов
    QList<VpnServer> servers;
    QSet<QString> failedServers;  // Список неудачных серверов
    QSet<QString> blockedCountries; // Исключенные страны
    int autoConnectIndex;          // Текущий индекс для авто-подключения
    QString currentSortType;       // Текущий тип сортировки

    // Потоки и менеджеры
    ServerDownloaderThread* downloaderThread;
    VpnManager* vpnManager;

    // Настройки и логи
    QSettings* settings;
    QStringList logMessages;
    int logMessageCount;

    // Меню и диалоги
    QMenu* countryFilterMenu;
    QMenu* serverContextMenu;

    // Таймеры и временные переменные
    QElapsedTimer connectionTimer; // Для отслеживания времени подключения
    QString currentAutoConnectServer; // Текущий сервер для авто-подключения
    QMap<QString, int> countryServerCounts; // Статистика серверов по странам

    // Настройки
    bool autoReconnectEnabled;
    bool autoRefreshEnabled;
    int connectionTimeout;
    int refreshIntervalMinutes;
    QTimer* reconnectTimer;
    QTimer* autoRefreshTimer;
    QTimer* connectionUpdateTimer; // Для обновления времени подключения
    QTimer* statsUpdateTimer;      // Для обновления статистики
    QString lastConnectedServerName;
    int reconnectAttempts;         // Счетчик попыток
    bool isAutoReconnecting;       // Флаг авто-подключения

    // VPN Gateway
    QProcess* gatewayProcess;
    bool vpnGatewayEnabled;
    QString gatewayInterface;
    QString localIPAddress;        // Локальный IP адрес для шлюза

    // Методы инициализации
    void initUI();
    void cleanupOldProcesses();
    void addLog(const QString& message, const QString& level = "INFO");
    void saveLogs();
    void saveSettings();
    void loadSettings();
    void initSortButtons();

    // Методы обновления UI
    void updateServerList();
    void updateSelection();
    void updateCountryStats();
    void updateStatusLabel(int displayed, int total, int failed, int blocked);
    void updateConnectionButtons(const QString& status, int displayed);
    void showEmptyListMessage(int displayed, int total, int failed, int blocked);
    void updateGatewayStatus();
    void updateLocalIP();

    // Управление странами
    void initCountryFilterMenu();
    void loadBlockedCountries();
    void saveBlockedCountries();
    void blockCountry(const QString& country);
    void unblockCountry(const QString& country);
    void toggleCountryBlock(const QString& country);
    void updateCountryStatistics();

    // Вспомогательные методы для стран
    QString getCountryCode(const QString& countryName);
    QString getCountryFlag(const QString& countryCode);
    QString getCountryDisplayName(const QString& countryName);

    // Дополнительные методы
    void showConnectionInfo(const VpnServer& server);
    void copyToClipboard(const QString& text, const QString& logMessage);
    void showServerTestDialog(const VpnServer& server);

    // Методы для работы с конфигурациями
    void exportServerConfig(const VpnServer& server);
    void importServerConfigs();
    void exportOpenVPNConfig(const VpnServer& server, const QString& filePath);
    void generateAndroidConfig(const VpnServer& server, const QString& filePath);
    void generateiOSConfig(const VpnServer& server, const QString& filePath);
    void generateWindowsConfig(const VpnServer& server, const QString& filePath);
    void generateRouterConfig(const VpnServer& server, const QString& filePath);
    void showExportMenu(const QPoint& pos, const VpnServer& server);
    void showExportMenu(const QPoint& pos);

    // Новые методы для генерации конфигов шлюза
    void generateGatewayConfig();
    QString getLocalIPAddress() const;

    // Методы для VPN Gateway
    void setupVPNGateway();
    void startVPNGateway();
    void stopVPNGateway();
    bool isVPNGatewayRunning() const;

    // Методы для сортировки и фильтрации
    void sortServersBySpeed();
    void sortServersByPing();
    void sortServersByCountry();
    void filterServersByCountry(const QString& country);
    void clearCountryFilter();

    // Методы для быстрого подключения
    VpnServer findFastestServer() const;
    VpnServer findMostStableServer() const;
    VpnServer findRandomServer() const;

    // Методы для управления кнопками сортировки
    void setSortButtonActive(QPushButton* activeButton);

    // Вспомогательные методы
    int getServerCountByCountry(const QString& country) const;
    int getWorkingServerCount() const;
    int getFailedServerCount() const;
};

#endif // MAINWINDOW_H
