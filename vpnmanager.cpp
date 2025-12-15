#include "vpnmanager.h"
#include <QTemporaryFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QByteArray>
#include <QFile>
#include <QTimer>
#include <QDir>

VpnManager::VpnManager(QObject *parent)
: QObject(parent), process(nullptr), isConnected(false) {
}

void VpnManager::connectToServer(const VpnServer& server) {
    QMutexLocker locker(&mutex);
    if (isConnected) {
        locker.unlock();
        emit connectionStatus("warning", "Уже подключено к VPN");
        return;
    }

    locker.unlock();
    try {
        locker.relock();
        currentServer = server;
        locker.unlock();
        
        emit connectionStatus("info", QString("Подключаюсь к %1...").arg(server.name));
        emit connectionLog(QString("🚀 Начинаю подключение к %1").arg(server.name));

        QByteArray configData = QByteArray::fromBase64(server.configBase64.toLatin1());
        QString configContent = QString::fromUtf8(configData);

        // Создаем временный файл в домашней директории, чтобы он не удалялся автоматически
        QString tempDir = QDir::tempPath();
        QString tempFileName = QString("vpngate_%1.ovpn").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss_zzz"));
        QString configPathLocal = QDir(tempDir).filePath(tempFileName);

        QFile configFile(configPathLocal);
        if (!configFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            locker.unlock();
            emit connectionStatus("error", "Не удалось создать конфиг");
            emit connectionLog(QString("❌ Ошибка создания файла: %1").arg(configFile.errorString()));
            return;
        }

        locker.relock();
        QString enhancedConfig = enhanceConfigForConnection(configContent, server);
        locker.unlock();
        
        QTextStream stream(&configFile);
        stream << enhancedConfig;
        configFile.close();

        locker.relock();
        this->configPath = configPathLocal;
        locker.unlock();
        
        emit connectionLog(QString("📄 Конфиг сохранен: %1").arg(configPathLocal));

        // Проверяем существование файла
        if (!QFile::exists(configPathLocal)) {
            emit connectionStatus("error", "Файл конфигурации не найден");
            emit connectionLog("❌ Файл конфигурации был удален");
            return;
        }

        QStringList cmd = {
            "sudo",
            "openvpn",
            "--config", configPathLocal,
            "--auth-user-pass", "/dev/stdin",
            "--verb", "3",
            "--connect-timeout", "30"
        };

        emit connectionLog("🔧 Запускаю OpenVPN...");

        locker.relock();
        safeCleanup(); // Убедимся, что старый процесс удален
        process = new QProcess(this);
        process->setProcessChannelMode(QProcess::MergedChannels);
        locker.unlock();

        // Подключаем сигналы с Qt::QueuedConnection для избежания гонок данных
        connect(process, &QProcess::readyRead, this, &VpnManager::readVpnOutput, Qt::QueuedConnection);
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &VpnManager::vpnProcessFinished, Qt::QueuedConnection);

        // Запускаем процесс
        locker.relock();
        QProcess* localProcess = process.data();
        locker.unlock();
        
        if (localProcess) {
            localProcess->start(cmd[0], cmd.mid(1));

            if (!localProcess->waitForStarted(3000)) {
                emit connectionStatus("error", "Не удалось запустить OpenVPN");
                emit connectionLog(QString("❌ Ошибка запуска: %1").arg(localProcess->errorString()));
                safeCleanup();
                return;
            }

            // Отправляем учетные данные
            localProcess->write("vpn\nvpn\n");
            localProcess->closeWriteChannel();

            // Таймер для проверки подключения
            QTimer::singleShot(30000, this, [this, localProcess]() {
                QMutexLocker timerLocker(&mutex);
                if (!isConnected && localProcess && localProcess->state() == QProcess::Running) {
                    timerLocker.unlock();
                    emit connectionStatus("error", "Таймаут подключения");
                    emit connectionLog("⏰ Таймаут подключения (30 секунд)");
                    disconnect();
                }
            });
        }

    } catch (const std::exception& e) {
        emit connectionStatus("error", QString("Ошибка подключения: %1").arg(e.what()));
        safeCleanup();
    }
}

void VpnManager::disconnect() {
    QMutexLocker locker(&mutex);
    bool wasConnected = isConnected;
    locker.unlock();
    
    if (wasConnected) {
        emit connectionStatus("info", "Отключаюсь...");
        emit connectionLog("🔌 Отключаю VPN...");
    }

    locker.relock();
    QProcess* localProcess = process.data();
    locker.unlock();
    
    if (localProcess && localProcess->state() == QProcess::Running) {
        // Безопасное завершение процесса - сначала SIGTERM, затем SIGKILL
        localProcess->terminate();
        if (!localProcess->waitForFinished(5000)) {
            localProcess->kill();
            localProcess->waitForFinished(1000);
        }
    }

    safeCleanup();

    locker.relock();
    if (wasConnected) {
        isConnected = false;
        locker.unlock();
        emit disconnected();
        emit connectionStatus("info", "Отключено");
    } else {
        locker.unlock();
    }
}

QPair<QString, QString> VpnManager::getStatus() const {
    QMutexLocker locker(&mutex);
    if (isConnected) {
        return qMakePair(QString("connected"), currentServer.name);
    } else {
        QProcess* localProcess = process.data();
        if (localProcess && localProcess->state() == QProcess::Running) {
            return qMakePair(QString("connecting"), QString("Подключение..."));
        } else {
            return qMakePair(QString("disconnected"), QString("Отключено"));
        }
    }
}

QVariantMap VpnManager::getConnectionInfo() const {
    QMutexLocker locker(&mutex);
    if (isConnected) {
        VpnServer localServer = currentServer;
        locker.unlock();
        
        QVariantMap info;
        info["server"] = localServer.name;
        info["country"] = localServer.country;
        info["ip"] = localServer.ip;
        info["speed"] = localServer.speedMbps;
        return info;
    }
    locker.unlock();
    return QVariantMap();
}

void VpnManager::readVpnOutput() {
    QMutexLocker locker(&mutex);
    QProcess* localProcess = process.data();
    if (!localProcess) {
        locker.unlock();
        return;
    }

    locker.unlock();
    while (localProcess->canReadLine()) {
        QString line = QString::fromUtf8(localProcess->readLine()).trimmed();
        if (!line.isEmpty()) {
            emit connectionLog(QString("🔍 %1").arg(line));

            locker.relock();
            if (line.contains("Initialization Sequence Completed")) {
                isConnected = true;
                VpnServer localServer = currentServer;
                locker.unlock();
                emit connectionStatus("success", QString("✅ Подключено к %1").arg(localServer.name));
                emit connectionLog("🎉 VPN подключение установлено!");
                emit connected(localServer.name);
            } else if (line.contains("AUTH_FAILED")) {
                locker.unlock();
                emit connectionStatus("error", "Ошибка аутентификации");
                emit connectionLog("❌ Неверный логин/пароль");
                disconnect();
            } else if (line.contains("TLS Error")) {
                locker.unlock();
                emit connectionStatus("error", "Ошибка TLS");
                emit connectionLog("❌ Ошибка TLS handshake");
            } else if (line.contains("SIGTERM") || line.contains("process exiting")) {
                // Процесс завершается
                if (isConnected) {
                    isConnected = false;
                    locker.unlock();
                    emit disconnected();
                } else {
                    locker.unlock();
                }
            } else {
                locker.unlock();
            }
        }
    }
}

void VpnManager::vpnProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    Q_UNUSED(exitCode);
    Q_UNUSED(exitStatus);

    QMutexLocker locker(&mutex);
    bool wasConnected = isConnected;
    if (wasConnected) {
        isConnected = false;
        locker.unlock();
        emit disconnected();
        emit connectionStatus("info", "Соединение разорвано");
    } else {
        QProcess* localProcess = process.data();
        locker.unlock();
        if (localProcess && localProcess->exitCode() != 0) {
            emit connectionStatus("error", QString("Ошибка подключения (код: %1)").arg(localProcess->exitCode()));
        }
    }

    safeCleanup();
}

QString VpnManager::enhanceConfigForConnection(const QString& configContent, const VpnServer& server) {
    Q_UNUSED(server);

    QStringList lines = configContent.split('\n');
    QStringList enhancedLines;

    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }

        // Убираем problem строки
        if (trimmed.startsWith(";") || trimmed.startsWith("#")) {
            enhancedLines.append(trimmed);
            continue;
        }

        if (trimmed.startsWith("cipher ")) {
            QString cipher = trimmed.split(' ')[1];
            enhancedLines.append(QString("# %1").arg(trimmed));
            enhancedLines.append(QString("data-ciphers AES-256-GCM:AES-128-GCM:CHACHA20-POLY1305:%1").arg(cipher));
            enhancedLines.append(QString("data-ciphers-fallback %1").arg(cipher));
        } else if (trimmed.contains("fragment") || trimmed.contains("mssfix")) {
            // Пропускаем проблемные настройки
            enhancedLines.append(QString("# %1  # Отключено для совместимости").arg(trimmed));
        } else {
            enhancedLines.append(trimmed);
        }
    }

    // Добавляем необходимые опции
    enhancedLines.append("\n# Оптимизации для VPNGate");
    enhancedLines.append("remote-cert-tls server");
    enhancedLines.append("tls-client");
    enhancedLines.append("reneg-sec 0");
    enhancedLines.append("script-security 2");
    enhancedLines.append("auth-retry interact");
    enhancedLines.append("auth-nocache");
    enhancedLines.append("connect-retry 2");
    enhancedLines.append("connect-retry-max 3");
    enhancedLines.append("connect-timeout 30");
    enhancedLines.append("keepalive 10 60");
    enhancedLines.append("tun-mtu 1500");
    enhancedLines.append("persist-key");
    enhancedLines.append("persist-tun");
    enhancedLines.append("nobind");

    return enhancedLines.join('\n');
}

void VpnManager::safeCleanup() {
    QMutexLocker locker(&mutex);
    
    // Удаляем временный файл через 5 секунд, чтобы дать OpenVPN время прочитать его
    if (!configPath.isEmpty() && QFile::exists(configPath)) {
        QString localConfigPath = configPath;
        locker.unlock();
        
        QTimer::singleShot(5000, [localConfigPath]() {
            if (QFile::exists(localConfigPath)) {
                QFile::remove(localConfigPath);
            }
        });
        
        locker.relock();
        configPath.clear();
    }

    QProcess* localProcess = process.data();
    if (localProcess) {
        // Отключаем все сигналы от процесса
        disconnect(localProcess, nullptr, nullptr, nullptr);
        
        // Завершаем процесс корректно
        if (localProcess->state() == QProcess::Running) {
            localProcess->terminate();
            if (!localProcess->waitForFinished(5000)) {
                localProcess->kill();
                localProcess->waitForFinished(1000);
            }
        }
        
        // Удаляем через deleteLater для безопасного удаления в правильном потоке
        localProcess->deleteLater();
    }
    
    process.clear(); // Очищаем QPointer
}

void VpnManager::cleanup() {
    safeCleanup();
}
