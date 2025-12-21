#include "mainwindow.h"
#include <QApplication>
#include <QStyleFactory>
#include <QDir>
#include <QMessageBox>
#include <QProcess>
#include <QDebug>
#include <QDateTime>
#include <QStandardPaths>
#include <QPushButton> // ДОБАВЬТЕ ЭТОТ ЗАГОЛОВОЧНЫЙ ФАЙЛ

bool checkDependencies() {
    qDebug() << "Проверка зависимостей...";

    // Проверяем наличие openvpn
    QProcess process;

    // Пробуем разные способы найти openvpn
    QStringList possiblePaths = {
        "openvpn",
        "/usr/sbin/openvpn",
        "/usr/bin/openvpn",
        "/sbin/openvpn",
        "/usr/local/sbin/openvpn",
        "/usr/local/bin/openvpn"
    };

    bool openvpnFound = false;
    QString openvpnPath;

    for (const QString& path : possiblePaths) {
        QProcess testProcess;
        testProcess.start(path, QStringList() << "--version");
        testProcess.waitForFinished(1000);

        if (testProcess.exitCode() == 0) {
            openvpnFound = true;
            openvpnPath = path;
            qDebug() << "✅ Найден OpenVPN по пути:" << path;
            break;
        }
    }

    // Пробуем через which
    if (!openvpnFound) {
        QProcess whichProcess;
        whichProcess.start("which", QStringList() << "openvpn");
        whichProcess.waitForFinished(1000);

        if (whichProcess.exitCode() == 0) {
            openvpnPath = QString::fromUtf8(whichProcess.readAllStandardOutput()).trimmed();
            if (!openvpnPath.isEmpty() && QFile::exists(openvpnPath)) {
                openvpnFound = true;
                qDebug() << "✅ Найден OpenVPN через which:" << openvpnPath;
            }
        }
    }

    // Пробуем через whereis
    if (!openvpnFound) {
        QProcess whereisProcess;
        whereisProcess.start("whereis", QStringList() << "-b" << "openvpn");
        whereisProcess.waitForFinished(1000);

        QString output = QString::fromUtf8(whereisProcess.readAllStandardOutput());
        if (output.contains("openvpn:")) {
            QStringList parts = output.split(':');
            if (parts.size() > 1) {
                QStringList bins = parts[1].trimmed().split(' ');
                for (const QString& bin : bins) {
                    if (QFile::exists(bin)) {
                        openvpnFound = true;
                        openvpnPath = bin;
                        qDebug() << "✅ Найден OpenVPN через whereis:" << bin;
                        break;
                    }
                }
            }
        }
    }

    if (!openvpnFound) {
        // Проверяем, установлен ли openvpn через пакетный менеджер
        #ifdef Q_OS_LINUX
        QProcess dpkgProcess;
        dpkgProcess.start("dpkg", QStringList() << "-l" << "openvpn");
        dpkgProcess.waitForFinished(1000);

        if (dpkgProcess.exitCode() == 0) {
            QString dpkgOutput = QString::fromUtf8(dpkgProcess.readAllStandardOutput());
            if (dpkgOutput.contains("ii") && dpkgOutput.contains("openvpn")) {
                qDebug() << "⚠️ OpenVPN установлен через dpkg, но не найден в PATH";
            }
        } else {
            QProcess rpmProcess;
            rpmProcess.start("rpm", QStringList() << "-qa" << "openvpn");
            rpmProcess.waitForFinished(1000);

            if (rpmProcess.exitCode() == 0) {
                qDebug() << "⚠️ OpenVPN установлен через rpm, но не найден в PATH";
            }
        }
        #endif

        // Показываем диалог с опциями
        QMessageBox msgBox;
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setWindowTitle("OpenVPN не найден");
        msgBox.setText("❌ OpenVPN не найден в системе!");
        msgBox.setInformativeText(
            "Для работы приложения необходим OpenVPN.\n\n"
            "Выберите действие:\n"
            "• Установить OpenVPN (требует sudo пароль)\n"
            "• Продолжить без OpenVPN (не рекомендуется)\n"
            "• Выйти из приложения"
        );

        QPushButton *installButton = msgBox.addButton("Установить", QMessageBox::ActionRole);
        QPushButton *continueButton = msgBox.addButton("Продолжить", QMessageBox::AcceptRole);
        QPushButton *exitButton = msgBox.addButton("Выйти", QMessageBox::RejectRole);
        msgBox.setDefaultButton(exitButton);

        msgBox.exec();

        QAbstractButton *clickedButton = msgBox.clickedButton(); // ИСПРАВЛЕНИЕ 1

        if (clickedButton == installButton) { // ИСПРАВЛЕНИЕ 2
            // Пытаемся установить OpenVPN
            QProcess installProcess;
            installProcess.start("pkexec", QStringList() << "apt" << "install" << "-y" << "openvpn");

            QMessageBox progressBox;
            progressBox.setWindowTitle("Установка OpenVPN");
            progressBox.setText("Идет установка OpenVPN...\n\nПожалуйста, подождите.");
            progressBox.setStandardButtons(QMessageBox::NoButton); // ИСПРАВЛЕНИЕ 3: используем константу
            progressBox.show();

            if (!installProcess.waitForStarted(5000)) {
                // Пробуем через sudo
                installProcess.start("sudo", QStringList() << "apt" << "install" << "-y" << "openvpn");
                installProcess.waitForStarted(5000);
            }

            installProcess.waitForFinished(60000); // Ждем до 60 секунд

            progressBox.close();

            if (installProcess.exitCode() == 0) {
                qDebug() << "✅ OpenVPN успешно установлен";
                QMessageBox::information(nullptr, "Успех",
                                         "OpenVPN успешно установлен!\n\nПриложение будет перезапущено.");

                // Перезапускаем приложение
                QProcess::startDetached(qApp->arguments()[0], qApp->arguments());
                qApp->quit();
                return true;
            } else {
                qDebug() << "❌ Ошибка установки OpenVPN";
                QMessageBox::critical(nullptr, "Ошибка установки",
                                      "Не удалось установить OpenVPN.\n\n"
                                      "Попробуйте установить вручную:\n"
                                      "sudo apt update && sudo apt install openvpn");
                return false;
            }
        } else if (clickedButton == continueButton) { // ИСПРАВЛЕНИЕ 4
            qDebug() << "⚠️ Продолжаем без OpenVPN (не рекомендуется)";
            return true;
        } else {
            return false; // Выход
        }
    }

    return true;
}

int main(int argc, char *argv[]) {
    // Включаем логирование для отладки
    qSetMessagePattern("[%{time yyyy-MM-dd hh:mm:ss}] %{type}: %{message}");

    QApplication app(argc, argv);

    // Настройка для Wayland/X11
    if (qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
        qputenv("QT_QPA_PLATFORM", "wayland");
    } else {
        qputenv("QT_QPA_PLATFORM", "xcb");
    }

    qputenv("QT_AUTO_SCREEN_SCALE_FACTOR", "0");
    qputenv("QT_SCALE_FACTOR", "1");

    // Устанавливаем имя приложения для настроек
    app.setApplicationName("VPNGate Manager");
    app.setOrganizationName("VPNGate");
    app.setApplicationVersion("1.0.0");

    qDebug() << "Запуск VPNGate Manager..." << QDateTime::currentDateTime().toString();
    qDebug() << "Версия Qt:" << qVersion();
    qDebug() << "Путь к данным:" << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    // Проверка зависимостей
    if (!checkDependencies()) {
        return 1;
    }

    // Устанавливаем стиль приложения
    QStringList styles = QStyleFactory::keys();
    qDebug() << "Доступные стили:" << styles;

    QString preferredStyle;
    if (styles.contains("Fusion")) {
        preferredStyle = "Fusion";
    } else if (!styles.isEmpty()) {
        preferredStyle = styles.first();
    }

    if (!preferredStyle.isEmpty()) {
        QApplication::setStyle(QStyleFactory::create(preferredStyle));
        qDebug() << "Установлен стиль:" << preferredStyle;
    }

    // Создаем директорию для логов если нужно
    QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(logDir);
    if (!dir.exists()) {
        dir.mkpath(".");
        qDebug() << "Создана директория для данных:" << logDir;
    }

    try {
        qDebug() << "Создание главного окна...";
        MainWindow window;

        // Устанавливаем иконку приложения (если есть)
        // window.setWindowIcon(QIcon(":/icons/vpn-icon.png"));

        qDebug() << "Показ главного окна...";
        window.show();

        qDebug() << "Запуск основного цикла приложения...";
        return app.exec();
    } catch (const std::exception& e) {
        qCritical() << "Ошибка при запуске приложения:" << e.what();
        QMessageBox::critical(nullptr, "Фатальная ошибка",
                              QString("Не удалось запустить приложение:\n%1\n\n"
                              "Попробуйте перезапустить приложение или "
                              "проверьте наличие всех зависимостей.").arg(e.what()));
        return 1;
    } catch (...) {
        qCritical() << "Неизвестная ошибка при запуске приложения";
        QMessageBox::critical(nullptr, "Фатальная ошибка",
                              "Неизвестная ошибка при запуске приложения.\n"
                              "Попробуйте перезапустить приложение.");
        return 1;
    }
}
