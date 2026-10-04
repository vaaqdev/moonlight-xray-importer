#pragma once

#include <QObject>
#include <QProcess>
#include <QTemporaryFile>
#include <QNetworkProxy>
#include <QUrl>

// Owns a userspace Xray process. No TUN adapter or Windows proxy changes.
class IntegratedTunnel : public QObject
{
public:
    explicit IntegratedTunnel(QObject* parent = nullptr);
    ~IntegratedTunnel() override;
    bool start(QString* error);
    static bool importProfile(const QUrl& source, QString* error);
    static bool hasProfile();
    static QString profilePath();
    static QString coreDirectory();
    static bool hasBundledCore();
    static bool required();
    static void setRequired(bool required);
    static bool active();
    static QNetworkProxy proxy();
    static void configureStreaming();

private:
    QProcess m_Process;
    QTemporaryFile m_Config;
    bool m_Stopping = false;
#ifdef Q_OS_WIN
    void* m_Job = nullptr;
#endif
};
