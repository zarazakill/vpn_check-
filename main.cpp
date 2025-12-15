#include "mainwindow.h"
#include <QApplication>
#include <QStyleFactory>
#include <QDir>
#include <QMessageBox>
#include <QProcess>

bool checkDependencies() {
    // Проверяем наличие openvpn
    QProcess process;
    process.start("which", QStringList() << "openvpn");
    process.waitForFinished(3000);

    if (process.exitCode() != 0) {
        // Пробуем найти по разным путям
        QStringList paths = {
            "/usr/sbin/openvpn",
            "/usr/bin/openvpn",
            "/sbin/openvpn",
            "/usr/local/sbin/openvpn"
        };

        for (const QString& path : paths) {
            if (QFile::exists(path) && QFileInfo(path).isExecutable()) {
                return true;
            }
        }
        return false;
    }
    return true;
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // Настройка для Wayland/X11
    if (qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
        qputenv("QT_QPA_PLATFORM", "wayland");
    } else {
        qputenv("QT_QPA_PLATFORM", "xcb");
    }

    qputenv("QT_AUTO_SCREEN_SCALE_FACTOR", "0");
    qputenv("QT_SCALE_FACTOR", "1");

    // Проверка зависимостей
    QMessageBox::information(nullptr, "VPNGate Manager",
                             "🔍 Проверка зависимостей...");

    if (!checkDependencies()) {
        QMessageBox msgBox;
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setWindowTitle("Не найдены зависимости");
        msgBox.setText("❌ OpenVPN не найден в системе!");
        msgBox.setInformativeText("Установите OpenVPN для работы приложения:\n\nsudo apt install openvpn\n\nПродолжить без OpenVPN?");
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        msgBox.setDefaultButton(QMessageBox::No);

        if (msgBox.exec() == QMessageBox::No) {
            return 1;
        }
    }

    app.setStyle(QStyleFactory::create("Fusion"));

    MainWindow window;
    window.show();

    return app.exec();
}
