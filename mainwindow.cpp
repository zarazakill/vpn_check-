#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "serverdownloader.h"
#include "vpnmanager.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QMessageBox>
#include <QFileDialog>
#include <QTextStream>
#include <QDateTime>
#include <QProcess>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QStyleFactory>
#include <QScrollBar>
#include <QElapsedTimer>
#include <QTemporaryFile>
#include <QRegularExpression>
#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QRandomGenerator>
#include <QClipboard>
#include <QApplication>
#include <QMenu>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QListWidget>
#include <QLabel>
#include <QLinearGradient>

#include <QDebug>

#ifdef Q_OS_LINUX
#include <unistd.h>
#include <sys/types.h>
#endif

MainWindow::MainWindow(QWidget *parent)
: QMainWindow(parent)
, ui(new Ui::MainWindow)
, downloaderThread(nullptr)
, vpnManager(nullptr)
, settings(nullptr)
, countryFilterMenu(nullptr)
, serverContextMenu(nullptr)
, autoReconnectEnabled(false)
, autoRefreshEnabled(false)
, connectionTimeout(45)
, refreshIntervalMinutes(30)
, reconnectTimer(nullptr)
, autoRefreshTimer(nullptr)
, connectionUpdateTimer(nullptr)    // НОВОЕ: инициализируем nullptr
, statsUpdateTimer(nullptr)         // НОВОЕ: инициализируем nullptr
, reconnectAttempts(0)
, isAutoReconnecting(false)
, autoConnectIndex(-1)
, gatewayProcess(nullptr)
, vpnGatewayEnabled(false)
, gatewayInterface("tun0")
, localIPAddress("")                // НОВОЕ: инициализируем пустой строкой
, logMessageCount(0)                // НОВОЕ: инициализируем счетчик
, currentSortType("speed")          // НОВОЕ: инициализируем тип сортировки
{
    try {
        ui->setupUi(this);

        settings = new QSettings("VPNGateManager", "Pro", this);
        vpnManager = new VpnManager(this);
        reconnectTimer = new QTimer(this);
        autoRefreshTimer = new QTimer(this);
        gatewayProcess = new QProcess(this);

        // НОВОЕ: создаем новые таймеры
        connectionUpdateTimer = new QTimer(this);
        statsUpdateTimer = new QTimer(this);

        initUI();
        loadSettings();
        loadBlockedCountries();
        initCountryFilterMenu();
        cleanupOldProcesses();

        QTimer::singleShot(1000, this, &MainWindow::on_refreshButton_clicked);
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, "Ошибка инициализации",
                              QString("Ошибка при запуске: %1").arg(e.what()));
        exit(1);
    }
}

MainWindow::~MainWindow() {
    if (downloaderThread && downloaderThread->isRunning()) {
        downloaderThread->quit();
        if (!downloaderThread->wait(2000)) {
            downloaderThread->terminate();
            downloaderThread->wait(1000);
        }
        delete downloaderThread;
    }

    saveSettings();

    // Останавливаем таймеры
    if (connectionUpdateTimer) {
        connectionUpdateTimer->stop();
        delete connectionUpdateTimer;
    }

    if (statsUpdateTimer) {
        statsUpdateTimer->stop();
        delete statsUpdateTimer;
    }

    if (reconnectTimer) {
        reconnectTimer->stop();
    }

    if (autoRefreshTimer) {
        autoRefreshTimer->stop();
    }

    // Останавливаем VPN Gateway если запущен
    if (vpnGatewayEnabled) {
        stopVPNGateway();
    }

    if (gatewayProcess) {
        delete gatewayProcess;
    }

    delete autoRefreshTimer;
    delete reconnectTimer;
    delete vpnManager;
    delete settings;
    delete ui;
}

void MainWindow::initUI() {
    if (!ui) {
        qCritical() << "UI не инициализирован!";
        return;
    }

    setWindowTitle("VPNGate Manager Pro");

    // Инициализация спинбоксов
    ui->timeoutSpinBox->setRange(30, 180);
    ui->timeoutSpinBox->setValue(45);
    ui->timeoutSpinBox->setEnabled(false);

    ui->autoRefreshIntervalSpinBox->setRange(5, 360);
    ui->autoRefreshIntervalSpinBox->setValue(30);
    ui->autoRefreshIntervalSpinBox->setEnabled(false);

    ui->connectButton->setEnabled(false);
    ui->disconnectButton->setEnabled(false);
    ui->gatewayStopButton->setEnabled(false);
    ui->createGatewayConfigButton->setEnabled(false);

    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(0);

    ui->testLogArea->setFont(QFont("Monospace", 9));
    ui->infoText->setFont(QFont("Monospace", 10));
    ui->logArea->setFont(QFont("Monospace", 9));

    ui->vpnStatusFrame->setVisible(true);

    // Обновляем статистику
    ui->statsLabel->setText("Статус: Загрузка...");
    ui->workingCountLabel->setText("✅ 0 доступно");
    ui->countryCountLabel->setText("🌍 0 стран");
    ui->failedCountLabel->setText("❌ 0 неудачных");
    ui->logCounterLabel->setText("Сообщений: 0");

    qRegisterMetaType<QList<VpnServer>>("QList<VpnServer>");

    if (!vpnManager) {
        qCritical() << "VPN Manager не инициализирован!";
        return;
    }

    connect(vpnManager, &VpnManager::connectionStatus, this, &MainWindow::onVpnStatus);
    connect(vpnManager, &VpnManager::connectionLog, this, &MainWindow::onVpnLog);
    connect(vpnManager, &VpnManager::connected, this, &MainWindow::onVpnConnected);
    connect(vpnManager, &VpnManager::disconnected, this, &MainWindow::onVpnDisconnected);

    // Подключение новых кнопок
    connect(ui->resetFailedButton, &QPushButton::clicked, this, &MainWindow::on_resetFailedButton_clicked);
    connect(ui->sortBySpeedButton, &QPushButton::clicked, this, &MainWindow::on_sortBySpeedButton_clicked);
    connect(ui->sortByPingButton, &QPushButton::clicked, this, &MainWindow::on_sortByPingButton_clicked);
    connect(ui->sortByCountryButton, &QPushButton::clicked, this, &MainWindow::on_sortByCountryButton_clicked);
    connect(ui->quickConnectFastButton, &QPushButton::clicked, this, &MainWindow::on_quickConnectFastButton_clicked);
    connect(ui->quickConnectStableButton, &QPushButton::clicked, this, &MainWindow::on_quickConnectStableButton_clicked);
    connect(ui->quickConnectRandomButton, &QPushButton::clicked, this, &MainWindow::on_quickConnectRandomButton_clicked);
    connect(ui->createGatewayConfigButton, &QPushButton::clicked, this, &MainWindow::on_createGatewayConfigButton_clicked);

    // Подключение кнопок экспорта и шлюза
    connect(ui->exportConfigButton, &QPushButton::clicked, this, &MainWindow::on_exportConfigButton_clicked);
    connect(ui->shareVPNButton, &QPushButton::clicked, this, &MainWindow::on_shareVPNButton_clicked);
    connect(ui->gatewayStartButton, &QPushButton::clicked, this, &MainWindow::on_gatewayStartButton_clicked);
    connect(ui->gatewayStopButton, &QPushButton::clicked, this, &MainWindow::on_gatewayStopButton_clicked);

    // Инициализация сортировки
    initSortButtons();

    // Контекстное меню для списка серверов
    ui->serverList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->serverList, &QWidget::customContextMenuRequested, this, &MainWindow::onServerListContextMenu);

    // Подключение завершения процесса шлюза
    connect(gatewayProcess, &QProcess::finished, this, &MainWindow::onGatewayProcessFinished);

    // Таймеры
    if (!reconnectTimer) {
        qCritical() << "Reconnect timer не инициализирован!";
        return;
    }

    connect(reconnectTimer, &QTimer::timeout, this, &MainWindow::checkConnectionAndReconnect);
    if (autoReconnectEnabled) {
        reconnectTimer->start(15000);
    }

    if (!autoRefreshTimer) {
        qCritical() << "Auto-refresh timer не инициализирован!";
        return;
    }

    connect(autoRefreshTimer, &QTimer::timeout, this, &MainWindow::autoRefreshServers);
    if (autoRefreshEnabled) {
        autoRefreshTimer->start(refreshIntervalMinutes * 60 * 1000);
    }

    // Новые таймеры
    connectionUpdateTimer = new QTimer(this);
    connect(connectionUpdateTimer, &QTimer::timeout, this, &MainWindow::updateConnectionTimerDisplay);
    connectionUpdateTimer->start(1000); // Обновлять каждую секунду

    statsUpdateTimer = new QTimer(this);
    connect(statsUpdateTimer, &QTimer::timeout, this, &MainWindow::updateStats);
    statsUpdateTimer->start(2000); // Обновлять каждые 2 секунды

    // Инициализация счетчика логов
    logMessageCount = 0;

    // Инициализация информации о шлюзе
    updateGatewayInfo();

    qDebug() << "UI инициализирован успешно";
}

void MainWindow::on_refreshButton_clicked() {
    if (downloaderThread && downloaderThread->isRunning()) {
        addLog("Загрузка уже выполняется", "WARNING");
        return;
    }

    ui->refreshButton->setEnabled(false);
    ui->statusLabel->setText("Загрузка списка серверов...");
    ui->testLogArea->clear();

    if (isAutoReconnecting) {
        addLog("🔄 Авто-подключение: обновляю список серверов...", "INFO");
        ui->testLogArea->append("🔄 Авто-подключение: обновляю список серверов...");
    } else {
        addLog("🔄 Загружаю список серверов с VPNGate...", "INFO");
        ui->testLogArea->append("🔄 Загружаю список серверов...");
    }

    downloaderThread = new ServerDownloaderThread(this);
    connect(downloaderThread, &ServerDownloaderThread::downloadFinished,
            this, &MainWindow::onServersDownloaded);
    connect(downloaderThread, &ServerDownloaderThread::downloadError,
            this, &MainWindow::onDownloadError);
    connect(downloaderThread, &ServerDownloaderThread::downloadProgress,
            ui->progressBar, &QProgressBar::setValue);
    connect(downloaderThread, &ServerDownloaderThread::logMessage,
            this, &MainWindow::onDownloadLog);

    downloaderThread->start();
}

void MainWindow::on_connectButton_clicked() {
    int row = ui->serverList->currentRow();
    if (row >= 0 && row < servers.size()) {
        VpnServer server = servers[row];

        // Сбрасываем флаг авто-подключения при ручном подключении
        isAutoReconnecting = false;
        reconnectAttempts = 0;
        autoConnectIndex = -1;

        ui->testLogArea->clear();
        vpnManager->connectToServer(server);
    }
}

void MainWindow::on_disconnectButton_clicked() {
    // Сбрасываем флаг авто-подключения при ручном отключении
    isAutoReconnecting = false;
    reconnectAttempts = 0;
    autoConnectIndex = -1;
    vpnManager->disconnect();
}

void MainWindow::on_clearLogButton_clicked() {
    ui->logArea->clear();
    logMessages.clear();
}

void MainWindow::on_saveLogButton_clicked() {
    saveLogs();
}

void MainWindow::on_serverList_itemSelectionChanged() {
    updateSelection();
}

void MainWindow::on_autoReconnectCheckbox_stateChanged(int state) {
    autoReconnectEnabled = (state == Qt::Checked);

    ui->timeoutSpinBox->setEnabled(autoReconnectEnabled);

    if (autoReconnectEnabled) {
        reconnectTimer->start(15000);
        addLog("Включено авто-подключение при обрыве", "INFO");
    } else {
        reconnectTimer->stop();
        reconnectAttempts = 0;
        isAutoReconnecting = false;
        autoConnectIndex = -1;
        addLog("Отключено авто-подключение", "INFO");
    }

    saveSettings();
}

void MainWindow::on_timeoutSpinBox_valueChanged(int value) {
    connectionTimeout = value;

    vpnManager->setConnectionTimeout(connectionTimeout);

    addLog(QString("Таймаут подключения установлен: %1 секунд").arg(connectionTimeout), "INFO");
    saveSettings();
}

void MainWindow::on_autoRefreshCheckbox_stateChanged(int state) {
    autoRefreshEnabled = (state == Qt::Checked);

    ui->autoRefreshIntervalSpinBox->setEnabled(autoRefreshEnabled);

    if (autoRefreshEnabled) {
        autoRefreshTimer->start(refreshIntervalMinutes * 60 * 1000);
        addLog(QString("Включено авто-обновление серверов каждые %1 минут").arg(refreshIntervalMinutes), "INFO");
    } else {
        autoRefreshTimer->stop();
        addLog("Отключено авто-обновление серверов", "INFO");
    }

    saveSettings();
}

void MainWindow::on_autoRefreshIntervalSpinBox_valueChanged(int value) {
    refreshIntervalMinutes = value;

    if (autoRefreshEnabled) {
        autoRefreshTimer->stop();
        autoRefreshTimer->start(refreshIntervalMinutes * 60 * 1000);
    }

    addLog(QString("Интервал авто-обновления установлен: %1 минут").arg(refreshIntervalMinutes), "INFO");
    saveSettings();
}

void MainWindow::on_exportConfigButton_clicked() {
    int row = ui->serverList->currentRow();
    if (row < 0 || row >= servers.size()) {
        QMessageBox::warning(this, "Выберите сервер",
                             "Пожалуйста, выберите сервер из списка для экспорта конфигурации");
        return;
    }

    VpnServer server = servers[row];
    showExportMenu(ui->serverList->mapFromGlobal(QCursor::pos()));
}

void MainWindow::on_shareVPNButton_clicked() {
    setupVPNGateway();

    QMessageBox::StandardButton reply = QMessageBox::question(this, "VPN Gateway",
                                                              "Запустить VPN Gateway для шаринга подключения?\n\n"
                                                              "Это позволит другим устройствам использовать ваше VPN подключение.\n"
                                                              "Требуются права администратора.",
                                                              QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        startVPNGateway();
    }
}

void MainWindow::on_gatewayStartButton_clicked() {
    on_shareVPNButton_clicked();
}

void MainWindow::on_gatewayStopButton_clicked() {
    stopVPNGateway();
}

void MainWindow::onGatewayProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    Q_UNUSED(exitStatus);

    if (exitCode != 0) {
        addLog(QString("VPN Gateway завершился с ошибкой (код: %1)").arg(exitCode), "ERROR");
        vpnGatewayEnabled = false;
        ui->gatewayStartButton->setEnabled(true);
        ui->gatewayStopButton->setEnabled(false);
        ui->gatewayStatusLabel->setText("Статус: Ошибка");
    }
}

void MainWindow::onDownloadError(const QString& error) {
    addLog(error, "ERROR");
    ui->testLogArea->append(QString("\n❌ Ошибка: %1").arg(error));
    ui->refreshButton->setEnabled(true);
    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(0);
    ui->statusLabel->setText("Ошибка загрузки");

    if (isAutoReconnecting) {
        isAutoReconnecting = false;
        autoConnectIndex = -1;
        addLog("Авто-подключение прервано из-за ошибки загрузки", "ERROR");
    }

    QMessageBox::critical(this, "Ошибка загрузка", error);
}

void MainWindow::onDownloadProgress(int progress) {
    ui->progressBar->setValue(progress);
}

void MainWindow::onDownloadLog(const QString& message) {
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    ui->testLogArea->append(QString("[%1] %2").arg(timestamp).arg(message));
}

void MainWindow::autoRefreshServers() {
    if (!autoRefreshEnabled) return;

    addLog("Авто-обновление списка серверов...", "INFO");
    on_refreshButton_clicked();
}

void MainWindow::startAutoReconnect() {
    if (isAutoReconnecting) {
        addLog("Авто-подключение уже запущено", "DEBUG");
        return;
    }

    isAutoReconnecting = true;
    reconnectAttempts = 0;
    autoConnectIndex = -1;
    currentAutoConnectServer.clear();
    connectionTimer.invalidate();

    addLog("🚀 Запуск авто-подключения...", "INFO");
    addLog(QString("Доступно серверов: %1, неудачных: %2")
    .arg(servers.size())
    .arg(failedServers.size()), "INFO");

    if (servers.isEmpty() || failedServers.size() >= servers.size()) {
        addLog("Список серверов требует обновления...", "INFO");
        failedServers.clear();
        isAutoReconnecting = false;
        on_refreshButton_clicked();

        QTimer::singleShot(5000, this, [this]() {
            if (autoReconnectEnabled) {
                isAutoReconnecting = true;
                startAutoReconnect();
            }
        });
        return;
    }

    ui->statsLabel->setText("Статус: Авто-подключение...");
    QTimer::singleShot(1000, this, &MainWindow::tryAutoConnect);
}

void MainWindow::checkConnectionAndReconnect() {
    if (!autoReconnectEnabled || isAutoReconnecting) return;

    auto status = vpnManager->getStatus();

    if (status.first == "disconnected" &&
        ui->disconnectButton->isEnabled() == false) {

        addLog("Обнаружен обрыв соединения, запускаю авто-подключение...", "WARNING");
    startAutoReconnect();
        }
}

void MainWindow::tryAutoConnect() {
    if (!isAutoReconnecting || !vpnManager) {
        addLog("Авто-подключение отключено или VPN менеджер не инициализирован", "DEBUG");
        return;
    }

    auto status = vpnManager->getStatus();
    addLog(QString("Текущий статус VPN: %1 - %2").arg(status.first).arg(status.second), "DEBUG");

    if (status.first == "connecting") {
        addLog("Уже идет подключение, жду 5 секунд...", "INFO");
        QTimer::singleShot(5000, this, &MainWindow::tryAutoConnect);
        return;
    }

    if (status.first == "connected") {
        addLog(QString("✅ Успешное авто-подключение к %1").arg(status.second), "SUCCESS");

        isAutoReconnecting = false;
        reconnectAttempts = 0;
        autoConnectIndex = -1;

        int failedCount = failedServers.size();
        if (failedCount > 0) {
            failedServers.clear();
            addLog(QString("✅ Очищен список неудачных серверов (%1 серверов)").arg(failedCount), "INFO");
            updateServerList();
        }

        return;
    }

    if (servers.isEmpty()) {
        addLog("Список серверов пуст, обновляю...", "INFO");

        bool wasAutoReconnecting = isAutoReconnecting;
        isAutoReconnecting = false;

        on_refreshButton_clicked();

        QTimer::singleShot(5000, this, [this, wasAutoReconnecting]() {
            if (wasAutoReconnecting && autoReconnectEnabled) {
                isAutoReconnecting = true;
                autoConnectIndex = -1;
                QTimer::singleShot(2000, this, &MainWindow::tryAutoConnect);
            }
        });
        return;
    }

    if (autoConnectIndex < 0 || autoConnectIndex >= servers.size()) {
        autoConnectIndex = servers.size() - 1;
        addLog(QString("Начинаю авто-подключение с конца списка (индекс: %1)").arg(autoConnectIndex), "INFO");
    }

    if (autoConnectIndex < 0) {
        addLog("❌ Все серверы в списке помечены как недоступные", "ERROR");

        isAutoReconnecting = false;
        failedServers.clear();
        addLog("Очищаю список неудачных серверов и обновляю список...", "INFO");

        on_refreshButton_clicked();

        QTimer::singleShot(10000, this, [this]() {
            if (autoReconnectEnabled) {
                isAutoReconnecting = true;
                autoConnectIndex = -1;
                startAutoReconnect();
            }
        });
        return;
    }

    VpnServer selectedServer;
    bool found = false;
    int attempts = 0;
    int startIndex = autoConnectIndex;

    while (autoConnectIndex >= 0 && attempts < servers.size()) {
        VpnServer candidate = servers[autoConnectIndex];

        if (!failedServers.contains(candidate.name)) {
            if (!blockedCountries.contains(candidate.country)) {
                selectedServer = candidate;
                found = true;
                addLog(QString("Выбран сервер: %1 (скорость: %2 Mbps, страна: %3)")
                .arg(candidate.name)
                .arg(candidate.speedMbps, 0, 'f', 1)
                .arg(candidate.country), "INFO");
                break;
            } else {
                addLog(QString("Пропускаем сервер %1: страна %2 заблокирована")
                .arg(candidate.name)
                .arg(candidate.country), "DEBUG");
            }
        }

        autoConnectIndex--;
        attempts++;
    }

    if (!found) {
        addLog("Все серверы в текущем списке недоступны или заблокированы, обновляю список...", "WARNING");

        failedServers.clear();
        isAutoReconnecting = false;

        on_refreshButton_clicked();

        QTimer::singleShot(5000, this, [this]() {
            if (autoReconnectEnabled) {
                isAutoReconnecting = true;
                autoConnectIndex = servers.size() - 1;
                QTimer::singleShot(2000, this, &MainWindow::tryAutoConnect);
            }
        });
        return;
    }

    if (selectedServer.name.isEmpty()) {
        addLog("Выбран невалидный сервер, пробую следующий...", "WARNING");
        autoConnectIndex--;
        QTimer::singleShot(2000, this, &MainWindow::tryAutoConnect);
        return;
    }

    reconnectAttempts++;

    addLog(QString("Попытка авто-подключения #%1: %2 (%3, %4 Mbps)")
    .arg(reconnectAttempts)
    .arg(selectedServer.name)
    .arg(selectedServer.country)
    .arg(selectedServer.speedMbps, 0, 'f', 1), "INFO");

    for (int i = 0; i < servers.size(); ++i) {
        if (servers[i].name == selectedServer.name) {
            ui->serverList->setCurrentRow(i);
            break;
        }
    }

    QTimer::singleShot(2000, this, [this, selectedServer, startIndex]() {
        auto currentStatus = vpnManager->getStatus();
        if (currentStatus.first == "connecting" || currentStatus.first == "connected") {
            addLog("Уже идет подключение или подключено, отменяю...", "INFO");
            return;
        }

        vpnManager->connectToServer(selectedServer);

        int checkTimeout = (connectionTimeout + 20) * 1000;

        QTimer::singleShot(checkTimeout, this, [this, selectedServer, startIndex]() {
            if (!isAutoReconnecting) {
                return;
            }

            auto currentStatus = vpnManager->getStatus();

            if (currentStatus.first != "connected") {
                addLog(QString("❌ Не удалось подключиться к %1 за %2 секунд")
                .arg(selectedServer.name)
                .arg(connectionTimeout + 20), "WARNING");

                failedServers.insert(selectedServer.name);
                updateServerList();
                autoConnectIndex--;

                if (autoConnectIndex < 0) {
                    autoConnectIndex = servers.size() - 1;
                    addLog("Достигнут конец списка, начинаю с начала...", "INFO");
                }

                QTimer::singleShot(5000, this, &MainWindow::tryAutoConnect);
            } else {
                addLog(QString("✅ Успешное подключение к %1").arg(selectedServer.name), "SUCCESS");

                isAutoReconnecting = false;
                autoConnectIndex = -1;
                failedServers.clear();
                updateServerList();
            }
        });

        QTimer::singleShot(60000, this, [this, selectedServer]() {
            if (!isAutoReconnecting) {
                return;
            }

            auto currentStatus = vpnManager->getStatus();
            if (currentStatus.first == "connected") {
                addLog(QString("✅ Стабильное подключение к %1 (60+ секунд)")
                .arg(selectedServer.name), "SUCCESS");
                isAutoReconnecting = false;
                autoConnectIndex = -1;
                failedServers.clear();
            }
        });
    });
}

void MainWindow::onServersDownloaded(const QList<VpnServer>& downloadedServers) {
    QList<VpnServer> filteredServers;

    for (const VpnServer& server : downloadedServers) {
        if (blockedCountries.contains(server.country)) {
            addLog(QString("Пропущен сервер %1: страна %2 заблокирована")
            .arg(server.name).arg(server.country), "DEBUG");
            continue;
        }

        if (failedServers.contains(server.name)) {
            continue;
        }

        filteredServers.append(server);
    }

    this->servers = filteredServers;

    std::sort(this->servers.begin(), this->servers.end(),
              [](const VpnServer& a, const VpnServer& b) {
                  return a.speedMbps > b.speedMbps;
              });

    updateServerList();

    QSet<QString> countries;
    for (const VpnServer& s : filteredServers) {
        countries.insert(s.country);
    }

    int totalServers = filteredServers.size();
    ui->statusLabel->setText(QString("Готово: %1 серверов из %2 стран").arg(totalServers).arg(countries.size()));
    ui->statsLabel->setText("Статус: Загрузка завершена");
    ui->workingCountLabel->setText(QString("📊 %1 серверов").arg(totalServers));
    ui->countryCountLabel->setText(QString("🌍 %1 стран").arg(countries.size()));

    ui->refreshButton->setEnabled(true);
    ui->progressBar->setValue(100);

    if (isAutoReconnecting) {
        autoConnectIndex = this->servers.size() - 1;

        if (autoConnectIndex >= 0) {
            addLog(QString("Авто-подключение: найдено %1 доступных серверов")
            .arg(this->servers.size()), "INFO");
            QTimer::singleShot(2000, this, &MainWindow::tryAutoConnect);
        } else {
            addLog("Нет доступных серверов для подключения", "ERROR");
            isAutoReconnecting = false;
            autoConnectIndex = -1;
        }
    }
    else if (!autoRefreshEnabled && !isAutoReconnecting) {
        if (!filteredServers.isEmpty()) {
            QMessageBox::information(this, "Загрузка завершена",
                                     QString("✅ Загружено %1 VPN серверов из %2 стран\n\n"
                                     "⚡ Самый быстрый сервер:\n"
                                     "   • %3\n"
                                     "   • Страна: %4\n"
                                     "   • Скорость: %5 Mbps")
                                     .arg(totalServers)
                                     .arg(countries.size())
                                     .arg(filteredServers[0].name)
                                     .arg(filteredServers[0].country)
                                     .arg(filteredServers[0].speedMbps));
        }
    }
}

void MainWindow::onVpnStatus(const QString& type, const QString& message) {
    QString icon;
    QString color;

    if (type == "success") {
        icon = "🟢";
        color = "#28a745";
        ui->vpnStatusLabel->setText(QString("%1 VPN: %2").arg(icon).arg(message));
        ui->vpnStatusLabel->setStyleSheet(QString("color: %1; font-weight: bold;").arg(color));
    } else if (type == "error") {
        icon = "🔴";
        color = "#dc3545";
        ui->vpnStatusLabel->setText(QString("%1 VPN: %2").arg(icon).arg(message));
        ui->vpnStatusLabel->setStyleSheet(QString("color: %1; font-weight: bold;").arg(color));

        if (isAutoReconnecting) {
            int currentRow = ui->serverList->currentRow();
            if (currentRow >= 0 && currentRow < servers.size()) {
                VpnServer failedServer = servers[currentRow];

                failedServers.insert(failedServer.name);
                addLog(QString("❌ Сервер %1 помечен как недоступный")
                .arg(failedServer.name), "ERROR");

                updateServerList();
                QTimer::singleShot(2000, this, &MainWindow::tryAutoConnect);
            }
        }
    } else if (type == "warning") {
        icon = "🟡";
        color = "#ffc107";
        ui->vpnStatusLabel->setText(QString("%1 VPN: %2").arg(icon).arg(message));
        ui->vpnStatusLabel->setStyleSheet(QString("color: %1; font-weight: bold;").arg(color));
    } else if (type == "info") {
        icon = "🔵";
        color = "#007bff";
        ui->vpnStatusLabel->setText(QString("%1 VPN: %2").arg(icon).arg(message));
        ui->vpnStatusLabel->setStyleSheet(QString("color: %1; font-weight: bold;").arg(color));
    }
}

void MainWindow::onVpnLog(const QString& message) {
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    ui->testLogArea->append(QString("[%1] %2").arg(timestamp).arg(message));
}

void MainWindow::onVpnConnected(const QString& serverName) {
    ui->connectButton->setEnabled(false);
    ui->disconnectButton->setEnabled(true);
    ui->vpnStatusFrame->setVisible(true);

    connectionTimer.start();

    if (isAutoReconnecting) {
        currentAutoConnectServer = serverName;
        addLog(QString("✅ Авто-подключение к %1 установлено").arg(serverName), "SUCCESS");

        QTimer::singleShot(30000, this, [this, serverName]() {
            if (isAutoReconnecting && connectionTimer.isValid() &&
                connectionTimer.elapsed() > 25000) {
                addLog(QString("✅ Авто-подключение к %1 стабильно (30+ секунд)")
                .arg(serverName), "SUCCESS");

            isAutoReconnecting = false;
            reconnectAttempts = 0;
            autoConnectIndex = -1;
            currentAutoConnectServer.clear();

            int failedCount = failedServers.size();
            if (failedCount > 0) {
                failedServers.clear();
                updateServerList();
                addLog(QString("✅ Очищен список неудачных серверов (%1)")
                .arg(failedCount), "INFO");
            }
                }
        });
    }

    reconnectAttempts = 0;

    if (isAutoReconnecting) {
        isAutoReconnecting = false;
        failedServers.clear();
        autoConnectIndex = -1;
        addLog("✅ Авто-подключение успешно завершено!", "SUCCESS");
    }

    QVariantMap info = vpnManager->getConnectionInfo();
    if (!info.isEmpty()) {
        QString infoText = QString("🔗 %1 | 🌍 %2 | 🌐 %3")
        .arg(info["server"].toString())
        .arg(info["country"].toString())
        .arg(info["ip"].toString());
        ui->vpnInfoLabel->setText(infoText);
    }

    if (vpnGatewayEnabled) {
        ui->gatewayStatusLabel->setText("Статус: VPN подключен + Gateway активен");
    }

    updateServerList();
}

void MainWindow::onVpnDisconnected() {
    ui->connectButton->setEnabled(true);
    ui->disconnectButton->setEnabled(false);
    ui->vpnInfoLabel->setText("");

    if (isAutoReconnecting && !currentAutoConnectServer.isEmpty()) {
        addLog(QString("❌ Авто-подключение к %1 разорвано")
        .arg(currentAutoConnectServer), "WARNING");

        failedServers.insert(currentAutoConnectServer);
        currentAutoConnectServer.clear();
        connectionTimer.invalidate();

        updateServerList();
        QTimer::singleShot(5000, this, &MainWindow::tryAutoConnect);
    }

    if (vpnGatewayEnabled) {
        addLog("VPN отключен, останавливаю Gateway...", "WARNING");
        stopVPNGateway();
    }

    ui->gatewayStatusLabel->setText("Статус: Остановлен");

    updateServerList();
}

void MainWindow::updateStats() {
    int totalServers = servers.size();

    if (totalServers > 0) {
        QSet<QString> countries;
        for (const VpnServer& s : servers) {
            countries.insert(s.country);
        }
        ui->countryCountLabel->setText(QString("🌍 %1 стран").arg(countries.size()));
    }
}

void MainWindow::updateServerList() {
    ui->serverList->clear();

    auto status = vpnManager->getStatus();
    QString currentVpnServer = status.first == "connected" ? status.second : QString();

    int totalDisplayed = 0;
    int failedCount = 0;
    int blockedCountryCount = 0;
    int totalServers = servers.size();

    for (const VpnServer& server : servers) {
        bool isFailed = failedServers.contains(server.name);
        bool isCountryBlocked = blockedCountries.contains(server.country);
        bool isConnected = (currentVpnServer == server.name);
        bool isAutoConnecting = (isAutoReconnecting && autoConnectIndex >= 0 &&
        autoConnectIndex < servers.size() &&
        servers[autoConnectIndex].name == server.name);

        if (isFailed) {
            failedCount++;
        } else if (isCountryBlocked) {
            blockedCountryCount++;
        }

        if (isFailed || isCountryBlocked) {
            continue;
        }

        QString statusIcon;
        QString speedColor;
        QString speedClass;

        double speed = server.speedMbps;
        if (speed > 100) {
            statusIcon = "⚡⚡";
            speedColor = "#0056b3";
            speedClass = "very-fast";
        } else if (speed > 50) {
            statusIcon = "⚡";
            speedColor = "#28a745";
            speedClass = "fast";
        } else if (speed > 20) {
            statusIcon = "🟢";
            speedColor = "#20c997";
            speedClass = "medium";
        } else if (speed > 5) {
            statusIcon = "🟡";
            speedColor = "#ffc107";
            speedClass = "slow";
        } else {
            statusIcon = "🔴";
            speedColor = "#dc3545";
            speedClass = "very-slow";
        }

        QString countryFlag = getCountryFlag(getCountryCode(server.country));

        QString currentMarker = isConnected ? " 🔗" : "";
        QString autoConnectMarker = isAutoConnecting ? " 🔄" : "";
        QString failedMarker = isFailed ? " ❌" : "";
        QString blockedMarker = isCountryBlocked ? " 🚫" : "";

        QString displayName = QString("%1 %2 %3 | %4 Mbps | %5%6%7%8%9")
        .arg(statusIcon)
        .arg(countryFlag)
        .arg(server.name)
        .arg(server.speedMbps, 0, 'f', 1)
        .arg(server.country)
        .arg(currentMarker)
        .arg(autoConnectMarker)
        .arg(failedMarker)
        .arg(blockedMarker);

        QListWidgetItem* item = new QListWidgetItem(displayName);

        item->setForeground(QColor(speedColor));

        QString tooltip = QString("Сервер: %1\n"
        "Страна: %2 %3\n"
        "IP: %4\n"
        "Порт: %5 (%6)\n"
        "Скорость: %7 Mbps (%8)\n"
        "Пинг: %9 ms\n"
        "Рейтинг: %10/100\n"
        "Сессии: %11\n"
        "Аптайм: %12")
        .arg(server.name)
        .arg(countryFlag)
        .arg(server.country)
        .arg(server.ip)
        .arg(server.port)
        .arg(server.protocol.toUpper())
        .arg(server.speedMbps, 0, 'f', 1)
        .arg(speedClass)
        .arg(server.ping)
        .arg(server.score)
        .arg(server.sessions)
        .arg(server.uptime);

        if (isConnected) {
            tooltip += QString("\n\n🔗 Текущее подключение");
            if (connectionTimer.isValid()) {
                int seconds = connectionTimer.elapsed() / 1000;
                int minutes = seconds / 60;
                seconds %= 60;
                tooltip += QString("\n⏱️ Время подключения: %1:%2")
                .arg(minutes, 2, 10, QChar('0'))
                .arg(seconds, 2, 10, QChar('0'));
            }
        }

        if (isAutoConnecting) {
            tooltip += QString("\n\n🔄 Авто-подключение: попытка #%1")
            .arg(reconnectAttempts + 1);
        }

        if (isFailed) {
            tooltip += QString("\n\n❌ Сервер помечен как недоступный");
        }

        if (isCountryBlocked) {
            tooltip += QString("\n\n🚫 Страна исключена из списка");
        }

        item->setToolTip(tooltip);

        if (isConnected) {
            item->setBackground(QColor("#d4edda"));
            item->setFont(QFont("", -1, QFont::Bold));
        } else if (isAutoConnecting) {
            item->setBackground(QColor("#fff3cd"));
            item->setFont(QFont("", -1, QFont::Bold));
        } else if (isFailed) {
            item->setBackground(QColor("#f8d7da"));
            item->setForeground(QColor("#721c24"));
        } else if (isCountryBlocked) {
            item->setBackground(QColor("#e2e3e5"));
            item->setForeground(QColor("#383d41"));
        } else {
            QLinearGradient gradient(0, 0, ui->serverList->width(), 0);

            if (speedClass == "very-fast") {
                gradient.setColorAt(0, QColor("#d1ecf1"));
                gradient.setColorAt(1, QColor("#ffffff"));
            } else if (speedClass == "fast") {
                gradient.setColorAt(0, QColor("#d4edda"));
                gradient.setColorAt(1, QColor("#ffffff"));
            } else if (speedClass == "medium") {
                gradient.setColorAt(0, QColor("#fff3cd"));
                gradient.setColorAt(1, QColor("#ffffff"));
            } else {
                gradient.setColorAt(0, QColor("#ffffff"));
                gradient.setColorAt(1, QColor("#ffffff"));
            }

            QBrush brush(gradient);
            item->setBackground(brush);
        }

        ui->serverList->addItem(item);
        totalDisplayed++;
    }

    ui->serverList->setCurrentRow(-1);
    ui->infoText->clear();

    updateStatusLabel(totalDisplayed, totalServers, failedCount, blockedCountryCount);
    updateConnectionButtons(status.first, totalDisplayed);
    showEmptyListMessage(totalDisplayed, totalServers, failedCount, blockedCountryCount);
    updateCountryStats();
}

void MainWindow::updateStatusLabel(int displayed, int total, int failed, int blocked) {
    QStringList statsParts;

    statsParts << QString("%1/%2 серверов").arg(displayed).arg(total);

    if (failed > 0) {
        statsParts << QString("❌ %1").arg(failed);
    }

    if (blocked > 0) {
        statsParts << QString("🚫 %1").arg(blocked);
    }

    auto status = vpnManager->getStatus();
    if (status.first == "connected") {
        QString timeStr = "";
        if (connectionTimer.isValid()) {
            int seconds = connectionTimer.elapsed() / 1000;
            int minutes = seconds / 60;
            seconds %= 60;
            timeStr = QString(" (%1:%2)").arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
        }
        statsParts << QString("🔗 %1%2").arg(status.second).arg(timeStr);
    } else if (status.first == "connecting") {
        statsParts << "🔄 Подключение...";
    }

    if (isAutoReconnecting) {
        statsParts << QString("🔄 #%1").arg(reconnectAttempts);
    }

    ui->statsLabel->setText(QString("Статус: %1").arg(statsParts.join(" | ")));
}

void MainWindow::updateConnectionButtons(const QString& status, int displayed) {
    if (status == "connected") {
        ui->connectButton->setEnabled(false);
        ui->disconnectButton->setEnabled(true);
    } else if (status == "disconnected" && displayed > 0) {
        ui->connectButton->setEnabled(true);
        ui->disconnectButton->setEnabled(false);
    } else if (status == "connecting") {
        ui->connectButton->setEnabled(false);
        ui->disconnectButton->setEnabled(true);
    } else {
        ui->connectButton->setEnabled(false);
        ui->disconnectButton->setEnabled(false);
    }
}

void MainWindow::showEmptyListMessage(int displayed, int total, int failed, int blocked) {
    if (displayed == 0) {
        QString message;

        if (total == 0) {
            message = "<div style='text-align: center; padding: 30px; color: #6c757d;'>"
            "👆 Нажмите <b>Обновить список</b><br>"
            "для загрузки серверов"
            "</div>";
        } else if (blocked > 0 && failed == 0) {
            message = QString("<div style='text-align: center; padding: 30px; color: #6c757d;'>"
            "📭 Нет доступных серверов<br>"
            "Все серверы (%1) исключены по странам<br><br>"
            "Используйте <b>Фильтр стран</b><br>"
            "чтобы разрешить некоторые страны"
            "</div>").arg(total);
        } else if (failed > 0 && blocked == 0) {
            message = QString("<div style='text-align: center; padding: 30px; color: #6c757d;'>"
            "📭 Нет доступных серверов<br>"
            "Все серверы (%1) помечены как недоступные<br><br>"
            "Попробуйте обновить список<br>"
            "или сбросить список неудачных серверов"
            "</div>").arg(total);
        } else if (failed > 0 && blocked > 0) {
            message = QString("<div style='text-align: center; padding: 30px; color: #6c757d;'>"
            "📭 Нет доступных серверов<br>"
            "Серверы исключены: %1 по странам, %2 как недоступные<br><br>"
            "Используйте <b>Фильтр стран</b> или обновите список"
            "</div>").arg(blocked).arg(failed);
        }

        ui->infoText->setHtml(message);
    }
}

void MainWindow::updateSelection() {
    int currentRow = ui->serverList->currentRow();

    if (currentRow >= 0 && currentRow < servers.size()) {
        VpnServer server = servers[currentRow];

        QString infoText = QString(
            "<style>"
            "h3 { color: #2c3e50; margin-bottom: 10px; }"
            ".info-block { background-color: #f8f9fa; border-left: 4px solid #007bff; padding: 10px; margin: 10px 0; border-radius: 4px; }"
            ".label { font-weight: bold; color: #495057; }"
            ".value { color: #212529; }"
            ".connected { color: #28a745; font-weight: bold; }"
            ".speed-good { color: #28a745; }"
            ".speed-medium { color: #ffc107; }"
            ".speed-slow { color: #dc3545; }"
            "</style>"

            "<div class='info-block'>"
            "<div><span class='label'>📡 Сервер:</span> <span class='value'>%1</span></div>"
            "<div><span class='label'>🌍 Страна:</span> <span class='value'>%2</span></div>"
            "<div><span class='label'>🌐 IP адрес:</span> <span class='value'>%3</span></div>"
            "<div><span class='label'>⚡ Скорость:</span> <span class='value speed-%4'>%5 Mbps</span></div>"
            "<div><span class='label'>⏱️ Пинг:</span> <span class='value'>%6 ms</span></div>"
            "<div><span class='label'>⭐ Рейтинг:</span> <span class='value'>%7</span></div>"
            "<div><span class='label'>👥 Сессии:</span> <span class='value'>%8</span></div>"
            "<div><span class='label'>🕒 Аптайм:</span> <span class='value'>%9</span></div>"
            "</div>"
        )
        .arg(server.name)
        .arg(server.country)
        .arg(server.ip)
        .arg(server.speedMbps > 50 ? "good" : server.speedMbps > 10 ? "medium" : "slow")
        .arg(server.speedMbps, 0, 'f', 1)
        .arg(server.ping)
        .arg(server.score)
        .arg(server.sessions)
        .arg(server.uptime);

        auto status = vpnManager->getStatus();
        if (status.first == "connected" && server.name == status.second) {
            infoText += "<div style='margin-top: 10px; padding: 8px; background-color: #d4edda; border: 1px solid #c3e6cb; border-radius: 4px;'>"
            "✅ <span class='connected'>Подключен к этому серверу</span>"
            "</div>";
        }

        ui->infoText->setHtml(infoText);

        if (status.first == "disconnected") {
            ui->connectButton->setEnabled(true);
        }
    } else {
        ui->infoText->setHtml("<div style='text-align: center; padding: 20px; color: #6c757d;'>"
        "👆 Выберите сервер из списка<br>для просмотра информации"
        "</div>");
        ui->connectButton->setEnabled(false);
    }
}

void MainWindow::cleanupOldProcesses() {
    QProcess process;
    #ifdef Q_OS_LINUX
    process.start("sudo", QStringList() << "pkill" << "-f" << "openvpn.*tun999");
    process.waitForFinished(1000);
    process.start("sudo", QStringList() << "pkill" << "-f" << "openvpn.*vpngate");
    process.waitForFinished(1000);
    process.start("sudo", QStringList() << "pkill" << "-9" << "-f" << "openvpn");
    process.waitForFinished(1000);
    #endif
}

void MainWindow::addLog(const QString& message, const QString& level) {
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    QString prefix;
    QString color;

    if (level == "ERROR") {
        prefix = "❌";
        color = "#dc3545";
    } else if (level == "WARNING") {
        prefix = "⚠️";
        color = "#ffc107";
    } else if (level == "SUCCESS") {
        prefix = "✅";
        color = "#28a745";
    } else if (level == "INFO") {
        prefix = "ℹ️";
        color = "#17a2b8";
    } else {
        prefix = "📝";
        color = "#6c757d";
    }

    QString logEntry = QString("<span style='color: %3;'>[%1]</span> <span style='font-weight: bold;'>%2</span> %4")
    .arg(timestamp)
    .arg(prefix)
    .arg("#6c757d")
    .arg(message);

    // Проверяем на дубликаты (игнорируем таймстамп и префикс)
    QString cleanMessage = message;
    for (const QString& existingLog : logMessages) {
        if (existingLog.contains(cleanMessage)) {
            return; // Пропускаем дубликат
        }
    }

    logMessages.append(logEntry);
    logMessageCount++;

    if (logMessages.size() > 1000) {
        logMessages = logMessages.mid(logMessages.size() - 1000);
        logMessageCount = 1000;
    }

    QString htmlEntry = QString("<div style='margin: 2px 0; color: %1;'>[%2] <b>%3</b> %4</div>")
    .arg(color)
    .arg(timestamp)
    .arg(prefix)
    .arg(message);

    ui->logArea->append(htmlEntry);

    // Автопрокрутка к последнему сообщению
    QTextCursor cursor = ui->logArea->textCursor();
    cursor.movePosition(QTextCursor::End);
    ui->logArea->setTextCursor(cursor);

    // Обновляем счетчик
    updateLogCounter();
}

void MainWindow::saveLogs() {
    QString downloadFolder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/vpngate_logs";
    QDir dir;
    if (!dir.exists(downloadFolder)) {
        dir.mkpath(downloadFolder);
    }

    QString logFile = downloadFolder + "/vpngate_" +
    QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".log";

    QFile file(logFile);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&file);

        for (const QString& log : logMessages) {
            QString plainLog = log;
            plainLog.remove(QRegularExpression("<[^>]*>"));
            stream << plainLog << "\n";
        }

        file.close();

        addLog(QString("Лог сохранен: %1").arg(logFile), "SUCCESS");
        QMessageBox::information(this, "Успех",
                                 QString("📁 Лог успешно сохранен:\n%1").arg(logFile));
    } else {
        QMessageBox::warning(this, "Ошибка", "Не удалось сохранить лог");
    }
}

void MainWindow::saveSettings() {
    settings->setValue("autoReconnect", autoReconnectEnabled);
    settings->setValue("connectionTimeout", connectionTimeout);
    settings->setValue("autoRefresh", autoRefreshEnabled);
    settings->setValue("refreshInterval", refreshIntervalMinutes);
    settings->setValue("lastConnectedServer", lastConnectedServerName);
    settings->sync();
}

void MainWindow::loadSettings() {
    autoReconnectEnabled = settings->value("autoReconnect", false).toBool();
    connectionTimeout = settings->value("connectionTimeout", 45).toInt();
    autoRefreshEnabled = settings->value("autoRefresh", false).toBool();
    refreshIntervalMinutes = settings->value("refreshInterval", 30).toInt();
    lastConnectedServerName = settings->value("lastConnectedServer", "").toString();

    ui->autoReconnectCheckbox->setChecked(autoReconnectEnabled);
    ui->timeoutSpinBox->setValue(connectionTimeout);
    ui->timeoutSpinBox->setEnabled(autoReconnectEnabled);

    ui->autoRefreshCheckbox->setChecked(autoRefreshEnabled);
    ui->autoRefreshIntervalSpinBox->setValue(refreshIntervalMinutes);
    ui->autoRefreshIntervalSpinBox->setEnabled(autoRefreshEnabled);

    vpnManager->setConnectionTimeout(connectionTimeout);

    if (autoReconnectEnabled) {
        reconnectTimer->start(15000);
    }

    if (autoRefreshEnabled) {
        autoRefreshTimer->start(refreshIntervalMinutes * 60 * 1000);
    }
}

void MainWindow::resetFailedServers() {
    int count = failedServers.size();
    failedServers.clear();
    addLog(QString("✅ Список неудачных серверов очищен (%1 серверов)").arg(count), "SUCCESS");

    updateServerList();

    if (isAutoReconnecting) {
        autoConnectIndex = servers.size() - 1;
        addLog("Индекс авто-подключения сброшен", "INFO");
    }

    updateStats();
}

void MainWindow::initCountryFilterMenu() {
    QPushButton* filterButton = ui->countryFilterButton;

    filterButton->setCursor(Qt::PointingHandCursor);

    countryFilterMenu = new QMenu(this);

    QAction* manageAction = new QAction("⚙️ Управление исключенными странами", this);
    QAction* clearBlockedAction = new QAction("🗑️ Очистить все исключения", this);
    QAction* showBlockedAction = new QAction("👁️ Показать исключенные", this);
    showBlockedAction->setCheckable(true);
    showBlockedAction->setChecked(false);

    countryFilterMenu->addAction(manageAction);
    countryFilterMenu->addAction(clearBlockedAction);
    countryFilterMenu->addSeparator();
    countryFilterMenu->addAction(showBlockedAction);

    filterButton->setMenu(countryFilterMenu);

    connect(manageAction, &QAction::triggered, this, &MainWindow::showCountryManager);
    connect(clearBlockedAction, &QAction::triggered, this, &MainWindow::clearAllBlockedCountries);
    connect(showBlockedAction, &QAction::toggled, this, &MainWindow::showBlockedCountries);
}

void MainWindow::showCountryManager() {
    QDialog dialog(this);
    dialog.setWindowTitle("🌍 Управление исключенными странами");
    dialog.setMinimumSize(500, 400);

    QVBoxLayout* layout = new QVBoxLayout(&dialog);

    QSet<QString> allCountriesSet;
    for (const VpnServer& server : servers) {
        allCountriesSet.insert(server.country);
    }

    QLabel* statsLabel = new QLabel(
        QString("Исключено стран: %1 из %2 найденных")
        .arg(blockedCountries.size())
        .arg(allCountriesSet.size()),
                                    &dialog
    );
    statsLabel->setStyleSheet("font-weight: bold; color: #6c757d; padding: 5px;");
    layout->addWidget(statsLabel);

    QListWidget* countryList = new QListWidget(&dialog);
    countryList->setSelectionMode(QListWidget::MultiSelection);

    QSet<QString> allCountries;
    for (const VpnServer& server : servers) {
        allCountries.insert(server.country);
    }

    QMap<QString, int> countryServerCount;
    for (const VpnServer& server : servers) {
        countryServerCount[server.country]++;
    }

    QList<QPair<QString, int>> sortedCountries;
    for (const QString& country : allCountries) {
        sortedCountries.append(qMakePair(country, countryServerCount[country]));
    }

    std::sort(sortedCountries.begin(), sortedCountries.end(),
              [](const QPair<QString, int>& a, const QPair<QString, int>& b) {
                  return b.second < a.second;
              });

    for (const auto& pair : sortedCountries) {
        QString country = pair.first;
        int serverCount = pair.second;
        QString countryCode = getCountryCode(country);
        QString flag = getCountryFlag(countryCode);

        QString displayText = QString("%1 %2 (%3 серверов)")
        .arg(flag)
        .arg(country)
        .arg(serverCount);

        QListWidgetItem* item = new QListWidgetItem(displayText);
        item->setData(Qt::UserRole, country);
        item->setData(Qt::UserRole + 1, serverCount);
        item->setCheckState(blockedCountries.contains(country) ? Qt::Checked : Qt::Unchecked);

        if (blockedCountries.contains(country)) {
            item->setForeground(QColor("#dc3545"));
            item->setBackground(QColor("#f8d7da"));
        } else {
            item->setForeground(QColor("#212529"));
            item->setBackground(serverCount > 10 ? QColor("#f8f9fa") : QColor("#ffffff"));
        }

        countryList->addItem(item);
    }

    layout->addWidget(countryList);

    QHBoxLayout* quickButtonsLayout = new QHBoxLayout();

    QPushButton* blockCensoredBtn = new QPushButton("🚫 Страны с цензурой", &dialog);
    QPushButton* blockStreamingBtn = new QPushButton("🎬 Блокировка стриминга", &dialog);
    QPushButton* blockCommonBtn = new QPushButton("🔒 Популярные для VPN", &dialog);

    blockCensoredBtn->setStyleSheet("QPushButton { padding: 5px; font-size: 11px; }");
    blockStreamingBtn->setStyleSheet("QPushButton { padding: 5px; font-size: 11px; }");
    blockCommonBtn->setStyleSheet("QPushButton { padding: 5px; font-size: 11px; }");

    quickButtonsLayout->addWidget(blockCensoredBtn);
    quickButtonsLayout->addWidget(blockStreamingBtn);
    quickButtonsLayout->addWidget(blockCommonBtn);
    quickButtonsLayout->addStretch();

    layout->addLayout(quickButtonsLayout);

    QHBoxLayout* buttonLayout = new QHBoxLayout();

    QPushButton* blockSelectedBtn = new QPushButton("🚫 Исключить выбранные", &dialog);
    QPushButton* unblockSelectedBtn = new QPushButton("✅ Разблокировать выбранные", &dialog);
    QPushButton* closeBtn = new QPushButton("Закрыть", &dialog);

    buttonLayout->addWidget(blockSelectedBtn);
    buttonLayout->addWidget(unblockSelectedBtn);
    buttonLayout->addStretch();
    buttonLayout->addWidget(closeBtn);

    layout->addLayout(buttonLayout);

    connect(blockSelectedBtn, &QPushButton::clicked, &dialog, [this, countryList, &dialog]() {
        int blocked = 0;
        for (int i = 0; i < countryList->count(); ++i) {
            QListWidgetItem* item = countryList->item(i);
            if (item->isSelected()) {
                QString country = item->data(Qt::UserRole).toString();
                if (!blockedCountries.contains(country)) {
                    blockCountry(country);
                    item->setCheckState(Qt::Checked);
                    item->setForeground(QColor("#dc3545"));
                    item->setBackground(QColor("#f8d7da"));
                    blocked++;
                }
            }
        }
        if (blocked > 0) {
            QMessageBox::information(&dialog, "Готово",
                                     QString("Заблокировано %1 стран").arg(blocked));
        }
    });

    connect(unblockSelectedBtn, &QPushButton::clicked, &dialog, [this, countryList, &dialog]() {
        int unblocked = 0;
        for (int i = 0; i < countryList->count(); ++i) {
            QListWidgetItem* item = countryList->item(i);
            if (item->isSelected()) {
                QString country = item->data(Qt::UserRole).toString();
                if (blockedCountries.contains(country)) {
                    unblockCountry(country);
                    item->setCheckState(Qt::Unchecked);
                    item->setForeground(QColor("#212529"));
                    item->setBackground(QColor("#ffffff"));
                    unblocked++;
                }
            }
        }
        if (unblocked > 0) {
            QMessageBox::information(&dialog, "Готово",
                                     QString("Разблокировано %1 стран").arg(unblocked));
        }
    });

    connect(blockCensoredBtn, &QPushButton::clicked, &dialog, [this, countryList, &dialog]() {
        QStringList censoredCountries = {
            "China", "Russia", "Iran", "North Korea", "Cuba",
            "Vietnam", "Saudi Arabia", "United Arab Emirates", "Pakistan",
            "Turkmenistan", "Uzbekistan", "Belarus", "Syria"
        };

        int blocked = 0;
        for (int i = 0; i < countryList->count(); ++i) {
            QListWidgetItem* item = countryList->item(i);
            QString country = item->data(Qt::UserRole).toString();

            for (const QString& censored : censoredCountries) {
                if (country.contains(censored, Qt::CaseInsensitive)) {
                    if (!blockedCountries.contains(country)) {
                        blockCountry(country);
                        item->setCheckState(Qt::Checked);
                        item->setForeground(QColor("#dc3545"));
                        item->setBackground(QColor("#f8d7da"));
                        blocked++;
                    }
                    break;
                }
            }
        }
        if (blocked > 0) {
            QMessageBox::information(&dialog, "Готово",
                                     QString("Заблокировано %1 стран с цензурой").arg(blocked));
        }
    });

    connect(blockStreamingBtn, &QPushButton::clicked, &dialog, [this, countryList, &dialog]() {
        QStringList streamingBlocked = {
            "United States", "UK", "Canada", "Australia", "Germany",
            "France", "Japan", "South Korea", "Brazil", "Mexico"
        };

        int blocked = 0;
        for (int i = 0; i < countryList->count(); ++i) {
            QListWidgetItem* item = countryList->item(i);
            QString country = item->data(Qt::UserRole).toString();

            for (const QString& blockedCountry : streamingBlocked) {
                if (country.contains(blockedCountry, Qt::CaseInsensitive)) {
                    if (!blockedCountries.contains(country)) {
                        blockCountry(country);
                        item->setCheckState(Qt::Checked);
                        item->setForeground(QColor("#dc3545"));
                        item->setBackground(QColor("#f8d7da"));
                        blocked++;
                    }
                    break;
                }
            }
        }
        if (blocked > 0) {
            QMessageBox::information(&dialog, "Готово",
                                     QString("Заблокировано %1 стран с блокировкой стриминга").arg(blocked));
        }
    });

    connect(blockCommonBtn, &QPushButton::clicked, &dialog, [this, countryList, &dialog]() {
        QStringList commonVPNCountries = {
            "United States", "Germany", "Netherlands", "Singapore",
            "United Kingdom", "Japan", "Canada", "Switzerland"
        };

        int blocked = 0;
        for (int i = 0; i < countryList->count(); ++i) {
            QListWidgetItem* item = countryList->item(i);
            QString country = item->data(Qt::UserRole).toString();

            for (const QString& common : commonVPNCountries) {
                if (country.contains(common, Qt::CaseInsensitive)) {
                    if (!blockedCountries.contains(country)) {
                        blockCountry(country);
                        item->setCheckState(Qt::Checked);
                        item->setForeground(QColor("#dc3545"));
                        item->setBackground(QColor("#f8d7da"));
                        blocked++;
                    }
                    break;
                }
            }
        }
        if (blocked > 0) {
            QMessageBox::information(&dialog, "Готово",
                                     QString("Заблокировано %1 популярных VPN стран").arg(blocked));
        }
    });

    connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::accept);

    dialog.exec();
}

void MainWindow::clearAllBlockedCountries() {
    if (blockedCountries.isEmpty()) {
        QMessageBox::information(this, "Информация", "Нет исключенных стран для очистки.");
        return;
    }

    QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        "Очистка исключений",
        QString("Вы уверены, что хотите очистить все исключенные страны (%1)?")
        .arg(blockedCountries.size()),
                                                              QMessageBox::Yes | QMessageBox::No
    );

    if (reply == QMessageBox::Yes) {
        int count = blockedCountries.size();
        blockedCountries.clear();
        saveBlockedCountries();

        addLog(QString("🗑️ Очищено %1 исключенных стран").arg(count), "SUCCESS");
        updateServerList();

        QMessageBox::information(this, "Готово",
                                 QString("Все исключенные страны (%1) были очищены.").arg(count));
    }
}

void MainWindow::showBlockedCountries(bool show) {
    updateServerList();
    addLog(QString("Режим показа исключенных стран: %1").arg(show ? "включен" : "выключен"), "INFO");
}

void MainWindow::on_countryFilterButton_clicked() {
    QPoint pos = ui->countryFilterButton->mapToGlobal(QPoint(0, ui->countryFilterButton->height()));

    if (countryFilterMenu) {
        countryFilterMenu->exec(pos);
    } else {
        initCountryFilterMenu();
        if (countryFilterMenu) {
            countryFilterMenu->exec(pos);
        }
    }
}

void MainWindow::showExportMenu(const QPoint& pos) {
    int row = ui->serverList->currentRow();
    if (row < 0 || row >= servers.size()) {
        return;
    }

    VpnServer server = servers[row];
    QPoint globalPos = ui->serverList->viewport()->mapToGlobal(pos);

    QMenu menu(this);

    QAction* exportForAndroid = new QAction("📱 Для Android", &menu);
    QAction* exportForiOS = new QAction("🍏 Для iOS", &menu);
    QAction* exportForWindows = new QAction("🪟 Для Windows", &menu);
    QAction* exportForRouter = new QAction("🔄 Для роутера", &menu);
    QAction* exportForAll = new QAction("📦 Все платформы", &menu);

    menu.addAction(exportForAndroid);
    menu.addAction(exportForiOS);
    menu.addAction(exportForWindows);
    menu.addAction(exportForRouter);
    menu.addSeparator();
    menu.addAction(exportForAll);

    connect(exportForAndroid, &QAction::triggered, [this, server]() {
        QString path = QFileDialog::getSaveFileName(this, "Сохранить для Android",
                                                    QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                                                    "/" + server.name + "_android.ovpn",
                                                    "OpenVPN файлы (*.ovpn)");
        if (!path.isEmpty()) {
            generateAndroidConfig(server, path);
        }
    });

    connect(exportForiOS, &QAction::triggered, [this, server]() {
        QString path = QFileDialog::getSaveFileName(this, "Сохранить для iOS",
                                                    QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                                                    "/" + server.name + "_ios.ovpn",
                                                    "OpenVPN файлы (*.ovpn)");
        if (!path.isEmpty()) {
            generateiOSConfig(server, path);
        }
    });

    connect(exportForWindows, &QAction::triggered, [this, server]() {
        QString path = QFileDialog::getSaveFileName(this, "Сохранить для Windows",
                                                    QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                                                    "/" + server.name + "_windows.ovpn",
                                                    "OpenVPN файлы (*.ovpn)");
        if (!path.isEmpty()) {
            generateWindowsConfig(server, path);
        }
    });

    connect(exportForRouter, &QAction::triggered, [this, server]() {
        QString path = QFileDialog::getSaveFileName(this, "Сохранить для роутера",
                                                    QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                                                    "/" + server.name + "_router.conf",
                                                    "Конфигурации (*.conf)");
        if (!path.isEmpty()) {
            generateRouterConfig(server, path);
        }
    });

    connect(exportForAll, &QAction::triggered, [this, server]() {
        QString dir = QFileDialog::getExistingDirectory(this, "Выберите папку для экспорта",
                                                        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));

        if (!dir.isEmpty()) {
            QString basePath = dir + "/" + server.name;
            generateAndroidConfig(server, basePath + "_android.ovpn");
            generateiOSConfig(server, basePath + "_ios.ovpn");
            generateWindowsConfig(server, basePath + "_windows.ovpn");
            generateRouterConfig(server, basePath + "_router.conf");

            QMessageBox::information(this, "Успех",
                                     "Конфигурации для всех платформ успешно экспортированы!");
        }
    });

    menu.exec(globalPos);
}

void MainWindow::exportOpenVPNConfig(const VpnServer& server, const QString& filePath) {
    QByteArray configData = QByteArray::fromBase64(server.configBase64.toLatin1());
    QString configContent = QString::fromUtf8(configData);

    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        stream << configContent;
        file.close();
        addLog(QString("Конфигурация экспортирована: %1").arg(filePath), "SUCCESS");
    } else {
        QMessageBox::warning(this, "Ошибка", "Не удалось сохранить файл");
    }
}

void MainWindow::generateAndroidConfig(const VpnServer& server, const QString& filePath) {
    QByteArray configData = QByteArray::fromBase64(server.configBase64.toLatin1());
    QString configContent = QString::fromUtf8(configData);

    QStringList lines = configContent.split('\n');
    QStringList enhancedLines;

    for (const QString& line : lines) {
        QString trimmed = line.trimmed();

        if (trimmed.startsWith(";") || trimmed.startsWith("#")) {
            enhancedLines.append(trimmed);
            continue;
        }

        if (trimmed.startsWith("cipher ")) {
            QString cipher = trimmed.split(' ')[1];
            enhancedLines.append(QString("# %1").arg(trimmed));
            enhancedLines.append("cipher AES-256-GCM");
            enhancedLines.append("auth SHA256");
        } else if (trimmed.contains("fragment") || trimmed.contains("mssfix")) {
            enhancedLines.append(QString("# %1  # Отключено для Android").arg(trimmed));
        } else if (trimmed.startsWith("comp-lzo")) {
            enhancedLines.append("comp-lzo no");
        } else {
            enhancedLines.append(trimmed);
        }
    }

    enhancedLines.append("\n# Оптимизации для Android");
    enhancedLines.append("tun-mtu 1500");
    enhancedLines.append("mssfix 1450");
    enhancedLines.append("reneg-sec 0");
    enhancedLines.append("auth-nocache");
    enhancedLines.append("persist-key");
    enhancedLines.append("persist-tun");
    enhancedLines.append("nobind");
    enhancedLines.append("remote-cert-tls server");
    enhancedLines.append("script-security 2");
    enhancedLines.append("float");
    enhancedLines.append("verb 3");
    enhancedLines.append("keepalive 10 60");

    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        stream << enhancedLines.join('\n');
        file.close();
        addLog(QString("Android конфиг создан: %1").arg(filePath), "SUCCESS");
        QMessageBox::information(this, "Экспорт завершен",
                                 QString("Конфигурация для Android успешно экспортирована:\n%1").arg(filePath));
    } else {
        QMessageBox::warning(this, "Ошибка", "Не удалось сохранить файл");
    }
}

void MainWindow::generateiOSConfig(const VpnServer& server, const QString& filePath) {
    QByteArray configData = QByteArray::fromBase64(server.configBase64.toLatin1());
    QString configContent = QString::fromUtf8(configData);

    QStringList lines = configContent.split('\n');
    QStringList enhancedLines;

    for (const QString& line : lines) {
        QString trimmed = line.trimmed();

        if (trimmed.startsWith(";") || trimmed.startsWith("#")) {
            enhancedLines.append(trimmed);
            continue;
        }

        if (trimmed.startsWith("cipher ")) {
            enhancedLines.append("cipher AES-256-GCM");
            enhancedLines.append("auth SHA256");
        } else if (trimmed.contains("fragment") || trimmed.contains("mssfix")) {
            enhancedLines.append(QString("# %1  # Отключено для iOS").arg(trimmed));
        } else if (trimmed.startsWith("comp-lzo")) {
            enhancedLines.append("compress lz4-v2");
        } else {
            enhancedLines.append(trimmed);
        }
    }

    enhancedLines.append("\n# Оптимизации для iOS");
    enhancedLines.append("tun-mtu 1500");
    enhancedLines.append("reneg-sec 0");
    enhancedLines.append("auth-nocache");
    enhancedLines.append("persist-key");
    enhancedLines.append("persist-tun");
    enhancedLines.append("nobind");
    enhancedLines.append("remote-cert-tls server");
    enhancedLines.append("script-security 2");
    enhancedLines.append("float");
    enhancedLines.append("verb 2");
    enhancedLines.append("keepalive 10 60");
    enhancedLines.append("redirect-gateway def1");

    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        stream << enhancedLines.join('\n');
        file.close();
        addLog(QString("iOS конфиг создан: %1").arg(filePath), "SUCCESS");
        QMessageBox::information(this, "Экспорт завершен",
                                 QString("Конфигурация для iOS успешно экспортирована:\n%1").arg(filePath));
    } else {
        QMessageBox::warning(this, "Ошибка", "Не удалось сохранить файл");
    }
}

void MainWindow::generateWindowsConfig(const VpnServer& server, const QString& filePath) {
    QByteArray configData = QByteArray::fromBase64(server.configBase64.toLatin1());
    QString configContent = QString::fromUtf8(configData);

    QStringList lines = configContent.split('\n');
    QStringList enhancedLines;

    for (const QString& line : lines) {
        QString trimmed = line.trimmed();

        if (trimmed.startsWith(";") || trimmed.startsWith("#")) {
            enhancedLines.append(trimmed);
            continue;
        }

        if (trimmed.startsWith("cipher ")) {
            QString cipher = trimmed.split(' ')[1];
            enhancedLines.append(QString("# %1").arg(trimmed));
            enhancedLines.append("cipher AES-256-CBC");
            enhancedLines.append("auth SHA256");
        } else if (trimmed.contains("fragment") || trimmed.contains("mssfix")) {
            enhancedLines.append(trimmed);
        } else {
            enhancedLines.append(trimmed);
        }
    }

    enhancedLines.append("\n# Оптимизации для Windows");
    enhancedLines.append("tun-mtu 1500");
    enhancedLines.append("mssfix 1400");
    enhancedLines.append("reneg-sec 0");
    enhancedLines.append("auth-nocache");
    enhancedLines.append("persist-key");
    enhancedLines.append("persist-tun");
    enhancedLines.append("nobind");
    enhancedLines.append("remote-cert-tls server");
    enhancedLines.append("script-security 2");
    enhancedLines.append("float");
    enhancedLines.append("verb 3");
    enhancedLines.append("keepalive 10 60");
    enhancedLines.append("route-method exe");
    enhancedLines.append("route-delay 2");

    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        stream << enhancedLines.join('\n');
        file.close();
        addLog(QString("Windows конфиг создан: %1").arg(filePath), "SUCCESS");
        QMessageBox::information(this, "Экспорт завершен",
                                 QString("Конфигурация для Windows успешно экспортирована:\n%1").arg(filePath));
    } else {
        QMessageBox::warning(this, "Ошибка", "Не удалось сохранить файл");
    }
}

void MainWindow::generateRouterConfig(const VpnServer& server, const QString& filePath) {
    QByteArray configData = QByteArray::fromBase64(server.configBase64.toLatin1());
    QString configContent = QString::fromUtf8(configData);

    QStringList lines = configContent.split('\n');
    QStringList enhancedLines;

    for (const QString& line : lines) {
        QString trimmed = line.trimmed();

        if (trimmed.startsWith(";") || trimmed.startsWith("#")) {
            continue;
        }

        if (trimmed.isEmpty()) {
            continue;
        }

        if (trimmed.startsWith("cipher ")) {
            enhancedLines.append("cipher AES-128-CBC");
        } else if (trimmed.startsWith("auth ")) {
            enhancedLines.append("auth SHA1");
        } else if (trimmed.contains("fragment") || trimmed.contains("mssfix")) {
            enhancedLines.append(trimmed);
        } else if (trimmed.startsWith("comp-lzo")) {
            enhancedLines.append("comp-lzo adaptive");
        } else if (!trimmed.startsWith("verb") && !trimmed.startsWith("mute")) {
            enhancedLines.append(trimmed);
        }
    }

    enhancedLines.append("\n# Минимальный конфиг для роутера");
    enhancedLines.append("tun-mtu 1500");
    enhancedLines.append("mssfix 1450");
    enhancedLines.append("reneg-sec 3600");
    enhancedLines.append("persist-key");
    enhancedLines.append("persist-tun");
    enhancedLines.append("nobind");
    enhancedLines.append("remote-cert-tls server");
    enhancedLines.append("script-security 2");
    enhancedLines.append("keepalive 20 120");
    enhancedLines.append("verb 1");

    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        stream << enhancedLines.join('\n');
        file.close();
        addLog(QString("Router конфиг создан: %1").arg(filePath), "SUCCESS");
        QMessageBox::information(this, "Экспорт завершен",
                                 QString("Конфигурация для роутера успешно экспортирована:\n%1").arg(filePath));
    } else {
        QMessageBox::warning(this, "Ошибка", "Не удалось сохранить файл");
    }
}

void MainWindow::setupVPNGateway() {
    addLog("Настройка VPN Gateway...", "INFO");

    #ifdef Q_OS_LINUX
    if (getuid() != 0) {
        QMessageBox::warning(this, "Требуются права",
                             "Для настройки VPN Gateway требуются права администратора.\n"
                             "Запустите программу с sudo.");
        return;
    }
    #else
    // Для Windows или других систем можно использовать альтернативную проверку
    addLog("VPN Gateway работает только под Linux", "WARNING");
    QMessageBox::warning(this, "Не поддерживается",
                         "VPN Gateway в настоящее время поддерживается только на Linux.");
    return;
    #endif

    QProcess process;
    process.start("which", QStringList() << "iptables");
    process.waitForFinished();

    if (process.exitCode() != 0) {
        QMessageBox::warning(this, "Отсутствуют зависимости",
                             "Для работы VPN Gateway требуется iptables.\n"
                             "Установите: sudo apt install iptables");
        return;
    }

    addLog("VPN Gateway готов к настройке", "SUCCESS");
}

void MainWindow::startVPNGateway() {
    if (!vpnManager->isConnected()) {
        QMessageBox::warning(this, "Нет VPN подключения",
                             "Сначала подключитесь к VPN серверу");
        return;
    }

    if (vpnGatewayEnabled) {
        addLog("VPN Gateway уже запущен", "WARNING");
        return;
    }

    addLog("🚀 Запуск VPN Gateway...", "INFO");

    auto status = vpnManager->getStatus();
    if (status.first != "connected") {
        addLog("Нет активного VPN подключения", "ERROR");
        return;
    }

    QProcess ifconfig;
    ifconfig.start("ip", QStringList() << "route" << "show" << "default");
    ifconfig.waitForFinished();

    QString output = QString::fromUtf8(ifconfig.readAllStandardOutput());
    QString defaultInterface;

    QRegularExpression re("dev\\s+(\\w+)");
    QRegularExpressionMatch match = re.match(output);
    if (match.hasMatch()) {
        defaultInterface = match.captured(1);
        addLog(QString("Основной интерфейс: %1").arg(defaultInterface), "INFO");
    } else {
        defaultInterface = "eth0";
        addLog(QString("Используем интерфейс по умолчанию: %1").arg(defaultInterface), "WARNING");
    }

    QString script = QString(
        "#!/bin/bash\n"
        "# Включаем IP forwarding\n"
        "echo 1 > /proc/sys/net/ipv4/ip_forward\n"
        "echo 1 > /proc/sys/net/ipv6/conf/all/forwarding\n"
        "\n"
        "# Настраиваем iptables для NAT\n"
        "iptables -t nat -A POSTROUTING -o %1 -j MASQUERADE\n"
        "iptables -A FORWARD -i %1 -o %2 -m state --state RELATED,ESTABLISHED -j ACCEPT\n"
        "iptables -A FORWARD -i %2 -o %1 -j ACCEPT\n"
        "\n"
        "echo 'Настройка завершена. Подключите устройства к сети.'\n"
    ).arg(gatewayInterface).arg(defaultInterface);

    QString scriptPath = QDir::tempPath() + "/vpngateway_setup.sh";
    QFile scriptFile(scriptPath);
    if (scriptFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&scriptFile);
        stream << script;
        scriptFile.close();
        scriptFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    gatewayProcess->start("sudo", QStringList() << "bash" << scriptPath);

    if (gatewayProcess->waitForStarted(3000)) {
        vpnGatewayEnabled = true;
        ui->gatewayStartButton->setEnabled(false);
        ui->gatewayStopButton->setEnabled(true);
        ui->gatewayStatusLabel->setText("Статус: Запущен");
        ui->gatewayInfoLabel->setText(QString("Шлюз активен на интерфейсе: %1").arg(defaultInterface));

        addLog("✅ VPN Gateway запущен", "SUCCESS");
        addLog("Теперь другие устройства могут использовать это подключение", "INFO");

        QMessageBox::information(this, "VPN Gateway запущен",
                                 "✅ VPN Gateway успешно запущен!\n\n"
                                 "Настройки для других устройств:\n"
                                 "• IP адрес этого ПК: [автоматически определите]\n"
                                 "• Шлюз по умолчанию: тот же IP\n"
                                 "• DNS: 8.8.8.8 или используйте системные\n\n"
                                 "Для остановки нажмите 'Остановить шлюз'");
    } else {
        addLog("❌ Не удалось запустить VPN Gateway", "ERROR");
    }
}

void MainWindow::stopVPNGateway() {
    if (!vpnGatewayEnabled) {
        addLog("VPN Gateway не запущен", "WARNING");
        return;
    }

    addLog("🛑 Остановка VPN Gateway...", "INFO");

    QString cleanupScript = QString(
        "#!/bin/bash\n"
        "# Очищаем iptables правила\n"
        "iptables -t nat -D POSTROUTING -o %1 -j MASQUERADE 2>/dev/null\n"
        "iptables -D FORWARD -i eth0 -o %1 -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null\n"
        "iptables -D FORWARD -i %1 -o eth0 -j ACCEPT 2>/dev/null\n"
        "\n"
        "# Выключаем IP forwarding\n"
        "echo 0 > /proc/sys/net/ipv4/ip_forward\n"
        "echo 0 > /proc/sys/net/ipv6/conf/all/forwarding\n"
        "\n"
        "echo 'VPN Gateway остановлен'\n"
    ).arg(gatewayInterface);

    QString scriptPath = QDir::tempPath() + "/vpngateway_cleanup.sh";
    QFile scriptFile(scriptPath);
    if (scriptFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&scriptFile);
        stream << cleanupScript;
        scriptFile.close();
        scriptFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    QProcess cleanupProcess;
    cleanupProcess.start("sudo", QStringList() << "bash" << scriptPath);
    cleanupProcess.waitForFinished(5000);

    vpnGatewayEnabled = false;
    ui->gatewayStartButton->setEnabled(true);
    ui->gatewayStopButton->setEnabled(false);
    ui->gatewayStatusLabel->setText("Статус: Остановлен");
    ui->gatewayInfoLabel->setText("IP: Не настроен");

    addLog("✅ VPN Gateway остановлен", "SUCCESS");

    QFile::remove(scriptPath);
}

bool MainWindow::isVPNGatewayRunning() const {
    return vpnGatewayEnabled;
}

void MainWindow::onServerListContextMenu(const QPoint& pos) {
    int row = ui->serverList->row(ui->serverList->itemAt(pos));
    if (row < 0 || row >= servers.size()) {
        return;
    }

    VpnServer server = servers[row];

    QMenu menu(this);

    QAction* connectAction = new QAction("🔗 Подключиться", &menu);
    QAction* copyIPAction = new QAction("📋 Скопировать IP", &menu);
    QAction* copyConfigAction = new QAction("📄 Скопировать конфиг", &menu);
    QAction* exportConfigAction = new QAction("💾 Экспорт конфига", &menu);

    bool isCountryBlocked = blockedCountries.contains(server.country);
    QString countryActionText = isCountryBlocked ?
    QString("✅ Разблокировать %1").arg(server.country) :
    QString("🚫 Исключить %1").arg(server.country);

    QAction* toggleCountryAction = new QAction(countryActionText, &menu);

    menu.addAction(connectAction);
    menu.addSeparator();
    menu.addAction(copyIPAction);
    menu.addAction(copyConfigAction);
    menu.addAction(exportConfigAction);
    menu.addSeparator();
    menu.addAction(toggleCountryAction);

    connect(connectAction, &QAction::triggered, [this, row]() {
        ui->serverList->setCurrentRow(row);
        on_connectButton_clicked();
    });

    connect(toggleCountryAction, &QAction::triggered, [this, server, isCountryBlocked]() {
        if (isCountryBlocked) {
            unblockCountry(server.country);
        } else {
            blockCountry(server.country);
        }
        updateServerList();
    });

    connect(copyIPAction, &QAction::triggered, [this, server]() {
        copyToClipboard(server.ip, QString("IP адрес %1 скопирован в буфер обмена").arg(server.ip));
    });

    connect(copyConfigAction, &QAction::triggered, [this, server]() {
        QByteArray configData = QByteArray::fromBase64(server.configBase64.toLatin1());
        QString config = QString::fromUtf8(configData);
        copyToClipboard(config, QString("Конфигурация сервера %1 скопирована").arg(server.name));
    });

    connect(exportConfigAction, &QAction::triggered, [this, server]() {
        exportServerConfig(server);
    });

    menu.exec(ui->serverList->viewport()->mapToGlobal(pos));
}

void MainWindow::blockCountry(const QString& country) {
    if (blockedCountries.contains(country)) {
        return;
    }

    blockedCountries.insert(country);
    saveBlockedCountries();

    addLog(QString("🚫 Страна исключена: %1").arg(country), "INFO");

    QList<VpnServer> filteredServers;
    for (const VpnServer& server : servers) {
        if (!blockedCountries.contains(server.country) &&
            !failedServers.contains(server.name)) {
            filteredServers.append(server);
            }
    }

    servers = filteredServers;
    updateServerList();

    if (isAutoReconnecting) {
        addLog("Обновляю авто-подключение после блокировки страны...", "INFO");
        autoConnectIndex = servers.size() - 1;
    }
}

void MainWindow::unblockCountry(const QString& country) {
    if (!blockedCountries.contains(country)) {
        return;
    }

    blockedCountries.remove(country);
    saveBlockedCountries();

    addLog(QString("✅ Страна разблокирована: %1").arg(country), "INFO");
    updateServerList();
}

void MainWindow::toggleCountryBlock(const QString& country) {
    if (blockedCountries.contains(country)) {
        unblockCountry(country);
    } else {
        blockCountry(country);
    }
}

void MainWindow::saveBlockedCountries() {
    settings->beginWriteArray("blockedCountries");
    int i = 0;
    for (const QString& country : blockedCountries) {
        settings->setArrayIndex(i++);
        settings->setValue("country", country);
    }
    settings->endArray();
    settings->sync();
}

QString MainWindow::getCountryCode(const QString& countryName) {
    static const QMap<QString, QString> countryToCode = {
        {"United States", "US"}, {"USA", "US"},
        {"Japan", "JP"}, {"Korea Republic of", "KR"}, {"South Korea", "KR"},
        {"Russian Federation", "RU"}, {"Russia", "RU"},
        {"Germany", "DE"}, {"China", "CN"}, {"United Kingdom", "GB"},
        {"France", "FR"}, {"Canada", "CA"}, {"Brazil", "BR"},
        {"Ukraine", "UA"}, {"Poland", "PL"}, {"Turkey", "TR"},
        {"Italy", "IT"}, {"Spain", "ES"}, {"Australia", "AU"},
        {"Netherlands", "NL"}, {"Sweden", "SE"}, {"Switzerland", "CH"},
        {"Singapore", "SG"}, {"India", "IN"}, {"Mexico", "MX"},
        {"Indonesia", "ID"}, {"Philippines", "PH"}, {"Thailand", "TH"},
        {"Malaysia", "MY"}, {"South Africa", "ZA"}, {"Egypt", "EG"},
        {"Saudi Arabia", "SA"}, {"United Arab Emirates", "AE"},
        {"Israel", "IL"}, {"Norway", "NO"}, {"Denmark", "DK"},
        {"Finland", "FI"}, {"Belgium", "BE"}, {"Austria", "AT"},
        {"Czech Republic", "CZ"}, {"Hungary", "HU"}, {"Romania", "RO"},
        {"Greece", "GR"}, {"Portugal", "PT"}, {"Ireland", "IE"},
        {"New Zealand", "NZ"}
    };

    for (auto it = countryToCode.begin(); it != countryToCode.end(); ++it) {
        if (countryName.contains(it.key(), Qt::CaseInsensitive)) {
            return it.value();
        }
    }

    return countryName.left(2).toUpper();
}

QString MainWindow::getCountryFlag(const QString& countryCode) {
    static const QMap<QString, QString> flagMap = {
        {"US", "🇺🇸"}, {"JP", "🇯🇵"}, {"KR", "🇰🇷"}, {"RU", "🇷🇺"},
        {"DE", "🇩🇪"}, {"CN", "🇨🇳"}, {"GB", "🇬🇧"}, {"FR", "🇫🇷"},
        {"CA", "🇨🇦"}, {"BR", "🇧🇷"}, {"UA", "🇺🇦"}, {"PL", "🇵🇱"},
        {"TR", "🇹🇷"}, {"IT", "🇮🇹"}, {"ES", "🇪🇸"}, {"AU", "🇦🇺"},
        {"NL", "🇳🇱"}, {"SE", "🇸🇪"}, {"CH", "🇨🇭"}, {"SG", "🇸🇬"},
        {"IN", "🇮🇳"}, {"MX", "🇲🇽"}, {"ID", "🇮🇩"}, {"PH", "🇵🇭"},
        {"TH", "🇹🇭"}, {"MY", "🇲🇾"}, {"ZA", "🇿🇦"}, {"EG", "🇪🇬"},
        {"SA", "🇸🇦"}, {"AE", "🇦🇪"}, {"IL", "🇮🇱"}, {"NO", "🇳🇴"},
        {"DK", "🇩🇰"}, {"FI", "🇫🇮"}, {"BE", "🇧🇪"}, {"AT", "🇦🇹"},
        {"CZ", "🇨🇿"}, {"HU", "🇭🇺"}, {"RO", "🇷🇴"}, {"GR", "🇬🇷"},
        {"PT", "🇵🇹"}, {"IE", "🇮🇪"}, {"NZ", "🇳🇿"}
    };

    return flagMap.value(countryCode, "🌐");
}

QString MainWindow::getCountryDisplayName(const QString& countryName) {
    QString code = getCountryCode(countryName);
    QString flag = getCountryFlag(code);
    return QString("%1 %2").arg(flag).arg(countryName);
}

void MainWindow::updateCountryStats() {
    countryServerCounts.clear();
    for (const VpnServer& server : servers) {
        if (!blockedCountries.contains(server.country) &&
            !failedServers.contains(server.name)) {
            countryServerCounts[server.country]++;
            }
    }
}

void MainWindow::showConnectionInfo(const VpnServer& server) {
    updateSelection();
}

void MainWindow::copyToClipboard(const QString& text, const QString& logMessage) {
    QClipboard* clipboard = QApplication::clipboard();
    if (clipboard) {
        clipboard->setText(text);
        if (!logMessage.isEmpty()) {
            addLog(logMessage, "INFO");
        }
    }
}

void MainWindow::showServerTestDialog(const VpnServer& server) {
    QMessageBox::information(this, "Тестирование сервера",
                             QString("Тестирование сервера %1\nIP: %2\nСтрана: %3\nСкорость: %4 Mbps\nПинг: %5 ms")
                             .arg(server.name)
                             .arg(server.ip)
                             .arg(server.country)
                             .arg(server.speedMbps, 0, 'f', 1)
                             .arg(server.ping));
}

void MainWindow::exportServerConfig(const VpnServer& server) {
    QString fileName = QFileDialog::getSaveFileName(this,
                                                    "Экспорт конфигурации",
                                                    QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                                                    "/" + server.name + ".ovpn",
                                                    "OpenVPN конфигурации (*.ovpn)");

    if (!fileName.isEmpty()) {
        QByteArray configData = QByteArray::fromBase64(server.configBase64.toLatin1());
        QString config = QString::fromUtf8(configData);

        QFile file(fileName);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&file);
            stream << config;
            file.close();
            addLog(QString("Конфигурация сервера %1 экспортирована в %2")
            .arg(server.name).arg(fileName), "SUCCESS");
        } else {
            QMessageBox::warning(this, "Ошибка", "Не удалось сохранить файл");
        }
    }
}

void MainWindow::importServerConfigs() {
    QStringList fileNames = QFileDialog::getOpenFileNames(this,
                                                          "Импорт конфигураций",
                                                          QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                                          "OpenVPN конфигурации (*.ovpn)");

    if (!fileNames.isEmpty()) {
        addLog(QString("Импортировано %1 конфигураций").arg(fileNames.size()), "INFO");
    }
}

void MainWindow::loadBlockedCountries() {
    blockedCountries.clear();
    int size = settings->beginReadArray("blockedCountries");
    for (int i = 0; i < size; ++i) {
        settings->setArrayIndex(i);
        QString country = settings->value("country").toString();
        if (!country.isEmpty()) {
            blockedCountries.insert(country);
        }
    }
    settings->endArray();

    addLog(QString("Загружено %1 исключенных стран").arg(blockedCountries.size()), "INFO");
}

// Методы сортировки (реализации для совместимости)
void MainWindow::sortServersBySpeed() {
    std::sort(servers.begin(), servers.end(),
              [](const VpnServer& a, const VpnServer& b) {
                  return a.speedMbps > b.speedMbps;
              });
    updateServerList();
    addLog("Серверы отсортированы по скорости", "INFO");
}

void MainWindow::sortServersByPing() {
    std::sort(servers.begin(), servers.end(),
              [](const VpnServer& a, const VpnServer& b) {
                  return a.ping < b.ping;
              });
    updateServerList();
    addLog("Серверы отсортированы по пингу", "INFO");
}

void MainWindow::sortServersByCountry() {
    std::sort(servers.begin(), servers.end(),
              [](const VpnServer& a, const VpnServer& b) {
                  return a.country < b.country;
              });
    updateServerList();
    addLog("Серверы отсортированы по стране", "INFO");
}

void MainWindow::filterServersByCountry(const QString& country) {
    updateServerList();
}

void MainWindow::clearCountryFilter() {
    clearAllBlockedCountries();
}

void MainWindow::updateCountryStatistics() {
    updateCountryStats();
}

void MainWindow::generateGatewayConfig() {
    // Получаем IP адрес текущего компьютера
    QProcess process;
    process.start("hostname", QStringList() << "-I");
    process.waitForFinished();
    QString localIP = QString::fromUtf8(process.readAllStandardOutput()).trimmed();

    if (localIP.isEmpty()) {
        addLog("Не удалось определить IP адрес", "ERROR");
        return;
    }

    QString config = QString(
        "client\n"
        "proto udp\n"
        "remote %1 1194\n"
        "dev tun\n"
        "resolv-retry infinite\n"
        "nobind\n"
        "persist-key\n"
        "persist-tun\n"
        "remote-cert-tls server\n"
        "cipher AES-256-CBC\n"
        "auth SHA256\n"
        "verb 3\n"
        "auth-user-pass\n"
        "auth-nocache\n"
        "\n"
        "# Автоматическое подключение при запуске\n"
        "pull\n"
        "tun-mtu 1500\n"
        "mssfix 1450\n"
        "keepalive 10 120\n"
        "\n"
        "# Комментарий\n"
        "# Подключение к VPN Gateway на %2\n"
        "# Логин/пароль: vpn/vpn\n"
    ).arg(localIP.split(" ").first()).arg(localIP.split(" ").first());

    QString fileName = QFileDialog::getSaveFileName(this,
                                                    "Сохранить конфигурацию шлюза",
                                                    QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/vpngate_gateway.ovpn",
                                                    "OpenVPN файлы (*.ovpn)");

    if (!fileName.isEmpty()) {
        QFile file(fileName);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&file);
            stream << config;
            file.close();

            addLog(QString("Конфигурация шлюза сохранена: %1").arg(fileName), "SUCCESS");
            QMessageBox::information(this, "Конфигурация создана",
                                     QString("✅ Конфигурация для VPN Gateway создана!\n\n"
                                     "IP адрес шлюза: %1\n"
                                     "Порт: 1194\n"
                                     "Логин: vpn\n"
                                     "Пароль: vpn\n\n"
                                     "Используйте этот файл на других устройствах.")
                                     .arg(localIP.split(" ").first()));
        }
    }
}

void MainWindow::on_resetFailedButton_clicked() {
    resetFailedServers();
}

void MainWindow::on_sortBySpeedButton_clicked() {
    currentSortType = "speed";
    sortServersBySpeed();
    setSortButtonActive(ui->sortBySpeedButton);
}

void MainWindow::on_sortByPingButton_clicked() {
    currentSortType = "ping";
    sortServersByPing();
    setSortButtonActive(ui->sortByPingButton);
}

void MainWindow::on_sortByCountryButton_clicked() {
    currentSortType = "country";
    sortServersByCountry();
    setSortButtonActive(ui->sortByCountryButton);
}

void MainWindow::on_quickConnectFastButton_clicked() {
    if (servers.isEmpty()) {
        QMessageBox::warning(this, "Нет серверов",
                             "Список серверов пуст. Обновите список.");
        return;
    }

    VpnServer fastestServer = findFastestServer();
    if (fastestServer.name.isEmpty()) {
        QMessageBox::warning(this, "Нет доступных серверов",
                             "Не найдено доступных серверов для подключения.");
        return;
    }

    addLog(QString("Быстрое подключение к самому быстрому серверу: %1 (%2 Mbps)")
    .arg(fastestServer.name).arg(fastestServer.speedMbps, 0, 'f', 1), "INFO");

    // Находим и выделяем сервер в списке
    for (int i = 0; i < servers.size(); ++i) {
        if (servers[i].name == fastestServer.name) {
            ui->serverList->setCurrentRow(i);
            break;
        }
    }

    // Подключаемся
    vpnManager->connectToServer(fastestServer);
}

void MainWindow::on_quickConnectStableButton_clicked() {
    if (servers.isEmpty()) {
        QMessageBox::warning(this, "Нет серверов",
                             "Список серверов пуст. Обновите список.");
        return;
    }

    VpnServer stableServer = findMostStableServer();
    if (stableServer.name.isEmpty()) {
        QMessageBox::warning(this, "Нет доступных серверов",
                             "Не найдено доступных серверов для подключения.");
        return;
    }

    addLog(QString("Быстрое подключение к самому стабильному серверу: %1")
    .arg(stableServer.name), "INFO");

    // Находим и выделяем сервер в списке
    for (int i = 0; i < servers.size(); ++i) {
        if (servers[i].name == stableServer.name) {
            ui->serverList->setCurrentRow(i);
            break;
        }
    }

    // Подключаемся
    vpnManager->connectToServer(stableServer);
}

void MainWindow::on_quickConnectRandomButton_clicked() {
    if (servers.isEmpty()) {
        QMessageBox::warning(this, "Нет серверов",
                             "Список серверов пуст. Обновите список.");
        return;
    }

    VpnServer randomServer = findRandomServer();
    if (randomServer.name.isEmpty()) {
        QMessageBox::warning(this, "Нет доступных серверов",
                             "Не найдено доступных серверов для подключения.");
        return;
    }

    addLog(QString("Случайное подключение к серверу: %1 (%2)")
    .arg(randomServer.name).arg(randomServer.country), "INFO");

    // Находим и выделяем сервер в списке
    for (int i = 0; i < servers.size(); ++i) {
        if (servers[i].name == randomServer.name) {
            ui->serverList->setCurrentRow(i);
            break;
        }
    }

    // Подключаемся
    vpnManager->connectToServer(randomServer);
}

void MainWindow::on_createGatewayConfigButton_clicked() {
    generateGatewayConfig();
}

void MainWindow::updateLogCounter() {
    if (ui->logCounterLabel) {
        ui->logCounterLabel->setText(QString("Сообщений: %1").arg(logMessageCount));
    }
}

void MainWindow::updateConnectionTimerDisplay() {
    if (vpnManager->isConnected() && connectionTimer.isValid()) {
        int elapsed = connectionTimer.elapsed() / 1000; // секунды
        int minutes = elapsed / 60;
        int seconds = elapsed % 60;

        QString timeString = QString("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));

        if (ui->connectionTimeLabel) {
            ui->connectionTimeLabel->setText(QString("Время: %1").arg(timeString));
        }
    }
}

void MainWindow::updateGatewayInfo() {
    updateLocalIP();

    if (vpnGatewayEnabled) {
        ui->gatewayStatusLabel->setText("Статус: Активен");
        ui->gatewayInfoLabel->setText(QString("IP: %1").arg(localIPAddress));
        ui->createGatewayConfigButton->setEnabled(true);
    } else {
        ui->gatewayStatusLabel->setText("Статус: Неактивен");
        ui->gatewayInfoLabel->setText("IP: Не определен");
        ui->createGatewayConfigButton->setEnabled(false);
    }
}

void MainWindow::initSortButtons() {
    currentSortType = "speed";
    setSortButtonActive(ui->sortBySpeedButton);
}

void MainWindow::setSortButtonActive(QPushButton* activeButton) {
    // Сбрасываем все кнопки
    ui->sortBySpeedButton->setChecked(false);
    ui->sortByPingButton->setChecked(false);
    ui->sortByCountryButton->setChecked(false);

    // Активируем выбранную
    activeButton->setChecked(true);
}

VpnServer MainWindow::findFastestServer() const {
    VpnServer fastest;
    double maxSpeed = -1.0;

    for (const VpnServer& server : servers) {
        if (!failedServers.contains(server.name) &&
            !blockedCountries.contains(server.country) &&
            server.speedMbps > maxSpeed) {
            maxSpeed = server.speedMbps;
        fastest = server;
            }
    }

    return fastest;
}

VpnServer MainWindow::findMostStableServer() const {
    VpnServer mostStable;
    int maxScore = -1;

    for (const VpnServer& server : servers) {
        if (!failedServers.contains(server.name) &&
            !blockedCountries.contains(server.country) &&
            server.score > maxScore) {
            maxScore = server.score;
        mostStable = server;
            }
    }

    return mostStable;
}

VpnServer MainWindow::findRandomServer() const {
    QList<VpnServer> availableServers;

    for (const VpnServer& server : servers) {
        if (!failedServers.contains(server.name) &&
            !blockedCountries.contains(server.country)) {
            availableServers.append(server);
            }
    }

    if (availableServers.isEmpty()) {
        return VpnServer();
    }

    int randomIndex = QRandomGenerator::global()->bounded(availableServers.size());
    return availableServers[randomIndex];
}

void MainWindow::updateLocalIP() {
    QProcess process;
    process.start("hostname", QStringList() << "-I");
    process.waitForFinished();

    QString output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    if (!output.isEmpty()) {
        localIPAddress = output.split(" ").first();
    } else {
        localIPAddress = "Не определен";
    }
}

int MainWindow::getServerCountByCountry(const QString& country) const {
    int count = 0;
    for (const VpnServer& server : servers) {
        if (server.country == country &&
            !failedServers.contains(server.name)) {
            count++;
            }
    }
    return count;
}

int MainWindow::getWorkingServerCount() const {
    int count = 0;
    for (const VpnServer& server : servers) {
        if (!failedServers.contains(server.name) &&
            !blockedCountries.contains(server.country)) {
            count++;
            }
    }
    return count;
}

int MainWindow::getFailedServerCount() const {
    return failedServers.size();
}
