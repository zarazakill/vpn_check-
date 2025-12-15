#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "serverdownloader.h"
#include "servertester.h"
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
#include <QPointer>

#include <QDebug>

// Base64 декодирование
QByteArray base64_decode(const QString &input) {
    return QByteArray::fromBase64(input.toLatin1());
}

MainWindow::MainWindow(QWidget *parent)
: QMainWindow(parent)
, ui(new Ui::MainWindow)
, downloaderThread(nullptr)
, testerThread(nullptr)
, vpnManager(new VpnManager(this))
, isTestingAll(false)
, testInProgress(false)
, currentTestIndex(0)
, totalWorkingServers(0)
, totalFailedServers(0)
, settings(new QSettings("VPNGateManager", "Pro", this)) {

    ui->setupUi(this);
    initUI();

    // Очистка старых процессов
    QTimer::singleShot(500, this, &MainWindow::cleanupOldProcesses);

    // Автоматическое обновление
    QTimer::singleShot(1000, this, &MainWindow::on_refreshButton_clicked);
}

MainWindow::~MainWindow() {
    // Корректно останавливаем все потоки перед удалением
    stopTesting();

    if (downloaderThread && downloaderThread->isRunning()) {
        disconnect(downloaderThread, nullptr, this, nullptr);
        downloaderThread->quit();
        if (!downloaderThread->wait(2000)) {
            downloaderThread->terminate();
            downloaderThread->wait(1000);
        }
    }

    if (testerThread && testerThread->isRunning()) {
        disconnect(testerThread, nullptr, this, nullptr);
        testerThread->quit();
        if (!testerThread->wait(2000)) {
            testerThread->terminate();
            testerThread->wait(1000);
        }
    }

    // Очищаем память
    if (downloaderThread) {
        downloaderThread->deleteLater();
        downloaderThread = nullptr;
    }

    if (testerThread) {
        // Удаляем через deleteLater для безопасности
        testerThread->deleteLater();
        testerThread = nullptr;
    }

    // Очищаем менеджер VPN
    if (vpnManager) {
        vpnManager->disconnect(); // Отключаемся от VPN
        vpnManager->deleteLater();
        vpnManager = nullptr;
    }

    delete ui;
}

void MainWindow::initUI() {
    setWindowTitle("VPNGate Manager - Проверка всех серверов");
    setGeometry(100, 100, 500, 900);

    QFont titleFont("Arial", 14, QFont::Bold);
    ui->titleLabel->setFont(titleFont);
    ui->titleLabel->setAlignment(Qt::AlignCenter);

    ui->refreshButton->setText("🔄 Загрузить и проверить ВСЕ");
    ui->stopTestButton->setText("⏹️ Остановить проверку");
    ui->testSelectedButton->setText("🔍 Проверить снова");
    ui->connectButton->setText("🔗 Подключиться");
    ui->disconnectButton->setText("❌ Отключить");

    ui->stopTestButton->setEnabled(false);
    ui->testSelectedButton->setEnabled(false);
    ui->connectButton->setEnabled(false);
    ui->disconnectButton->setEnabled(false);

    ui->testProgressBar->setRange(0, 100);
    ui->testProgressBar->setValue(0);
    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(0);

    ui->testLogArea->setFont(QFont("Monospace", 8));
    ui->infoText->setFont(QFont("Monospace", 9));
    ui->logArea->setFont(QFont("Monospace", 8));

    ui->vpnStatusFrame->setVisible(false);

    qRegisterMetaType<QList<VpnServer>>("QList<VpnServer>");

    connect(vpnManager, &VpnManager::connectionStatus, this, &MainWindow::onVpnStatus, Qt::QueuedConnection);
    connect(vpnManager, &VpnManager::connectionLog, this, &MainWindow::onVpnLog, Qt::QueuedConnection);
    connect(vpnManager, &VpnManager::connected, this, &MainWindow::onVpnConnected, Qt::QueuedConnection);
    connect(vpnManager, &VpnManager::disconnected, this, &MainWindow::onVpnDisconnected, Qt::QueuedConnection);

    QTimer* statsTimer = new QTimer(this);
    connect(statsTimer, &QTimer::timeout, this, &MainWindow::updateStats);
    statsTimer->start(1000);
}

void MainWindow::on_refreshButton_clicked() {
    if (testInProgress) {
        QMessageBox::warning(this, "Внимание",
                             "Тестирование уже выполняется, дождитесь завершения");
        return;
    }

    ui->refreshButton->setEnabled(false);
    ui->stopTestButton->setEnabled(false);
    ui->statusLabel->setText("Загрузка списка серверов...");
    ui->testLogArea->clear();

    addLog("🔄 Загружаю список серверов с VPNGate...", "INFO");
    ui->testLogArea->append("🔄 Загружаю список серверов...");

    // Очищаем старый поток, если он есть
    if (downloaderThread) {
        downloaderThread->deleteLater();
        downloaderThread = nullptr;
    }

    downloaderThread = new ServerDownloaderThread(this);
    connect(downloaderThread, &ServerDownloaderThread::downloadFinished,
            this, &MainWindow::onServersDownloaded, Qt::QueuedConnection);
    connect(downloaderThread, &ServerDownloaderThread::downloadError,
            this, &MainWindow::onDownloadError, Qt::QueuedConnection);
    connect(downloaderThread, &ServerDownloaderThread::downloadProgress,
            ui->progressBar, &QProgressBar::setValue, Qt::QueuedConnection);
    connect(downloaderThread, &ServerDownloaderThread::logMessage,
            this, &MainWindow::onDownloadLog, Qt::QueuedConnection);

    // Автоматическое удаление потока после завершения
    connect(downloaderThread, &QThread::finished, downloaderThread, &QObject::deleteLater);
    connect(downloaderThread, &QThread::finished, this, [this]() {
        downloaderThread = nullptr;
    });

    downloaderThread->start();
}

void MainWindow::on_stopTestButton_clicked() {
    stopTesting();
}

void MainWindow::on_testSelectedButton_clicked() {
    int row = ui->serverList->currentRow();
    if (row >= 0 && row < workingServers.size()) {
        VpnServer server = workingServers[row];
        manualTestServer(server);
    }
}

void MainWindow::on_connectButton_clicked() {
    int row = ui->serverList->currentRow();
    if (row >= 0 && row < workingServers.size()) {
        VpnServer server = workingServers[row];

        ui->testLogArea->clear();
        vpnManager->connectToServer(server);
    }
}

void MainWindow::on_disconnectButton_clicked() {
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

void MainWindow::onServersDownloaded(const QList<VpnServer>& servers) {
    this->servers = servers;
    workingServers.clear();
    currentTestIndex = 0;
    testInProgress = true;

    int totalServers = servers.size();
    addLog(QString("✅ Загружено %1 серверов, начинаю тестирование ВСЕХ...").arg(totalServers), "SUCCESS");

    ui->testLogArea->append(QString("✅ Загружено %1 серверов").arg(totalServers));
    ui->testLogArea->append(QString("🔍 Начинаю тестирование ВСЕХ %1 серверов...").arg(totalServers));

    ui->testProgressBar->setValue(0);
    ui->testProgressLabel->setText("0%");
    ui->statsLabel->setText("Статус: Тестирование ВСЕХ серверов...");
    ui->workingCountLabel->setText("✅: 0");
    ui->failedCountLabel->setText("❌: 0");
    ui->testedCountLabel->setText(QString("📊: 0/%1").arg(totalServers));
    ui->countryCountLabel->setText("🌍: 0 стран");

    ui->stopTestButton->setEnabled(true);
    ui->refreshButton->setEnabled(false);
    isTestingAll = true;

    testNextServer();
}

void MainWindow::onDownloadError(const QString& error) {
    addLog(error, "ERROR");
    ui->testLogArea->append(QString("\n❌ Ошибка: %1").arg(error));
    ui->refreshButton->setEnabled(true);
    ui->stopTestButton->setEnabled(false);
    testInProgress = false;
    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(0);
    ui->statusLabel->setText("Ошибка загрузки");

    QMessageBox::critical(this, "Ошибка загрузки", error);
}

void MainWindow::onDownloadProgress(int progress) {
    ui->progressBar->setValue(progress);
}

void MainWindow::onDownloadLog(const QString& message) {
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    ui->testLogArea->append(QString("[%1] %2").arg(timestamp).arg(message));
}

void MainWindow::onTestFinished(bool success, const QString& message, int pingMs) {
    Q_UNUSED(pingMs);

    QString level = success ? "SUCCESS" : "ERROR";
    if (testerThread) {
        QString serverName = testerThread->property("serverName").toString();
        if (!serverName.isEmpty()) {
            addLog(QString("%1: %2").arg(serverName).arg(message), level);
        }
    }
}

void MainWindow::onTestProgress(const QString& message) {
    if (message.contains("успешно", Qt::CaseInsensitive) ||
        message.contains("ошибка", Qt::CaseInsensitive) ||
        message.contains("таймаут", Qt::CaseInsensitive) ||
        message.contains("останов", Qt::CaseInsensitive)) {
        QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    ui->testLogArea->append(QString("[%1] %2").arg(timestamp).arg(message));
        }
}

void MainWindow::onRealTestFinished(bool success, const QString& message) {
    // Не обрабатываем, если поток был удален
    if (!sender()) {
        return;
    }

    QString serverName;
    if (sender()) {
        serverName = sender()->property("serverName").toString();
    }

    if (!serverName.isEmpty()) {
        QString level = success ? "SUCCESS" : "ERROR";
        addLog(QString("%1: %2").arg(serverName).arg(message), level);

        // Обновляем сервер только если тест успешен
        if (success) {
            for (int i = 0; i < servers.size(); ++i) {
                if (servers[i].name == serverName) {
                    servers[i].tested = true;
                    servers[i].available = true;
                    servers[i].realConnectionTested = true;

                    QRegularExpression re("за (\\d+)ms");
                    QRegularExpressionMatch match = re.match(message);
                    if (match.hasMatch()) {
                        servers[i].testPing = match.captured(1).toInt();
                    } else {
                        servers[i].testPing = 100;
                    }

                    // Проверяем, нет ли уже этого сервера в списке
                    bool alreadyExists = false;
                    for (const auto& server : workingServers) {
                        if (server.name == servers[i].name) {
                            alreadyExists = true;
                            break;
                        }
                    }

                    if (!alreadyExists) {
                        workingServers.append(servers[i]);
                    }
                    break;
                }
            }

            updateStats();
        }
    }
}

void MainWindow::onVpnStatus(const QString& type, const QString& message) {
    if (type == "success") {
        ui->vpnStatusLabel->setText(QString("VPN: ✅ %1").arg(message));
        ui->vpnStatusLabel->setStyleSheet("color: #00aa00; font-weight: bold;");
    } else if (type == "error") {
        ui->vpnStatusLabel->setText(QString("VPN: ❌ %1").arg(message));
        ui->vpnStatusLabel->setStyleSheet("color: #cc0000; font-weight: bold;");
    } else if (type == "warning") {
        ui->vpnStatusLabel->setText(QString("VPN: ⚠️ %1").arg(message));
        ui->vpnStatusLabel->setStyleSheet("color: #cc8800; font-weight: bold;");
    } else if (type == "info") {
        ui->vpnStatusLabel->setText(QString("VPN: 🔄 %1").arg(message));
        ui->vpnStatusLabel->setStyleSheet("color: #0066cc; font-weight: bold;");
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

    QVariantMap info = vpnManager->getConnectionInfo();
    if (!info.isEmpty()) {
        QString infoText = QString("Сервер: %1 | Страна: %2 | IP: %3")
        .arg(info["server"].toString())
        .arg(info["country"].toString())
        .arg(info["ip"].toString());
        ui->vpnInfoLabel->setText(infoText);
    }

    updateServerList();
}

void MainWindow::onVpnDisconnected() {
    ui->connectButton->setEnabled(true);
    ui->disconnectButton->setEnabled(false);
    ui->vpnStatusFrame->setVisible(false);
    ui->vpnInfoLabel->setText("");
    updateServerList();
}

void MainWindow::testNextServer() {
    if (!isTestingAll || currentTestIndex >= servers.size()) {
        finishTesting();
        return;
    }

    // Если есть активный поток, не создаем новый
    if (testerThread && testerThread->isRunning()) {
        return;
    }

    VpnServer server = servers[currentTestIndex];
    currentTestIndex++;

    int progress = static_cast<int>((currentTestIndex * 100) / servers.size());
    ui->testProgressBar->setValue(progress);
    ui->testProgressLabel->setText(QString("%1%").arg(progress));
    ui->testedCountLabel->setText(QString("📊: %1/%2").arg(currentTestIndex).arg(servers.size()));

    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    QString logMsg = QString("[%1] 🔍 Тестирую %2 (%3/%4)...")
    .arg(timestamp)
    .arg(server.name)
    .arg(currentTestIndex)
    .arg(servers.size());

    ui->testLogArea->append(logMsg);

    // Создаем новый поток
    testerThread = new ServerTesterThread(server.ip, server.name, this);
    testerThread->setProperty("serverName", server.name);
    testerThread->setOvpnConfig(server.configBase64);

    // Подключаем сигналы с Qt::QueuedConnection для безопасности
    connect(testerThread, &ServerTesterThread::testProgress,
            this, &MainWindow::onTestProgress, Qt::QueuedConnection);

    connect(testerThread, &ServerTesterThread::realConnectionTestFinished,
            this, &MainWindow::onRealTestFinished, Qt::QueuedConnection);

    // Автоматически удаляем поток после завершения
    connect(testerThread, &QThread::finished, this, [this]() {
        if (testerThread) {
            testerThread->deleteLater();
            testerThread = nullptr;
            QMetaObject::invokeMethod(this, &MainWindow::testNextServer, Qt::QueuedConnection);
        }
    });

    testerThread->start();
}

void MainWindow::onTestTimeout() {
    // Эта функция теперь не используется напрямую
}

void MainWindow::updateStats() {
    int working = workingServers.size();
    int failed = currentTestIndex - working;

    ui->workingCountLabel->setText(QString("✅: %1").arg(working));
    ui->failedCountLabel->setText(QString("❌: %1").arg(failed));

    if (!workingServers.isEmpty()) {
        QSet<QString> countries;
        for (const VpnServer& s : workingServers) {
            countries.insert(s.country);
        }
        ui->countryCountLabel->setText(QString("🌍: %1 стран").arg(countries.size()));
    }
}

void MainWindow::finishTesting() {
    isTestingAll = false;
    testInProgress = false;

    // Корректно останавливаем тестер если он работает
    if (testerThread) {
        disconnect(testerThread, nullptr, this, nullptr);

        if (testerThread->isRunning()) {
            testerThread->quit();
            testerThread->wait(1000);
        }

        testerThread->deleteLater();
        testerThread = nullptr;
    }

    // Сортируем рабочие серверы по скорости
    std::sort(workingServers.begin(), workingServers.end(),
              [](const VpnServer& a, const VpnServer& b) {
                  return a.speedMbps > b.speedMbps;
              });

    int totalServers = servers.size();
    int workingCount = workingServers.size();

    addLog(QString("Проверка завершена! Рабочих серверов: %1 из %2")
    .arg(workingCount).arg(totalServers), "SUCCESS");

    ui->testLogArea->append("\n" + QString("=").repeated(60));
    ui->testLogArea->append("✅ ПРОВЕРКА ЗАВЕРШЕНА!");
    ui->testLogArea->append(QString("📊 Всего серверов: %1").arg(totalServers));
    ui->testLogArea->append(QString("✅ Рабочих: %1").arg(workingCount));
    ui->testLogArea->append(QString("❌ Не рабочих: %1").arg(totalServers - workingCount));

    if (!workingServers.isEmpty()) {
        QSet<QString> countries;
        for (const VpnServer& s : workingServers) {
            countries.insert(s.country);
        }
        ui->testLogArea->append(QString("🌍 Стран: %1").arg(countries.size()));

        QStringList countryList = countries.values();
        countryList.sort();
        ui->testLogArea->append(QString("📍 Список стран: %1").arg(countryList.join(", ")));
    }

    updateServerList();
    ui->statusLabel->setText(QString("Готово: %1 рабочих из %2").arg(workingCount).arg(totalServers));
    ui->statsLabel->setText("Статус: Завершено");

    ui->refreshButton->setEnabled(true);
    ui->stopTestButton->setEnabled(false);
    ui->testProgressBar->setValue(100);
    ui->testProgressLabel->setText("100%");

    if (workingCount > 0) {
        VpnServer fastestServer = workingServers.first();
        QSet<QString> countries;
        for (const VpnServer& s : workingServers) {
            countries.insert(s.country);
        }

        QMessageBox::information(this, "Проверка завершена",
                                 QString("✅ Найдено %1 рабочих VPN серверов из %2\n\n"
                                 "⚡ Самый быстрый сервер:\n"
                                 "   • %3\n"
                                 "   • Страна: %4\n"
                                 "   • Скорость: %5 Mbps\n\n"
                                 "🌍 Серверы из %6 стран")
                                 .arg(workingCount)
                                 .arg(totalServers)
                                 .arg(fastestServer.name)
                                 .arg(fastestServer.country)
                                 .arg(fastestServer.speedMbps)
                                 .arg(countries.size()));
    } else {
        QMessageBox::warning(this, "Проверка завершена",
                             QString("❌ Не найдено рабочих VPN серверов из %1\n\n"
                             "Возможные причины:\n"
                             "1. Проблемы с интернет-подключением\n"
                             "2. Все серверы VPNGate временно недоступны\n"
                             "3. Требуется настройка брандмауэра\n"
                             "4. Проблемы с конфигурацией OpenVPN")
                             .arg(totalServers));
    }
}

void MainWindow::stopTesting() {
    isTestingAll = false;
    testInProgress = false;

    // Корректно останавливаем тестер
    if (testerThread) {
        disconnect(testerThread, nullptr, this, nullptr);
        testerThread->cancel();

        // Даем время на завершение
        if (testerThread->isRunning()) {
            if (!testerThread->wait(3000)) {
                testerThread->terminate();
                testerThread->wait(2000);
            }
        }

        testerThread->deleteLater();
        testerThread = nullptr;
    }

    int totalTested = currentTestIndex;
    int working = workingServers.size();

    addLog(QString("Проверка остановлена. Проверено: %1, рабочих: %2")
    .arg(totalTested).arg(working), "WARNING");

    ui->testLogArea->append("\n" + QString("=").repeated(60));
    ui->testLogArea->append("⏹️ ПРОВЕРКА ОСТАНОВЛЕНА ПОЛЬЗОВАТЕЛЕМ");
    ui->testLogArea->append(QString("📊 Проверено серверов: %1").arg(totalTested));
    ui->testLogArea->append(QString("✅ Рабочих: %1").arg(working));
    ui->testLogArea->append(QString("❌ Не рабочих: %1").arg(totalTested - working));

    ui->statusLabel->setText("Проверка остановлена");
    ui->statsLabel->setText("Статус: Остановлено");

    updateServerList();

    ui->refreshButton->setEnabled(true);
    ui->stopTestButton->setEnabled(false);
}

void MainWindow::updateServerList() {
    ui->serverList->clear();

    auto status = vpnManager->getStatus();
    QString currentVpnServer = status.first == "connected" ? status.second : QString();

    for (const VpnServer& server : workingServers) {
        double speed = server.speedMbps;
        int ping = server.testPing;

        QString statusIcon;
        QColor statusColor;

        if (speed > 50) {
            statusIcon = "⚡ ";
            statusColor = QColor("#00cc00");
        } else if (speed > 10) {
            statusIcon = "🟢 ";
            statusColor = QColor("#00aa00");
        } else {
            statusIcon = "🟡 ";
            statusColor = QColor("#cccc00");
        }

        if (ping > 300) {
            statusIcon = "🐌 ";
            statusColor = QColor("#cc8800");
        }

        QString currentMarker = currentVpnServer == server.name ? " ⚡" : "";
        QString displayName = QString("%1%2 - %3 Mbps (%4ms)%5")
        .arg(statusIcon)
        .arg(server.name)
        .arg(server.speedMbps, 0, 'f', 1)
        .arg(ping)
        .arg(currentMarker);

        QListWidgetItem* item = new QListWidgetItem(displayName);
        item->setForeground(statusColor);

        if (currentVpnServer == server.name) {
            item->setBackground(QColor("#e3f2fd"));
        }

        ui->serverList->addItem(item);
    }

    ui->serverList->setCurrentRow(-1);
    ui->infoText->clear();
    ui->testSelectedButton->setEnabled(!workingServers.isEmpty());

    if (status.first == "connected") {
        ui->connectButton->setEnabled(false);
        ui->disconnectButton->setEnabled(true);
    } else if (status.first == "disconnected" && !workingServers.isEmpty()) {
        ui->connectButton->setEnabled(true);
        ui->disconnectButton->setEnabled(false);
    } else {
        ui->connectButton->setEnabled(false);
        ui->disconnectButton->setEnabled(false);
    }
}

void MainWindow::updateSelection() {
    int currentRow = ui->serverList->currentRow();

    if (currentRow >= 0 && currentRow < workingServers.size()) {
        VpnServer server = workingServers[currentRow];

        QStringList infoLines;
        infoLines.append(QString("Сервер: %1").arg(server.name));
        infoLines.append(QString("Страна: %1").arg(server.country));
        infoLines.append(QString("IP адрес: %1").arg(server.ip));
        infoLines.append(QString("Скорость: %1 Mbps").arg(server.speedMbps, 0, 'f', 1));
        infoLines.append(QString("Пинг: %1 ms").arg(server.testPing));
        infoLines.append(QString("Рейтинг: %1").arg(server.score));
        infoLines.append("Статус: ✅ Проверен и рабочий");
        infoLines.append(QString("Сессии: %1").arg(server.sessions));
        infoLines.append(QString("Аптайм: %1").arg(server.uptime));

        auto status = vpnManager->getStatus();
        if (status.first == "connected" && server.name == status.second) {
            infoLines.append("");
            infoLines.append("⚡ В данный момент подключен к этому серверу");
        }

        ui->infoText->setPlainText(infoLines.join('\n'));
        ui->testSelectedButton->setEnabled(true);

        if (status.first == "disconnected") {
            ui->connectButton->setEnabled(true);
        }
    } else {
        ui->infoText->clear();
        ui->testSelectedButton->setEnabled(false);
        ui->connectButton->setEnabled(false);
    }
}

void MainWindow::manualTestServer(const VpnServer& server) {
    if (testerThread && testerThread->isRunning()) {
        addLog("Проверка уже выполняется", "WARNING");
        return;
    }

    ui->statusLabel->setText(QString("Проверяю %1...").arg(server.name));
    ui->testSelectedButton->setEnabled(false);
    ui->testLogArea->append(QString("\n🔍 Ручная проверка %1...").arg(server.name));

    // Очищаем старый поток
    if (testerThread) {
        testerThread->deleteLater();
        testerThread = nullptr;
    }

    testerThread = new ServerTesterThread(server.ip, server.name, this);
    testerThread->setProperty("serverName", server.name);
    testerThread->setOvpnConfig(server.configBase64);

    connect(testerThread, &ServerTesterThread::realConnectionTestFinished,
            this, [this, server](bool success, const QString& msg) {
                if (success) {
                    addLog(QString("✅ %1: %2").arg(server.name).arg(msg), "SUCCESS");
                } else {
                    addLog(QString("❌ %1: %2").arg(server.name).arg(msg), "ERROR");

                    // Удаляем сервер из списка рабочих
                    for (int i = 0; i < workingServers.size(); ++i) {
                        if (workingServers[i].name == server.name) {
                            workingServers.removeAt(i);
                            updateServerList();
                            break;
                        }
                    }
                }
                ui->statusLabel->setText("Проверка завершена");
                ui->testSelectedButton->setEnabled(true);

                // Очищаем указатель
                if (testerThread) {
                    testerThread->deleteLater();
                    testerThread = nullptr;
                }
            }, Qt::QueuedConnection);

    connect(testerThread, &ServerTesterThread::testProgress,
            this, &MainWindow::onTestProgress, Qt::QueuedConnection);

    testerThread->start();
}

void MainWindow::cleanupOldProcesses() {
    QProcess process;
    #ifdef Q_OS_LINUX
    // Безопасная очистка процессов OpenVPN
    process.start("pkill", QStringList() << "-SIGTERM" << "-f" << "openvpn.*tun999");
    process.waitForFinished(300);
    QThread::msleep(200);

    process.start("pkill", QStringList() << "-SIGKILL" << "-f" << "openvpn.*tun999");
    process.waitForFinished(300);

    process.start("pkill", QStringList() << "-SIGTERM" << "-f" << "openvpn.*vpngate");
    process.waitForFinished(300);
    QThread::msleep(200);

    process.start("pkill", QStringList() << "-SIGKILL" << "-f" << "openvpn.*vpngate");
    process.waitForFinished(300);
    #endif
}

void MainWindow::addLog(const QString& message, const QString& level) {
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    QString prefix;

    if (level == "ERROR") {
        prefix = "❌ ";
    } else if (level == "WARNING") {
        prefix = "⚠️  ";
    } else if (level == "SUCCESS") {
        prefix = "✅ ";
    } else if (level == "INFO") {
        prefix = "ℹ️  ";
    }

    QString logEntry = QString("[%1] %2%3").arg(timestamp).arg(prefix).arg(message);

    // Проверяем, нет ли уже такого сообщения
    if (!logMessages.isEmpty() && logMessages.last().contains(message)) {
        return; // Пропускаем дубликат
    }

    logMessages.append(logEntry);

    if (logMessages.size() > 1000) {
        logMessages = logMessages.mid(logMessages.size() - 1000);
    }

    ui->logArea->append(logEntry);
}

void MainWindow::saveLogs() {
    QString downloadFolder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/vpngate_logs";
    QDir dir;
    if (!dir.exists(downloadFolder)) {
        dir.mkpath(downloadFolder);
    }

    QString logFile = downloadFolder + "/vpngate_" +
    QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".txt";

    QFile file(logFile);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&file);

        for (const QString& log : logMessages) {
            stream << log << "\n";
        }

        file.close();

        addLog(QString("Лог сохранен в файл: %1").arg(logFile), "SUCCESS");
        QMessageBox::information(this, "Успех",
                                 QString("Лог успешно сохранен в файл:\n%1").arg(logFile));
    } else {
        QMessageBox::warning(this, "Ошибка", "Не удалось сохранить лог");
    }
}
