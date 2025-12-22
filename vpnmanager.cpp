#include "vpnmanager.h"
#include <QTemporaryFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QByteArray>
#include <QFile>
#include <QTimer>
#include <QDir>
#include <QDateTime>
#include <QCoreApplication>
#include <QDebug>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

VpnManager::VpnManager(QObject *parent)
: QObject(parent), process(nullptr), m_isConnected(false), connectionTimeout(45) {
    // Игнорируем SIGPIPE для предотвращения крашей при записи в закрытый pipe
    std::signal(SIGPIPE, SIG_IGN);
}

QString VpnManager::findOpenVPN() {
    QStringList possiblePaths = {
        "openvpn",
        "/usr/sbin/openvpn",
        "/usr/bin/openvpn",
        "/sbin/openvpn",
        "/bin/openvpn",
        "/usr/local/sbin/openvpn",
        "/usr/local/bin/openvpn",
        "/opt/local/sbin/openvpn",
        "/opt/local/bin/openvpn"
    };

    QProcess whichProcess;
    whichProcess.start("which", QStringList() << "openvpn");
    whichProcess.waitForFinished(1000);

    if (whichProcess.exitCode() == 0) {
        QString path = QString::fromUtf8(whichProcess.readAllStandardOutput()).trimmed();
        if (!path.isEmpty() && QFile::exists(path)) {
            return path;
        }
    }

    for (const QString& path : possiblePaths) {
        QProcess testProcess;
        testProcess.start(path, QStringList() << "--version");
        testProcess.waitForFinished(1000);

        if (testProcess.exitCode() == 0) {
            return path;
        }
    }

    QProcess whereisProcess;
    whereisProcess.start("whereis", QStringList() << "-b" << "openvpn");
    whereisProcess.waitForFinished(1000);

    QString output = QString::fromUtf8(whereisProcess.readAllStandardOutput());
    if (output.contains("openvpn:")) {
        QStringList parts = output.split(':');
        if (parts.size() > 1) {
            QStringList bins = parts[1].trimmed().split(' ');
            for (const QString& bin : bins) {
                if (QFile::exists(bin) && QFileInfo(bin).isExecutable()) {
                    return bin;
                }
            }
        }
    }

    return QString();
}

void VpnManager::connectToServer(const VpnServer& server) {
    if (m_isConnected) {
        emit connectionStatus("warning", "Уже подключено к VPN");
        return;
    }

    try {
        currentServer = server;
        emit connectionStatus("info", QString("Подключаюсь к %1...").arg(server.name));
        emit connectionLog(QString("🚀 Начинаю подключение к %1").arg(server.name));

        QByteArray configData = QByteArray::fromBase64(server.configBase64.toLatin1());
        QString configContent = QString::fromUtf8(configData);

        QString tempDir = QDir::tempPath();
        QString safeServerName = server.name;
        safeServerName.replace(QRegularExpression("[^a-zA-Z0-9]"), "_");

        QString tempFileName = QString("vpngate_%1_%2.ovpn")
        .arg(safeServerName)
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
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

        if (!QFile::exists(configPath)) {
            emit connectionStatus("error", "Файл конфигурации не найден");
            emit connectionLog("❌ Файл конфигурации был удален");
            return;
        }

        QString openvpnPath = findOpenVPN();
        if (openvpnPath.isEmpty()) {
            emit connectionStatus("error", "OpenVPN не найден");
            emit connectionLog("❌ OpenVPN не найден в системе");
            return;
        }

        emit connectionLog(QString("✅ Найден OpenVPN: %1").arg(openvpnPath));

        QStringList cmd;
        if (getuid() == 0) {
            cmd = {
                openvpnPath,
                "--config", configPath,
                "--auth-user-pass", "/dev/stdin",
                "--verb", "3",
                "--connect-timeout", QString::number(connectionTimeout)
            };
        } else {
            cmd = {
                "sudo",
                openvpnPath,
                "--config", configPath,
                "--auth-user-pass", "/dev/stdin",
                "--verb", "3",
                "--connect-timeout", QString::number(connectionTimeout)
            };
        }

        emit connectionLog("🔧 Запускаю OpenVPN...");

        // Создаем новый процесс
        process = new QProcess(this);
        process->setProcessChannelMode(QProcess::MergedChannels);

        // Используем лямбду для безопасного чтения вывода
        connect(process, &QProcess::readyRead, this, [this]() {
            // QPointer автоматически проверяет, жив ли объект
            if (!process) {
                return;
            }

            QProcess* currentProcess = process.data();
            if (!currentProcess || currentProcess->state() == QProcess::NotRunning) {
                return;
            }

            try {
                while (currentProcess->canReadLine()) {
                    QByteArray data = currentProcess->readLine();
                    if (data.isEmpty()) break;

                    QString line = QString::fromUtf8(data).trimmed();
                    if (!line.isEmpty()) {
                        emit connectionLog(QString("🔍 %1").arg(line));

                        if (line.contains("Initialization Sequence Completed")) {
                            m_isConnected = true;
                            emit connectionStatus("success", QString("✅ Подключено к %1").arg(currentServer.name));
                            emit connectionLog("🎉 VPN подключение установлено!");
                            emit connected(currentServer.name);
                        } else if (line.contains("AUTH_FAILED")) {
                            emit connectionStatus("error", "Ошибка аутентификации");
                            emit connectionLog("❌ Неверный логин/пароль");
                            QTimer::singleShot(0, this, &VpnManager::disconnect);
                        } else if (line.contains("TLS Error")) {
                            emit connectionStatus("error", "Ошибка TLS");
                            emit connectionLog("❌ Ошибка TLS handshake");
                            QTimer::singleShot(0, this, &VpnManager::disconnect);
                        } else if (line.contains("SIGTERM") || line.contains("process exiting")) {
                            if (m_isConnected) {
                                m_isConnected = false;
                                emit disconnected();
                            }
                        } else if (line.contains("Error reading username from Auth authfile: /dev/stdin")) {
                            emit connectionStatus("error", "Ошибка переподключения");
                            emit connectionLog("❌ OpenVPN пытается перечитать учетные данные");
                            QTimer::singleShot(0, this, &VpnManager::disconnect);
                        } else if (line.contains("Options error: --keepalive conflicts with --ping")) {
                            emit connectionStatus("error", "Ошибка конфигурации OpenVPN");
                            emit connectionLog("❌ Конфликт опций keepalive и ping");
                            QTimer::singleShot(0, this, &VpnManager::disconnect);
                        }
                    }
                }
            } catch (const std::exception& e) {
                qDebug() << "Exception in readVpnOutput lambda:" << e.what();
            } catch (...) {
                qDebug() << "Unknown exception in readVpnOutput lambda";
            }
        });

        // Обработка завершения процесса
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &VpnManager::vpnProcessFinished);

        // Обработка ошибок запуска
        connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                emit connectionStatus("error", "Не удалось запустить OpenVPN");
                emit connectionLog("❌ Ошибка запуска OpenVPN");
                cleanup();
            }
        });

        // Запускаем процесс
        process->start(cmd[0], cmd.mid(1));

        if (!process->waitForStarted(3000)) {
            emit connectionStatus("error", "Не удалось запустить OpenVPN");
            emit connectionLog(QString("❌ Ошибка запуска: %1").arg(process->errorString()));
            cleanup();
            return;
        }

        // Отправляем учетные данные
        QString credentials = currentServer.username + "\n" + currentServer.password + "\n";
        if (process->state() == QProcess::Running) {
            process->write(credentials.toUtf8());
            process->waitForBytesWritten(1000);
            process->closeWriteChannel();
        }

        // Таймер для проверки подключения
        QTimer::singleShot(connectionTimeout * 1000, this, [this]() {
            if (!m_isConnected && process && process->state() == QProcess::Running) {
                emit connectionStatus("error", "Таймаут подключения");
                emit connectionLog(QString("⏰ Таймаут подключения (%1 секунд)").arg(connectionTimeout));
                disconnect();
            }
        });

    } catch (const std::exception& e) {
        emit connectionStatus("error", QString("Ошибка подключения: %1").arg(e.what()));
        cleanup();
    } catch (...) {
        emit connectionStatus("error", "Неизвестная ошибка подключения");
        cleanup();
    }
}

void VpnManager::disconnect() {
    if (m_isConnected) {
        emit connectionStatus("info", "Отключаюсь...");
        emit connectionLog("🔌 Отключаю VPN...");
    }

    if (process) {
        QProcess* currentProcess = process.data();
        if (currentProcess && currentProcess->state() == QProcess::Running) {
            emit connectionLog("📤 Отправляю сигнал завершения...");

            // Пробуем корректно завершить
            currentProcess->terminate();

            if (!currentProcess->waitForFinished(2000)) {
                emit connectionLog("⚠️ OpenVPN не отвечает, принудительно завершаю...");
                currentProcess->kill();
                currentProcess->waitForFinished(500);
            }
        }
    }

    cleanup();

    if (m_isConnected) {
        m_isConnected = false;
        emit disconnected();
        emit connectionStatus("info", "Отключено");
    }
}

QPair<QString, QString> VpnManager::getStatus() const {
    if (m_isConnected) {
        return qMakePair(QString("connected"), currentServer.name);
    } else if (process && process->state() == QProcess::Running) {
        return qMakePair(QString("connecting"), QString("Подключение..."));
    } else {
        return qMakePair(QString("disconnected"), QString("Отключено"));
    }
}

QVariantMap VpnManager::getConnectionInfo() const {
    if (m_isConnected) {
        QVariantMap info;
        info["server"] = currentServer.name;
        info["country"] = currentServer.country;
        info["ip"] = currentServer.ip;
        info["speed"] = currentServer.speedMbps;
        return info;
    }
    return QVariantMap();
}

void VpnManager::readVpnOutput()
{
    if (!process || !process->isOpen()) return;

    QByteArray output = process->readAllStandardOutput();
    QByteArray errors = process->readAllStandardError();
    QByteArray combined = output + errors;

    if (combined.isEmpty()) return;

    QTextStream stream(combined);
    QString line;
    while (stream.readLineInto(&line)) {
        line = line.trimmed();
        if (line.isEmpty()) continue;

        emit connectionLog(QString("🔍 %1").arg(line));

        // Успешное подключение
        if (line.contains("Initialization Sequence Completed")) {
            if (!m_isConnected) {
                m_isConnected = true;
                m_lastConnectionTime = QDateTime::currentDateTime();
                emit connectionEstablished();
                emit connectionStatus("success", "VPN подключение установлено!");
                emit connectionLog("🎉 VPN подключение установлено!");
            }
            continue;
        }

        // Ошибки аутентификации
        if (line.contains("AUTH_FAILED")) {
            emit connectionStatus("error", "Ошибка аутентификации на сервере");
            emit connectionLog("❌ Ошибка аутентификации: сервер отклонил логин/пароль");
            QTimer::singleShot(0, this, &VpnManager::disconnect);
            continue;
        }

        // Критические сетевые или TLS ошибки
        if (line.contains("TLS Error") ||
            line.contains("Connection reset") ||
            line.contains("TCP connection failed") ||
            line.contains("TLS key negotiation failed") ||
            line.contains("write UDP: Operation not permitted") ||
            line.contains("Bad encapsulated packet length") ||
            line.contains("Fatal TLS error")) {
            emit connectionStatus("error", "Сетевая или TLS ошибка");
        emit connectionLog("⚠️ Сетевая или TLS ошибка, попытка подключения прервана");
        QTimer::singleShot(0, this, &VpnManager::disconnect);
        continue;
            }

            // Ошибки конфигурации
            if (line.contains("Error reading username from Auth authfile") ||
                line.contains("Cannot open TUN/TAP dev") ||
                line.contains("Cannot allocate TUN/TAP dev dynamically")) {
                emit connectionStatus("error", "Ошибка конфигурации OpenVPN");
            emit connectionLog("❌ Ошибка конфигурации OpenVPN");
            QTimer::singleShot(0, this, &VpnManager::disconnect);
            continue;
                }

                // Ошибки сжатия данных (новая обработка)
                if (line.contains("Bad compression stub decompression header byte") ||
                    line.contains("Decompress error") ||
                    line.contains("bad compression stub decompression header")) {
                    emit connectionStatus("warning", "Конфликт настроек сжатия");
                emit connectionLog("⚠️ Конфликт настроек сжатия с сервером");

                // Пробуем исправить настройки сжатия на лету
                emit connectionLog("🔄 Пытаюсь исправить настройки сжатия...");

                // Отправляем команду для переподключения с новыми настройками
                if (process && process->state() == QProcess::Running) {
                    // Отправляем SIGUSR1 для мягкого переподключения
                    process->write("signal SIGUSR1\n");
                    process->waitForBytesWritten(100);

                    // Также отправляем команду для изменения настроек сжатия
                    QString restartCommand = "echo \"comp-lzo adaptive\" > /dev/stdin\n";
                    process->write(restartCommand.toUtf8());
                    process->waitForBytesWritten(100);
                }

                // Если через 5 секунд ошибка сохраняется, переподключаемся полностью
                QTimer::singleShot(5000, this, [this]() {
                    if (m_isConnected && process && process->state() == QProcess::Running) {
                        // Проверяем, сохраняется ли проблема
                        emit connectionLog("🔄 Полное переподключение для исправления сжатия...");

                        // Сохраняем текущий сервер
                        VpnServer tempServer = currentServer;

                        // Отключаемся
                        disconnect();

                        // Переподключаемся с задержкой
                        QTimer::singleShot(2000, this, [this, tempServer]() {
                            emit connectionLog("🔄 Переподключаюсь с исправленными настройками сжатия...");
                            connectToServer(tempServer);
                        });
                    }
                });

                continue;
                    }

                    // Проблемы с маршрутизацией
                    if (line.contains("ROUTE: route addition failed") ||
                        line.contains("Cannot ioctl TUNSETIFF") ||
                        line.contains("TUN/TAP device") ||
                        line.contains("route gateway is not reachable")) {
                        emit connectionStatus("warning", "Проблема с маршрутизацией");
                    emit connectionLog("⚠️ Возможная проблема с маршрутами VPN");

                    // Пробуем исправить, отправив команду перенастройки маршрутов
                    if (process && process->state() == QProcess::Running) {
                        QString routeCommand = "echo \"route-nopull\" > /dev/stdin\n";
                        process->write(routeCommand.toUtf8());
                        process->waitForBytesWritten(100);
                    }

                    continue;
                        }

                        // Предупреждения о deprecated опциях
                        if (line.contains("deprecated") || line.contains("WARNING:")) {
                            emit connectionLog(QString("ℹ️ %1").arg(line));
                            continue;
                        }

                        // Неизвестные критические ошибки
                        if (line.contains("Exiting due to fatal error") ||
                            line.contains("SIGTERM[soft,") ||
                            line.contains("Process exiting")) {
                            if (m_isConnected) {
                                m_isConnected = false;
                                emit connectionLost();
                                emit connectionStatus("info", "Соединение закрыто");
                                emit connectionLog("🔌 Соединение с VPN завершено");
                            }
                            }

                            // Информационные сообщения о переподключении
                            if (line.contains("SIGUSR1") || line.contains("soft reset")) {
                                emit connectionLog(QString("🔄 %1").arg(line));
                                if (line.contains("connection reset")) {
                                    emit connectionStatus("info", "Переподключение...");
                                }
                            }
    }
}

void VpnManager::vpnProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    Q_UNUSED(exitCode);
    Q_UNUSED(exitStatus);

    bool wasConnected = m_isConnected;

    if (wasConnected) {
        m_isConnected = false;
        emit disconnected();
        emit connectionStatus("info", "Соединение разорвано");
        emit connectionLog("🔗 VPN соединение закрыто");
    } else if (process && process->exitCode() != 0) {
        QString exitCodeStr = QString::number(process->exitCode());
        emit connectionStatus("error", QString("Ошибка подключения (код: %1)").arg(exitCodeStr));
    }

    cleanup();
}

QString VpnManager::enhanceConfigForConnection(const QString& configContent, const VpnServer& server) {
    Q_UNUSED(server);

    QStringList lines = configContent.split('\n');
    QStringList enhancedLines;

    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(";") || trimmed.startsWith("#")) {
            enhancedLines.append(line); // Сохраняем комментарии с их оригинальным форматированием
            continue;
        }

        // Игнорируем настройки ping от сервера, но НЕ настройки сжатия
        if (trimmed.startsWith("ping ") || trimmed.startsWith("ping-restart ") ||
            trimmed.startsWith("keepalive ") || trimmed.startsWith("ping-timer-rem")) {
            enhancedLines.append(QString("# %1  # Игнорируем, устанавливаем свои").arg(trimmed));
            continue;
        }

        if (trimmed.startsWith("cipher ")) {
            QString cipher = trimmed.split(' ', Qt::SkipEmptyParts)[1];
            enhancedLines.append(QString("# %1  # Сохраняем оригинальную настройку").arg(trimmed));
            enhancedLines.append(QString("cipher %1").arg(cipher)); // Используем оригинальный шифр
        } else if (trimmed.startsWith("auth ")) {
            QString auth = trimmed.split(' ', Qt::SkipEmptyParts)[1];
            enhancedLines.append(QString("# %1  # Сохраняем оригинальную настройку").arg(trimmed));
            enhancedLines.append(QString("auth %1").arg(auth)); // Используем оригинальную аутентификацию
        } else if (trimmed.contains("fragment") || trimmed.contains("mssfix")) {
            // Убираем эти настройки, чтобы использовать наши собственные
            enhancedLines.append(QString("# %1  # Заменено нашими настройками").arg(trimmed));
        } else if (trimmed.startsWith("comp-lzo") || trimmed.contains("compress")) {
            // ВАЖНО: Не игнорируем настройки сжатия от сервера
            // Вместо этого, комментируем их и добавляем соответствующую настройку
            if (trimmed.contains("adaptive")) {
                enhancedLines.append(QString("# %1").arg(trimmed));
                enhancedLines.append("comp-lzo adaptive");  // Используем адаптивное сжатие
            } else if (trimmed.contains("yes") || trimmed.contains("lzo")) {
                enhancedLines.append(QString("# %1").arg(trimmed));
                enhancedLines.append("comp-lzo yes");  // Разрешаем сжатие
            } else if (trimmed.contains("no") || trimmed.contains("stub")) {
                enhancedLines.append(QString("# %1").arg(trimmed));
                enhancedLines.append("comp-lzo no");  // Используем stub compression
            } else {
                enhancedLines.append(trimmed); // Сохраняем оригинальную настройку
            }
        } else if (trimmed.startsWith("auth-user-pass")) {
            // Заменяем любые существующие настройки auth-user-pass, т.к. мы передаем их через stdin
            enhancedLines.append(QString("# %1  # Заменено нашей аутентификацией").arg(trimmed));
        } else {
            enhancedLines.append(line); // Сохраняем оригинальную строку с форматированием
        }
    }

    // Добавляем наши оптимизации
    enhancedLines.append("\n# Оптимизации для VPNGate");
    enhancedLines.append("remote-cert-tls server");
    enhancedLines.append("tls-client");
    enhancedLines.append("reneg-sec 0");
    enhancedLines.append("script-security 2");
    
    // Настройки аутентификации
    enhancedLines.append("auth-user-pass");  // Используем stdin для аутентификации
    
    // Повтор подключения
    enhancedLines.append("connect-retry 2");
    enhancedLines.append("connect-retry-max 5");
    enhancedLines.append(QString("connect-timeout %1").arg(connectionTimeout));

    // Блокируем только настройки ping, НЕ настройки сжатия
    enhancedLines.append("pull-filter ignore \"ping\"");
    enhancedLines.append("pull-filter ignore \"ping-restart\"");
    enhancedLines.append("pull-filter ignore \"keepalive\"");
    enhancedLines.append("pull-filter ignore \"explicit-exit-notify\"");

    // НЕ блокируем настройки сжатия:
    // enhancedLines.append("pull-filter ignore \"comp-lzo\"");
    // enhancedLines.append("pull-filter ignore \"compress\"");

    // Наши собственные настройки keepalive
    enhancedLines.append("keepalive 10 60");

    enhancedLines.append("tun-mtu 1500");
    enhancedLines.append("fragment 1300");  // Уменьшаем размер фрагмента для лучшей совместимости
    enhancedLines.append("mssfix 1200");    // Уменьшаем MSS для лучшей совместимости
    enhancedLines.append("persist-key");
    enhancedLines.append("persist-tun");
    enhancedLines.append("nobind");

    // Для лучшей стабильности
    enhancedLines.append("resolv-retry infinite");
    enhancedLines.append("mute-replay-warnings");

    // Дополнительные опции для стабильности
    enhancedLines.append("explicit-exit-notify 0");
    enhancedLines.append("fast-io");        // Улучшает производительность
    enhancedLines.append("sndbuf 393216");  // Увеличиваем буфер отправки
    enhancedLines.append("rcvbuf 393216");  // Увеличиваем буфер приема

    // Настройки логирования
    enhancedLines.append("verb 3");
    enhancedLines.append("mute 10");

    return enhancedLines.join('\n');
}

void VpnManager::cleanup() {
    // Отключаем все сигналы от process
    if (process) {
        QObject::disconnect(process, nullptr, this, nullptr);

        // Если процесс еще работает, завершаем его
        QProcess* currentProcess = process.data();
        if (currentProcess && currentProcess->state() == QProcess::Running) {
            currentProcess->kill();
            currentProcess->waitForFinished(100);
        }

        // QPointer автоматически управляет временем жизни
        // Просто очищаем указатель
        process.clear();
    }

    // Удаляем временный файл конфигурации
    if (!configPath.isEmpty() && QFile::exists(configPath)) {
        QTimer::singleShot(5000, [configPath = this->configPath]() {
            if (QFile::exists(configPath)) {
                QFile::remove(configPath);
            }
        });
        configPath.clear();
    }
}
