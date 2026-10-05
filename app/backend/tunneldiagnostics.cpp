#include "tunneldiagnostics.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryFile>
#include <algorithm>

namespace {
QMutex mutex;
QString logDirectory;
QStringList secrets;

void collect(const QJsonValue& value, bool sensitive = false)
{
    if (value.isObject()) {
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            const auto key = it.key().toLower();
            const bool hide = sensitive || QStringList{"id", "uuid", "user", "password", "pass", "privatekey",
                "publickey", "shortid", "shortids", "token", "email", "address", "servername", "host"}.contains(key);
            collect(it.value(), hide);
        }
    } else if (value.isArray()) {
        for (const auto& entry : value.toArray()) collect(entry, sensitive);
    } else if (sensitive && value.isString() && value.toString().size() >= 4 && secrets.size() < 4096) {
        if (!secrets.contains(value.toString())) secrets.append(value.toString());
    }
}

QString clean(QString text)
{
    for (const auto& secret : secrets) text.replace(secret, "[redacted]");
    text.replace(QDir::homePath(), "[home]", Qt::CaseInsensitive);
    text.replace(QDir::toNativeSeparators(QDir::homePath()), "[home]", Qt::CaseInsensitive);
    static const QRegularExpression uuid("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}");
    static const QRegularExpression ip("\\b(?:[0-9]{1,3}\\.){3}[0-9]{1,3}\\b");
    static const QRegularExpression key("(?i)((?:password|pass|token|privateKey|publicKey|shortId)\\s*[\"']?\\s*[=:]\\s*[\"']?)[^\\s,\"'}&]+");
    text.replace(uuid, "[uuid]");
    text.replace(ip, "[ip]");
    text.replace(key, "\\1[redacted]");
    return text.left(8192);
}

QString location()
{
    if (!logDirectory.isEmpty()) return logDirectory;
    QStringList candidates;
#ifdef Q_OS_WIN
    candidates << QCoreApplication::applicationDirPath() + "/logs";
#endif
    candidates << QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs";
    for (const auto& candidate : candidates) {
        if (!QDir().mkpath(candidate)) continue;
        QTemporaryFile probe(candidate + "/write-test-XXXXXX");
        if (probe.open()) { logDirectory = candidate; break; }
    }
    return logDirectory;
}
}

void TunnelDiagnostics::protectProfile(const QJsonObject& profile)
{
    QMutexLocker guard(&mutex);
    collect(profile);
    std::sort(secrets.begin(), secrets.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });
}

QString TunnelDiagnostics::sanitize(const QString& text)
{
    QMutexLocker guard(&mutex);
    return clean(text);
}

QString TunnelDiagnostics::directory()
{
    QMutexLocker guard(&mutex);
    return location();
}

void TunnelDiagnostics::write(const QString& stage, const QString& text)
{
    QMutexLocker guard(&mutex);
    const auto folder = location();
    if (folder.isEmpty()) return;
    const auto path = folder + "/tunnel-debug.jsonl";
    QFile file(path);
    if (file.size() > 2 * 1024 * 1024) {
        QFile::remove(path + ".2");
        QFile::rename(path + ".1", path + ".2");
        QFile::rename(path, path + ".1");
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    const QJsonObject entry{{"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                           {"pid", QCoreApplication::applicationPid()}, {"stage", stage}, {"message", clean(text)}};
    file.write(QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n');
    file.flush();
}

QString TunnelDiagnostics::exportReport(const QString& status, const QString& error)
{
    QMutexLocker guard(&mutex);
    const auto folder = location();
    if (folder.isEmpty()) return {};
    QByteArray report = "Moonlight Xray diagnostic report (profile and credentials excluded)\n";
    report += "UTC: " + QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toUtf8() + '\n';
    report += "OS: " + QSysInfo::prettyProductName().toUtf8() + '\n';
    report += "CPU: " + QSysInfo::currentCpuArchitecture().toUtf8() + '\n';
    report += "Moonlight: " + QCoreApplication::applicationVersion().toUtf8() + '\n';
    report += "Qt: " + QByteArray(qVersion()) + '\n';
    report += "Status: " + clean(status).toUtf8() + '\n';
    report += "Last error: " + clean(error).toUtf8() + "\n\nRecent events:\n";
    QFile events(folder + "/tunnel-debug.jsonl");
    if (events.open(QIODevice::ReadOnly)) {
        if (events.size() > 256 * 1024) {
            events.seek(events.size() - 256 * 1024);
            events.readLine();
        }
        report += events.readAll();
    }
    const auto path = folder + "/xray-diagnostic-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + ".txt";
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(report) != report.size() || !output.commit()) return {};
    return path;
}
