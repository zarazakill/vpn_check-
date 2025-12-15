#include "servertester.h"
#include <QProcess>
#include <QTemporaryFile>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QDir>
#include <QFileInfo>
#include <QByteArray>
#include <QTextStream>
#include <QTimer>
#include <QEventLoop>
#include <QCoreApplication>

ServerTesterThread::ServerTesterThread(const QString& serverIp, const QString& serverName, QObject *parent)
: QThread(parent), serverIp(serverIp), serverName(serverName), process(nullptr), isCanceled(false) {
}

void ServerTesterThread::setOvpnConfig(const QString& configBase64) {
    testOvpnConfig = configBase64;
}

void ServerTesterThread::run() {
    // Проверяем флаг отмены
    if (isCanceled) {
        emit realConnectionTestFinished(false, "Тест отменен");
        return;
    }

    emit testProgress(QString("🔍 Начинаю тестирование сервера: %1").arg(serverName));

    // Гарантируем убийство старых процессов
    killAllOpenvpn();
    msleep(500);

    if (testOvpnConfig.isEmpty()) {
        emit realConnectionTestFinished(false, "Нет конфигурации");
        return;
    }

    int connectTime = 0;
    auto result = testRealOpenvpnConnection(connectTime);

    // Снова убиваем все процессы после теста
    killAllOpenvpn();

    emit realConnectionTestFinished(result.first, result.second);
}

void ServerTesterThread::cancel() {
    isCanceled = true;
    killAllOpenvpn();

    if (process && process->state() == QProcess::Running) {
        process->kill();
        process->waitForFinished(500);
    }

    if (isRunning()) {
        quit();
        wait(500);
    }
}

QString ServerTesterThread::findOpenvpn() {
    QStringList paths = {
        "/usr/sbin/openvpn",
        "/usr/bin/openvpn",
        "/sbin/openvpn",
        "/usr/local/sbin/openvpn",
        "openvpn"
    };

    for (const QString& path : paths) {
        QFileInfo file(path);
        if (file.exists() && file.isExecutable()) {
            return path;
        }
    }

    QProcess whichProcess;
    whichProcess.start("which", QStringList() << "openvpn");
    whichProcess.waitForFinished(1000); // Уменьшаем таймаут

    if (whichProcess.exitCode() == 0) {
        return QString::fromUtf8(whichProcess.readAllStandardOutput()).trimmed();
    }

    return "openvpn";
}

void ServerTesterThread::killAllOpenvpn() {
    // Убиваем все процессы OpenVPN, связанные с тестированием
    QProcess killProcess;

    #ifdef Q_OS_LINUX
    // Убиваем по имени процесса (используем killProcess синхронно)
    killProcess.start("pkill", QStringList() << "-9" << "openvpn");
    killProcess.waitForFinished(500);

    killProcess.start("pkill", QStringList() << "-9" << "-f" << "tun999");
    killProcess.waitForFinished(500);

    killProcess.start("pkill", QStringList() << "-9" << "-f" << "vpngate");
    killProcess.waitForFinished(500);

    killProcess.start("pkill", QStringList() << "-9" << "-f" << "test.ovpn");
    killProcess.waitForFinished(500);
    #endif

    // Также убиваем наш процесс, если он еще работает
    if (process) {
        if (process->state() == QProcess::Running) {
            process->kill();
            process->waitForFinished(500);
        }
        delete process;
        process = nullptr;
    }
}

QPair<bool, QString> ServerTesterThread::testRealOpenvpnConnection(int& connectTime) {
    QElapsedTimer elapsedTimer;
    elapsedTimer.start();

    QTemporaryFile tempFile;
    QTemporaryFile authFile;

    try {
        if (testOvpnConfig.isEmpty()) {
            return qMakePair(false, "Нет конфигурации");
        }

        QByteArray configData = QByteArray::fromBase64(testOvpnConfig.toLatin1());
        QString configContent = QString::fromUtf8(configData);

        if (!tempFile.open()) {
            return qMakePair(false, "Не удалось создать временный файл");
        }

        QString enhancedConfig = enhanceConfig(configContent);
        QTextStream stream(&tempFile);
        stream << enhancedConfig;
        tempFile.close();

        if (!authFile.open()) {
            return qMakePair(false, "Не удалось создать файл аутентификации");
        }

        QTextStream authStream(&authFile);
        authStream << "vpn\nvpn\n";
        authFile.close();

        QString openvpnPath = findOpenvpn();
        QFileInfo openvpnInfo(openvpnPath);
        if (!openvpnInfo.exists() || !openvpnInfo.isExecutable()) {
            return qMakePair(false, "OpenVPN не найден");
        }

        // Увеличиваем verb для лучшего логирования
        QStringList cmd = {
            openvpnPath,
            "--config", tempFile.fileName(),
            "--auth-user-pass", authFile.fileName(),
            "--verb", "1",  // Увеличили для отладки
            "--connect-timeout", "15",  // Увеличили таймаут
            "--auth-retry", "nointeract",
            "--nobind",
            "--dev", "tun999"
        };

        process = new QProcess();
        process->setProcessChannelMode(QProcess::SeparateChannels);  // Разделяем каналы

        // Сохраняем вывод для анализа
        QString allOutput;

        // Используем локальные указатели для захвата в лямбде
        QProcess* proc = process;

        connect(process, &QProcess::readyReadStandardOutput, this, [proc, &allOutput]() {
            allOutput += QString::fromUtf8(proc->readAllStandardOutput());
        });

        connect(process, &QProcess::readyReadStandardError, this, [proc, &allOutput]() {
            allOutput += QString::fromUtf8(proc->readAllStandardError());
        });

        // Запускаем процесс
        process->start(cmd[0], cmd.mid(1));

        if (!process->waitForStarted(2000)) {
            QString error = QString("Не удалось запустить: %1").arg(process->errorString());
            delete process;
            process = nullptr;
            return qMakePair(false, error);
        }

        // Ждем завершения с увеличенным таймаутом
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);

        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                &loop, &QEventLoop::quit);
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

        timer.start(15000); // Увеличили таймаут до 15 секунд
        loop.exec();

        connectTime = elapsedTimer.elapsed();

        // Получаем оставшийся вывод после завершения
        allOutput += QString::fromUtf8(proc->readAllStandardOutput());
        allOutput += QString::fromUtf8(proc->readAllStandardError());

        // Анализируем вывод OpenVPN
        if (process->exitStatus() == QProcess::NormalExit) {
            // Проверяем ключевые фразы в выводе
            QString debugMsg = QString("OpenVPN output for %1: %2").arg(serverName).arg(allOutput);
            qDebug() << debugMsg;

            if (process->exitCode() == 0) {
                // ОСНОВНОЕ ИСПРАВЛЕНИЕ: Проверяем, действительно ли установился туннель
                if (allOutput.contains("Initialization Sequence Completed", Qt::CaseInsensitive)) {
                    return qMakePair(true, QString("Реальное подключение за %1ms").arg(connectTime));
                } else {
                    // OpenVPN завершился с кодом 0, но туннель не установился
                    return qMakePair(false, "Ошибка: туннель не установлен");
                }
            } else {
                if (allOutput.contains("AUTH_FAILED", Qt::CaseInsensitive) ||
                    allOutput.contains("TLS Error", Qt::CaseInsensitive) ||
                    allOutput.contains("connection timeout", Qt::CaseInsensitive) ||
                    allOutput.contains("connection refused", Qt::CaseInsensitive) ||
                    allOutput.contains("No route to host", Qt::CaseInsensitive)) {
                    return qMakePair(false, "Ошибка подключения");
                    }
                    return qMakePair(false, QString("Ошибка (код: %1)").arg(process->exitCode()));
            }
        } else {
            // Таймаут или ошибка
            if (process->state() == QProcess::Running) {
                process->terminate();
                if (!process->waitForFinished(1000)) {
                    process->kill();
                    process->waitForFinished(500);
                }
            }

            // Получаем оставшийся вывод
            allOutput += QString::fromUtf8(proc->readAllStandardOutput());
            allOutput += QString::fromUtf8(proc->readAllStandardError());

            // Проверяем вывод даже при таймауте
            if (allOutput.contains("Initialization Sequence Completed", Qt::CaseInsensitive)) {
                return qMakePair(true, QString("Подключено (таймаут) за %1ms").arg(connectTime));
            }

            return qMakePair(false, QString("Таймаут (%1ms)").arg(connectTime));
        }

    } catch (const std::exception& e) {
        return qMakePair(false, QString("Исключение: %1").arg(e.what()));
    }
}

QString ServerTesterThread::enhanceConfig(const QString& config) {
    QStringList lines = config.split('\n');
    QStringList enhancedLines;

    bool hasRoute = false;
    bool hasRedirect = false;

    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }

        // Упрощаем конфиг
        if (trimmed.startsWith("cipher ")) {
            enhancedLines.append("cipher AES-256-CBC");
        } else if (trimmed.startsWith("auth ")) {
            enhancedLines.append("auth SHA256");
        } else if (trimmed.contains("fragment") || trimmed.contains("mssfix")) {
            // Пропускаем проблемные настройки
            continue;
        } else if (trimmed.startsWith("route ") || trimmed.startsWith("redirect-gateway")) {
            // Отмечаем наличие маршрутов
            if (trimmed.startsWith("redirect-gateway")) hasRedirect = true;
            if (trimmed.startsWith("route ")) hasRoute = true;
            enhancedLines.append(trimmed);
        } else {
            enhancedLines.append(trimmed);
        }
    }

    // Если нет маршрутизации, добавляем минимальную
    if (!hasRedirect && !hasRoute) {
        enhancedLines.append("route 8.8.8.8 255.255.255.255 net_gateway");
    }

    // Добавляем минимальные настройки для тестирования
    enhancedLines.append("nobind");
    enhancedLines.append("persist-key");
    enhancedLines.append("persist-tun");
    enhancedLines.append("verb 1");  // Увеличили для отладки
    enhancedLines.append("connect-timeout 15");  // Увеличили таймаут
    enhancedLines.append("auth-retry nointeract");
    enhancedLines.append("auth-nocache");
    enhancedLines.append("script-security 2");
    enhancedLines.append("remote-cert-tls server");

    // Отключаем необязательные вещи для ускорения
    enhancedLines.append("keepalive 5 30");
    enhancedLines.append("reneg-sec 0");

    return enhancedLines.join('\n');
}
