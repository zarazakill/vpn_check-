#include "servertester.h"
#include <QTemporaryFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QByteArray>
#include <QFile>
#include <QTimer>
#include <QElapsedTimer>
#include <QProcess>

ServerTesterThread::ServerTesterThread(const QString& serverIp, const QString& serverName, QObject *parent)
: QThread(parent), serverIp(serverIp), serverName(serverName), cancelled(false) {
}

void ServerTesterThread::setOvpnConfig(const QString& configBase64) {
    ovpnConfigBase64 = configBase64;
}

void ServerTesterThread::cancel() {
    cancelled = true;
}

void ServerTesterThread::run() {
    emit testProgress(QString("Начинаю проверку сервера %1...").arg(serverName));

    // Сначала проверяем пинг
    if (cancelled) {
        emit realConnectionTestFinished(false, "Проверка отменена");
        return;
    }

    emit testProgress("Проверяю доступность сервера (ping)...");
    bool pingSuccess = testPing();

    if (!pingSuccess) {
        emit realConnectionTestFinished(false, "Сервер недоступен (нет ping)");
        return;
    }

    // Если есть конфигурация, проверяем реальное подключение
    if (!ovpnConfigBase64.isEmpty()) {
        if (cancelled) {
            emit realConnectionTestFinished(false, "Проверка отменена");
            return;
        }

        emit testProgress("Проверяю реальное VPN подключение...");
        bool connectionSuccess = testRealConnection();

        if (connectionSuccess) {
            emit realConnectionTestFinished(true, QString("Успешное подключение"));
        } else {
            emit realConnectionTestFinished(false, "Не удалось установить VPN подключение");
        }
    } else {
        emit realConnectionTestFinished(true, "Сервер доступен (ping успешен)");
    }
}

bool ServerTesterThread::testPing() {
    QProcess pingProcess;
    QStringList args;

    #ifdef Q_OS_WINDOWS
    args << "-n" << "2" << "-w" << "3000" << serverIp;
    #else
    args << "-c" << "2" << "-W" << "3" << serverIp;
    #endif

    pingProcess.start("ping", args);

    if (!pingProcess.waitForStarted(3000)) {
        emit testProgress("❌ Ошибка запуска ping");
        return false;
    }

    if (!pingProcess.waitForFinished(5000)) {
        pingProcess.kill();
        emit testProgress("⏰ Таймаут ping");
        return false;
    }

    QString output = QString::fromLocal8Bit(pingProcess.readAllStandardOutput());
    int exitCode = pingProcess.exitCode();

    if (exitCode == 0) {
        // Парсим время пинга
        QRegularExpression re("time[=<](\\d+\\.?\\d*)");
        QRegularExpressionMatch match = re.match(output);

        if (match.hasMatch()) {
            float pingTime = match.captured(1).toFloat();
            emit testProgress(QString("✅ Ping успешен: %1 ms").arg(pingTime));
            return true;
        } else {
            emit testProgress("✅ Ping успешен (время не получено)");
            return true;
        }
    } else {
        emit testProgress(QString("❌ Ping неуспешен (код: %1)").arg(exitCode));
        return false;
    }
}

bool ServerTesterThread::testRealConnection() {
    if (ovpnConfigBase64.isEmpty()) {
        emit testProgress("❌ Нет конфигурации OpenVPN для тестирования");
        return false;
    }

    QTemporaryFile tempFile;
    if (!tempFile.open()) {
        emit testProgress("❌ Не удалось создать временный файл");
        return false;
    }

    try {
        QByteArray configData = QByteArray::fromBase64(ovpnConfigBase64.toLatin1());
        QString configContent = QString::fromUtf8(configData);

        QString enhancedConfig = enhanceConfigForTest(configContent);

        QTextStream stream(&tempFile);
        stream << enhancedConfig;
        tempFile.close();

        emit testProgress("📄 Создан временный конфиг OpenVPN");

        QStringList args = {
            "openvpn",
            "--config", tempFile.fileName(),
            "--auth-user-pass", "/dev/stdin",
            "--verb", "0",
            "--connect-timeout", "10",
            "--ping", "2",
            "--ping-exit", "5"
        };

        QProcess openvpnProcess;
        openvpnProcess.setProcessChannelMode(QProcess::MergedChannels);

        QElapsedTimer timer;
        timer.start();

        openvpnProcess.start(args[0], args.mid(1));

        if (!openvpnProcess.waitForStarted(3000)) {
            emit testProgress(QString("❌ Не удалось запустить OpenVPN: %1").arg(openvpnProcess.errorString()));
            return false;
        }

        // Отправляем стандартные учетные данные VPNGate
        openvpnProcess.write("vpn\nvpn\n");
        openvpnProcess.closeWriteChannel();

        bool connected = false;
        QString output;

        // Ждем не более 15 секунд
        while (timer.elapsed() < 15000) {
            if (!openvpnProcess.waitForReadyRead(100)) {
                continue;
            }

            output += QString::fromUtf8(openvpnProcess.readAll());

            if (output.contains("Initialization Sequence Completed")) {
                connected = true;
                int connectionTime = timer.elapsed();
                emit testProgress(QString("✅ VPN подключение успешно установлено за %1 ms").arg(connectionTime));
                break;
            }

            if (output.contains("AUTH_FAILED") ||
                output.contains("TLS Error") ||
                output.contains("connection failed")) {
                emit testProgress("❌ Ошибка аутентификации/подключения");
            break;
                }

                QThread::msleep(100);
        }

        if (openvpnProcess.state() == QProcess::Running) {
            openvpnProcess.terminate();
            if (!openvpnProcess.waitForFinished(2000)) {
                openvpnProcess.kill();
            }
        }

        return connected;

    } catch (const std::exception& e) {
        emit testProgress(QString("❌ Исключение: %1").arg(e.what()));
        return false;
    }
}

QString ServerTesterThread::enhanceConfigForTest(const QString& configContent) {
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
            enhancedLines.append(QString("# %1  # Отключено для теста").arg(trimmed));
        } else {
            enhancedLines.append(trimmed);
        }
    }

    // Добавляем необходимые опции для быстрого тестирования
    enhancedLines.append("\n# Оптимизации для быстрого теста");
    enhancedLines.append("remote-cert-tls server");
    enhancedLines.append("tls-client");
    enhancedLines.append("reneg-sec 0");
    enhancedLines.append("script-security 2");
    enhancedLines.append("auth-retry interact");
    enhancedLines.append("auth-nocache");
    enhancedLines.append("connect-retry 1");
    enhancedLines.append("connect-timeout 10");

    return enhancedLines.join('\n');
}

void ServerTesterThread::cleanup() {
    // Очистка ресурсов при необходимости
}
