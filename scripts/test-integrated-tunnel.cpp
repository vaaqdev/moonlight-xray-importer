#include "backend/integratedtunnel.h"
#include <QCoreApplication>
#include <QTcpSocket>
#include <QDebug>
#include <QThread>
#include <Limelight.h>
#include <cstdio>
#include <QFile>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QStandardPaths>

static int testProfileImport()
{
    const QString installed = IntegratedTunnel::profilePath();
    QFile previous(installed);
    const bool existed = previous.exists();
    QByteArray original;
    if (existed) {
        if (!previous.open(QIODevice::ReadOnly)) return 20;
        original = previous.readAll();
        previous.close();
    }
    auto restore = qScopeGuard([&]() {
        if (existed) {
            QFile file(installed);
            if (file.open(QIODevice::WriteOnly)) file.write(original);
        }
        else QFile::remove(installed);
    });
    QTemporaryDir directory;
    const QString source = directory.path() + "/profile with spaces.json";
    auto write = [&](const QByteArray& bytes) {
        QFile file(source);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    auto readInstalled = [&]() {
        QFile file(installed);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    QString error;
    if (!write("{malformed") || IntegratedTunnel::importProfile(QUrl::fromLocalFile(source), &error)) return 21;
    if (readInstalled() != original) return 22;
    if (!write("{}") || IntegratedTunnel::importProfile(QUrl::fromLocalFile(source), &error)) return 23;
    if (!write("{\"outbounds\":[{\"protocol\":\"invalid-protocol\"}]}") ||
        IntegratedTunnel::importProfile(QUrl::fromLocalFile(source), &error)) return 24;
    if (readInstalled() != original) return 25;
    if (IntegratedTunnel::importProfile(QUrl("https://example.com/profile.json"), &error)) return 26;
    if (!write(QByteArray(1024 * 1024 + 1, 'x')) ||
        IntegratedTunnel::importProfile(QUrl::fromLocalFile(source), &error)) return 27;
    const QByteArray valid = "{\"outbounds\":[{\"tag\":\"test\",\"protocol\":\"freedom\"}],"
                             "\"routing\":{\"rules\":[{\"type\":\"field\",\"network\":\"tcp,udp\",\"outboundTag\":\"test\"}]}}";
    if (!write(valid) || !IntegratedTunnel::importProfile(QUrl::fromLocalFile(source), &error)) {
        fprintf(stderr, "VALID IMPORT FAILED: %s\n", error.toUtf8().constData());
        return 28;
    }
    if (!IntegratedTunnel::hasProfile() || IntegratedTunnel::active()) return 29;
    const auto saved = readInstalled();
    if (!saved.contains("freedom") || !saved.contains("routing")) return 30;
    if (!write("{bad") || IntegratedTunnel::importProfile(QUrl::fromLocalFile(source), &error) ||
        readInstalled() != saved) return 31;
    qInfo() << "PASS: valid import, paths with spaces, JSON/schema validation, size limit, routing preservation, atomic replacement";
    return 0;
}

static bool configured = false;
extern "C" void LiSetSocks5Proxy(bool enabled, const char* host, uint16_t port,
                                const char* user, const char* password)
{
    configured = enabled && QString::fromUtf8(host) == "127.0.0.1" && port != 0 &&
                 QString::fromUtf8(user).size() == 36 && QString::fromUtf8(password).size() == 36;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
#ifdef Q_OS_DARWIN
    QCoreApplication::setApplicationName("MoonlightImporterCITests");
    QStandardPaths::setTestModeEnabled(true);
#endif
    if (app.arguments().contains("--import-tests")) return testProfileImport();
    if (app.arguments().contains("--local-lifecycle-tests")) {
        QTemporaryDir directory;
        QFile file(directory.path() + "/local profile.json");
        if (!file.open(QIODevice::WriteOnly)) return 10;
        file.write("{\"outbounds\":[{\"protocol\":\"freedom\"}]}");
        file.close();
        QString error;
        if (!IntegratedTunnel::importProfile(QUrl::fromLocalFile(file.fileName()), &error)) {
            qCritical().noquote() << error;
            return 11;
        }
        auto cleanup = qScopeGuard([] { QFile::remove(IntegratedTunnel::profilePath()); });
        {
            IntegratedTunnel tunnel;
            if (!tunnel.start(&error)) { qCritical().noquote() << error; return 12; }
            if (!IntegratedTunnel::active()) return 13;
            IntegratedTunnel::configureStreaming();
            if (!configured) return 14;
            tunnel.stop();
            if (IntegratedTunnel::active()) return 15;
        }
        qInfo() << "PASS: native Qt importer, authenticated tunnel startup, streaming configuration and shutdown";
        return 0;
    }
    if (IntegratedTunnel::active()) return 1;
    {
        IntegratedTunnel tunnel;
        QString error;
        if (!tunnel.start(&error)) {
            fprintf(stderr, "START FAILED: %s\n", error.toUtf8().constData());
            qCritical().noquote() << error;
            return 1;
        }
        if (!IntegratedTunnel::active()) return 2;
        IntegratedTunnel::configureStreaming();
        if (!configured) return 3;
        if (app.arguments().contains("--hold")) {
            qInfo() << "MANAGER_READY";
            return app.exec();
        }
        QTcpSocket http;
        http.setProxy(IntegratedTunnel::proxy());
        http.connectToHost("example.com", 80);
        if (!http.waitForConnected(15000)) {
            qCritical() << "Qt SOCKS CONNECT failed:" << http.errorString();
            return 4;
        }
        http.write("GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n");
        if (http.bytesToWrite() && !http.waitForBytesWritten(3000)) return 5;
        if (!http.bytesAvailable() && !http.waitForReadyRead(15000)) return 6;
        if (!http.readAll().startsWith("HTTP/")) return 7;
        qInfo() << "PASS: managed Xray, random port/password, Qt HTTP through Reality, streaming configuration";
    }
    if (IntegratedTunnel::active()) return 8;
    qInfo() << "PASS: normal manager shutdown";
    return 0;
}
