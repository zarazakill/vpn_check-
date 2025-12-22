#ifndef VPNTYPES_H
#define VPNTYPES_H

#include <QString>
#include <QList>
#include <QMetaType>

struct VpnServer {
    QString name;
    QString filename;
    QString configBase64;
    QString country;
    QString ip;
    int port;
    QString protocol;
    int score;
    int ping;
    double speedMbps;
    QString sessions;
    QString uptime;
    bool tested;
    bool available;
    int testPing;
    bool realConnectionTested;
    QString username;
    QString password;

    VpnServer()
    : port(1194), score(0), ping(999), speedMbps(0.0),
    tested(false), available(false), testPing(999),
    realConnectionTested(false) {
        // Устанавливаем стандартные учетные данные для VPNGate
        username = "vpn";
        password = "vpn";
    }

    // Добавляем операторы сравнения
    bool operator==(const VpnServer& other) const {
        return name == other.name &&
        ip == other.ip &&
        port == other.port &&
        country == other.country;
    }

    bool operator!=(const VpnServer& other) const {
        return !(*this == other);
    }
};

// Регистрируем тип для использования в сигналах/слотах
Q_DECLARE_METATYPE(VpnServer)

#endif // VPNTYPES_H
