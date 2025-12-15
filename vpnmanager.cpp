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
    if (isConnected) {
        emit connectionStatus("warning", "Уже подключено к VPN");
        return;
    }

    try {
        currentServer = server;
        emit connectionStatus("info", QString("Подключаюсь к %1...").arg(server.name));
        emit connectionLog(QString("🚀 Начинаю подключение к %1").arg(server.name));

        QByteArray configData = QByteArray::fromBase64(server.configBase64.toLatin1());
        QString configContent = QString::fromUtf8(configData);

        // Создаем временный файл в домашней директории, чтобы он не удалялся автоматически
        QString tempDir = QDir::tempPath();
        QString tempFileName = QString("vpngate_%1.ovpn").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss_zzz"));
        configPath = QDir(tempDir).filePath(tempFileName);

        QFile configFile(configPath);
        if (!configFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            emit connectionStatus("error", "Не удалось создать конфиг");
            emit connectionLog(QString("❌ Ошибка создания файла: %1").arg(configFile.errorString()));
            return;
        }

        QString enhancedConfig = enhanceConfigForConnection(configContent, server);
        QTextStream stream(&configFile);
        stream << enhancedConfig;
        configFile.close();

        emit connectionLog(QString("📄 Конфиг сохранен: %1").arg(configPath));

        // Проверяем существование файла
        if (!QFile::exists(configPath)) {
            emit connectionStatus("error", "Файл конфигурации не найден");
            emit connectionLog("❌ Файл конфигурации был удален");
            return;
        }

        QStringList cmd = {
            "sudo",
            "openvpn",
            "--config", configPath,
            "--auth-user-pass", "/dev/stdin",
            "--verb", "3",
            "--connect-timeout", "30"
        };

        emit connectionLog("🔧 Запускаю OpenVPN...");

        process = new QProcess(this);
        process->setProcessChannelMode(QProcess::MergedChannels);

        connect(process, &QProcess::readyRead, this, &VpnManager::readVpnOutput);
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &VpnManager::vpnProcessFinished);

        // Запускаем процесс
        process->start(cmd[0], cmd.mid(1));

        if (!process->waitForStarted(3000)) {
            emit connectionStatus("error", "Не удалось запустить OpenVPN");
            emit connectionLog(QString("❌ Ошибка запуска: %1").arg(process->errorString()));
            cleanup();
            return;
        }

        // Отправляем учетные данные
        process->write("vpn\nvpn\n");
        process->closeWriteChannel();

        // Таймер для проверки подключения
        QTimer::singleShot(30000, this, [this]() {
            if (!isConnected && process && process->state() == QProcess::Running) {
                emit connectionStatus("error", "Таймаут подключения");
                emit connectionLog("⏰ Таймаут подключения (30 секунд)");
                disconnect();
            }
        });

    } catch (const std::exception& e) {
        emit connectionStatus("error", QString("Ошибка подключения: %1").arg(e.what()));
        cleanup();
    }
}

void VpnManager::disconnect() {
    if (isConnected) {
        emit connectionStatus("info", "Отключаюсь...");
        emit connectionLog("🔌 Отключаю VPN...");
    }

    if (process && process->state() == QProcess::Running) {
        process->terminate();
        if (!process->waitForFinished(5000)) {
            process->kill();
            process->waitForFinished(1000);
        }
    }

    cleanup();

    if (isConnected) {
        isConnected = false;
        emit disconnected();
        emit connectionStatus("info", "Отключено");
    }
}

QPair<QString, QString> VpnManager::getStatus() const {
    if (isConnected) {
        return qMakePair(QString("connected"), currentServer.name);
    } else if (process && process->state() == QProcess::Running) {
        return qMakePair(QString("connecting"), QString("Подключение..."));
    } else {
        return qMakePair(QString("disconnected"), QString("Отключено"));
    }
}

QVariantMap VpnManager::getConnectionInfo() const {
    if (isConnected) {
        QVariantMap info;
        info["server"] = currentServer.name;
        info["country"] = currentServer.country;
        info["ip"] = currentServer.ip;
        info["speed"] = currentServer.speedMbps;
        return info;
    }
    return QVariantMap();
}

void VpnManager::readVpnOutput() {
    if (!process) return;

    while (process->canReadLine()) {
        QString line = QString::fromUtf8(process->readLine()).trimmed();
        if (!line.isEmpty()) {
            emit connectionLog(QString("🔍 %1").arg(line));

            if (line.contains("Initialization Sequence Completed")) {
                isConnected = true;
                emit connectionStatus("success", QString("✅ Подключено к %1").arg(currentServer.name));
                emit connectionLog("🎉 VPN подключение установлено!");
                emit connected(currentServer.name);
            } else if (line.contains("AUTH_FAILED")) {
                emit connectionStatus("error", "Ошибка аутентификации");
                emit connectionLog("❌ Неверный логин/пароль");
                disconnect();
            } else if (line.contains("TLS Error")) {
                emit connectionStatus("error", "Ошибка TLS");
                emit connectionLog("❌ Ошибка TLS handshake");
            } else if (line.contains("SIGTERM") || line.contains("process exiting")) {
                // Процесс завершается
                if (isConnected) {
                    isConnected = false;
                    emit disconnected();
                }
            }
        }
    }
}

void VpnManager::vpnProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    Q_UNUSED(exitCode);
    Q_UNUSED(exitStatus);

    if (isConnected) {
        isConnected = false;
        emit disconnected();
        emit connectionStatus("info", "Соединение разорвано");
    } else if (process && process->exitCode() != 0) {
        emit connectionStatus("error", QString("Ошибка подключения (код: %1)").arg(process->exitCode()));
    }

    cleanup();
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

void VpnManager::cleanup() {
    // Удаляем временный файл через 5 секунд, чтобы дать OpenVPN время прочитать его
    if (!configPath.isEmpty() && QFile::exists(configPath)) {
        QTimer::singleShot(5000, [configPath = this->configPath]() {
            if (QFile::exists(configPath)) {
                QFile::remove(configPath);
            }
        });
        configPath.clear();
    }

    if (process) {
        process->deleteLater();
        process = nullptr;
    }
}
