#pragma once

#include <QObject>
#include <QProcess>
#include <QTemporaryFile>
#include <QNetworkProxy>
#include <QUrl>
#include <QTimer>
#include <QThread>

// Owns a userspace Xray process. No TUN adapter or Windows proxy changes.
class IntegratedTunnel : public QObject
{
    Q_OBJECT
public:
    explicit IntegratedTunnel(QObject* parent = nullptr);
    ~IntegratedTunnel() override;
    bool start(QString* error);
    void startAsync();
    void requestReconnect();
    void retryNow();
    void setAutomaticRetryEnabled(bool enabled);
    bool starting() const { return m_Starting; }
    void stop();
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
    static IntegratedTunnel* instance();
    static QString status();
    static QString lastError();
    static QString connectionDiagnostics();

signals:
    void stateChanged();
    void connectionRecovered();

private:
    QProcess m_Process;
    QTemporaryFile m_Config;
    bool m_ConfigExists = false;
    bool m_Stopping = false;
    QString m_Status = QStringLiteral("Not started");
    QString m_LastError;
    QString m_LastOutput;
    QTimer m_RetryTimer;
    QTimer m_NetworkTimer;
    QThread* m_StartupWorker = nullptr;
    bool m_Starting = false;
    bool m_AutomaticRetry = true;
    bool m_RetryAllowed = true;
    bool m_RestartRequested = false;
    int m_RetryAttempt = 0;
    QByteArray m_NetworkSignature;
    void scheduleRetry();
    void updateState(const QString& status, const QString& error = QString());
#ifdef Q_OS_WIN
    void* m_Job = nullptr;
#endif
};
