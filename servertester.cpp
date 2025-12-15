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
#include <QDebug>

ServerTesterThread::ServerTesterThread(const QString& serverIp, const QString& serverName, QObject *parent)
: QThread(parent), serverIp(serverIp), serverName(serverName), process(nullptr), isCanceled(false) {
    qDebug() << "ServerTesterThread created for:" << serverName;
}

ServerTesterThread::~ServerTesterThread() {
    qDebug() << "ServerTesterThread destroying for:" << serverName;
    safeCleanup();

    // Корректно завершаем поток
    if (isRunning()) {
        quit();
        wait(1000);
    }
}

void ServerTesterThread::setOvpnConfig(const QString& configBase64) {
    QMutexLocker locker(&mutex);
    testOvpnConfig = configBase64;
}

void ServerTesterThread::run() {
    // Проверяем флаг отмены
    {
        QMutexLocker locker(&mutex);
        if (isCanceled) {
            emit realConnectionTestFinished(false, "Тест отменен");
            return;
        }
    }

    emit testProgress(QString("🔍 Начинаю тестирование сервера: %1").arg(serverName));

    // Гарантируем убийство старых процессов
    killAllOpenvpn();
    msleep(500);

    QString configCopy;
    {
        QMutexLocker locker(&mutex);
        if (testOvpnConfig.isEmpty()) {
            emit realConnectionTestFinished(false, "Нет конфигурации");
            return;
        }
        configCopy = testOvpnConfig;
    }

    int connectTime = 0;
    auto result = testRealOpenvpnConnection(connectTime);

    // Снова убиваем все процессы после теста
    killAllOpenvpn();

    emit realConnectionTestFinished(result.first, result.second);
}

void ServerTesterThread::cancel() {
    {
        QMutexLocker locker(&mutex);
        isCanceled = true;
    }

    killAllOpenvpn();
    safeCleanup();

    if (isRunning()) {
        quit();
        wait(500);
    }
}

bool ServerTesterThread::isProcessRunning() const {
    QMutexLocker locker(&mutex);
    return process && process->state() == QProcess::Running;
}

void ServerTesterThread::safeCleanup() {
    QMutexLocker locker(&mutex);

    if (process) {
        if (process->state() == QProcess::Running) {
            disconnect(process, nullptr, nullptr, nullptr);
            process->kill();
            process->waitForFinished(500);
        }
        delete process;
        process = nullptr;
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
    whichProcess.waitForFinished(1000);

    if (whichProcess.exitCode() == 0) {
        return QString::fromUtf8(whichProcess.readAllStandardOutput()).trimmed();
    }

    return "openvpn";
}

void ServerTesterThread::killAllOpenvpn() {
    // Убиваем все процессы OpenVPN, связанные с тестированием
    QProcess killProcess;

    #ifdef Q_OS_LINUX
    // Более безопасный способ - сначала пытаемся завершить корректно
    killProcess.start("pkill", QStringList() << "-SIGTERM" << "openvpn");
    killProcess.waitForFinished(300);

    killProcess.start("pkill", QStringList() << "-SIGTERM" << "-f" << "tun999");
    killProcess.waitForFinished(300);

    killProcess.start("pkill", QStringList() << "-SIGTERM" << "-f" << "vpngate");
    killProcess.waitForFinished(300);

    killProcess.start("pkill", QStringList() << "-SIGTERM" << "-f" << "test.ovpn");
    killProcess.waitForFinished(300);

    // Ждем немного
    msleep(200);

    // Если процессы еще живы, убиваем жестко
    killProcess.start("pkill", QStringList() << "-SIGKILL" << "openvpn");
    killProcess.waitForFinished(300);

    killProcess.start("pkill", QStringList() << "-SIGKILL" << "-f" << "tun999");
    killProcess.waitForFinished(300);

    killProcess.start("pkill", QStringList() << "-SIGKILL" << "-f" << "vpngate");
    killProcess.waitForFinished(300);

    killProcess.start("pkill", QStringList() << "-SIGKILL" << "-f" << "test.ovpn");
    killProcess.waitForFinished(300);
    #endif

    safeCleanup();
}

QPair<bool, QString> ServerTesterThread::testRealOpenvpnConnection(int& connectTime) {
    QElapsedTimer elapsedTimer;
    elapsedTimer.start();

    QTemporaryFile tempFile;
    QTemporaryFile authFile;

    try {
        QString configCopy;
        {
            QMutexLocker locker(&mutex);
            if (testOvpnConfig.isEmpty()) {
                return qMakePair(false, "Нет конфигурации");
            }
            configCopy = testOvpnConfig;
        }

        QByteArray configData = QByteArray::fromBase64(configCopy.toLatin1());
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

        QStringList cmd = {
            openvpnPath,
            "--config", tempFile.fileName(),
            "--auth-user-pass", authFile.fileName(),
            "--verb", "1",
            "--connect-timeout", "15",
            "--auth-retry", "nointeract",
            "--nobind",
            "--dev", "tun999",
            "--management", "127.0.0.1", "0"  // Отключаем management для чистоты
        };

        {
            QMutexLocker locker(&mutex);
            safeCleanup(); // Убедимся, что старый процесс удален

            process = new QProcess();
            process->setProcessChannelMode(QProcess::MergedChannels);
        }

        // Запускаем процесс
        process->start(cmd[0], cmd.mid(1));

        if (!process->waitForStarted(2000)) {
            QString error = QString("Не удалось запустить: %1").arg(process->errorString());
            safeCleanup();
            return qMakePair(false, error);
        }

        // Ждем завершения с таймаутом
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);

        // Используем локальную копию указателя для безопасности
        QProcess* localProcess = process;

        auto connection = connect(localProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                                  &loop, &QEventLoop::quit, Qt::QueuedConnection);

        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

        timer.start(15000); // Таймаут 15 секунд
        loop.exec();

        disconnect(connection);

        connectTime = elapsedTimer.elapsed();

        QString output;
        if (localProcess) {
            output = QString::fromUtf8(localProcess->readAll());
        }

        bool processWasRunning = false;
        {
            QMutexLocker locker(&mutex);
            processWasRunning = (process == localProcess);
        }

        if (!processWasRunning) {
            return qMakePair(false, "Процесс был прерван");
        }

        if (localProcess->exitStatus() == QProcess::NormalExit) {
            if (localProcess->exitCode() == 0) {
                // ОСНОВНОЕ ИСПРАВЛЕНИЕ: Проверяем, действительно ли установился туннель
                if (output.contains("Initialization Sequence Completed", Qt::CaseInsensitive)) {
                    return qMakePair(true, QString("Реальное подключение за %1ms").arg(connectTime));
                } else {
                    // OpenVPN завершился с кодом 0, но туннель не установился
                    return qMakePair(false, "Нет подтверждения подключения");
                }
            } else {
                if (output.contains("AUTH_FAILED", Qt::CaseInsensitive) ||
                    output.contains("TLS Error", Qt::CaseInsensitive) ||
                    output.contains("connection timeout", Qt::CaseInsensitive) ||
                    output.contains("connection refused", Qt::CaseInsensitive)) {
                    return qMakePair(false, "Ошибка подключения");
                    }
                    return qMakePair(false, QString("Ошибка (код: %1)").arg(localProcess->exitCode()));
            }
        } else {
            // Таймаут или ошибка
            if (localProcess->state() == QProcess::Running) {
                localProcess->terminate();
                if (!localProcess->waitForFinished(1000)) {
                    localProcess->kill();
                    localProcess->waitForFinished(500);
                }
            }

            // Проверяем вывод даже при таймауте
            if (output.contains("Initialization Sequence Completed", Qt::CaseInsensitive)) {
                return qMakePair(true, QString("Подключено (таймаут) за %1ms").arg(connectTime));
            }

            return qMakePair(false, QString("Таймаут (%1ms)").arg(connectTime));
        }

    } catch (const std::exception& e) {
        safeCleanup();
        return qMakePair(false, QString("Исключение: %1").arg(e.what()));
    }
}

QString ServerTesterThread::enhanceConfig(const QString& config) {
    QStringList lines = config.split('\n');
    QStringList enhancedLines;

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
        } else {
            enhancedLines.append(trimmed);
        }
    }

    // Добавляем минимальные настройки для тестирования
    enhancedLines.append("nobind");
    enhancedLines.append("persist-key");
    enhancedLines.append("persist-tun");
    enhancedLines.append("verb 1");
    enhancedLines.append("connect-timeout 15");
    enhancedLines.append("auth-retry nointeract");
    enhancedLines.append("auth-nocache");
    enhancedLines.append("script-security 2");
    enhancedLines.append("remote-cert-tls server");

    return enhancedLines.join('\n');
}
