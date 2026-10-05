#include "integratedtunnel.h"
#include "tunneldiagnostics.h"
#include <Limelight.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QElapsedTimer>
#include <QUuid>
#include <QThread>
#include <QDebug>
#include <QNetworkProxy>
#include <QStandardPaths>
#include <QSysInfo>
#include <QScopeGuard>
#include <QReadWriteLock>
#include <QDateTime>
#include <QNetworkInformation>
#include <QNetworkInterface>
#include <QCryptographicHash>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
quint16 tunnelPort = 0;
QString tunnelUser;
QString tunnelPassword;
bool tunnelRequired = false;
QReadWriteLock tunnelLock;
IntegratedTunnel* tunnelInstance = nullptr;
constexpr int ProcessStartTimeoutMs = 10000;
constexpr int ReadyTimeoutMs = 20000;
constexpr int ValidationTimeoutMs = 30000;

QByteArray networkSignature()
{
    QStringList entries;
    for (const auto& networkInterface : QNetworkInterface::allInterfaces()) {
        if (!(networkInterface.flags() & QNetworkInterface::IsUp) || (networkInterface.flags() & QNetworkInterface::IsLoopBack)) continue;
        for (const auto& entry : networkInterface.addressEntries()) entries << networkInterface.name() + ':' + entry.ip().toString();
    }
    entries.sort();
    return QCryptographicHash::hash(entries.join('|').toUtf8(), QCryptographicHash::Sha256);
}

QString corePath()
{
#ifdef Q_OS_WIN
    return IntegratedTunnel::coreDirectory() + "/xray.exe";
#else
    const QString dir = IntegratedTunnel::coreDirectory();
#ifdef Q_OS_MACOS
    // Universal DMGs ship both CPU variants; run the one matching this process
    // (QSysInfo reports x86_64 when running under Rosetta).
    if (QSysInfo::currentCpuArchitecture() == QLatin1String("x86_64") &&
        QFile::exists(dir + "/xray-x86_64")) {
        return dir + "/xray-x86_64";
    }
#endif
    return dir + "/xray";
#endif
}

bool parseProfile(const QByteArray& bytes, QJsonObject* config, QString* error)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        document.object().value("outbounds").toArray().isEmpty()) {
        *error = QObject::tr("Invalid Xray profile. Import a JSON configuration with an outbounds array.");
        return false;
    }
    *config = document.object();
    TunnelDiagnostics::protectProfile(*config);
    return true;
}

QJsonObject runtimeConfig(QJsonObject config, quint16 port, const QString& user, const QString& password)
{
    const QJsonObject settings{{"auth", "password"}, {"udp", true}, {"ip", "127.0.0.1"},
                              {"accounts", QJsonArray{QJsonObject{{"user", user}, {"pass", password}}}}};
    config["inbounds"] = QJsonArray{QJsonObject{{"tag", "moonlight"}, {"listen", "127.0.0.1"},
                                               {"port", port}, {"protocol", "socks"}, {"settings", settings}}};
    config["log"] = QJsonObject{{"loglevel", "info"}};
    return config;
}
}

IntegratedTunnel::IntegratedTunnel(QObject* parent) : QObject(parent)
{
    tunnelInstance = this;
    TunnelDiagnostics::write("session", QString("Moonlight=%1 Qt=%2 OS=%3 CPU=%4 UTC=%5")
        .arg(QCoreApplication::applicationVersion(), qVersion(), QSysInfo::prettyProductName(),
             QSysInfo::currentCpuArchitecture(), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)));
    TunnelDiagnostics::write("assets", QString("core=%1 geoip=%2 geosite=%3 tempWritable=%4")
        .arg(hasBundledCore()).arg(QFile::exists(coreDirectory() + "/geoip.dat"))
        .arg(QFile::exists(coreDirectory() + "/geosite.dat")).arg(QFileInfo(QDir::tempPath()).isWritable()));
    m_Process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_Process, &QProcess::readyReadStandardOutput, this, [this]() {
        const auto output = TunnelDiagnostics::sanitize(QString::fromUtf8(m_Process.readAllStandardOutput()).trimmed());
        if (!output.isEmpty()) {
            m_LastOutput = (m_LastOutput + '\n' + output).right(4096);
            TunnelDiagnostics::write("xray.output", output);
            qInfo().noquote() << "Xray:" << output;
        }
    }, Qt::DirectConnection);
    connect(&m_Process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::Timedout) return; // Readiness polling is bounded separately.
        TunnelDiagnostics::write("process.error", QString("Qt error=%1: %2").arg(int(error)).arg(m_Process.errorString()));
    }, Qt::DirectConnection);
    connect(&m_Process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        if (m_Starting) return;
        if (!m_Stopping && active()) {
            TunnelDiagnostics::write("process.unexpected_exit", QString("exitCode=%1 recentOutput=%2").arg(code).arg(m_LastOutput));
            qCritical() << "Integrated tunnel stopped unexpectedly, exit code:" << code;
            stop();
            QNetworkProxy::setApplicationProxy(proxy());
            updateState(tr("Xray stopped; waiting to reconnect"), tr("Xray exited unexpectedly (code %1).").arg(code));
            scheduleRetry();
        }
    });
    m_RetryTimer.setSingleShot(true);
    connect(&m_RetryTimer, &QTimer::timeout, this, [this] {
        if (!required() || !m_AutomaticRetry || m_Starting) return;
        startAsync();
    });
    m_NetworkSignature = networkSignature();
    m_NetworkTimer.setInterval(5000);
    connect(&m_NetworkTimer, &QTimer::timeout, this, [this] {
        const auto current = networkSignature();
        if (current != m_NetworkSignature) {
            m_NetworkSignature = current;
            TunnelDiagnostics::write("network.changed", "Local interfaces or addresses changed (addresses are not logged)");
            requestReconnect();
        }
    });
    QNetworkInformation::loadDefaultBackend();
    if (auto information = QNetworkInformation::instance()) {
        connect(information, &QNetworkInformation::reachabilityChanged, this, [this](QNetworkInformation::Reachability reachability) {
            TunnelDiagnostics::write("network.reachability", QString("OS reachability=%1").arg(int(reachability)));
            if (reachability != QNetworkInformation::Reachability::Disconnected) requestReconnect();
            else updateState(tr("Network disconnected; waiting for Wi-Fi..."));
        });
        connect(information, &QNetworkInformation::transportMediumChanged, this, [this](QNetworkInformation::TransportMedium) { requestReconnect(); });
    }
}

IntegratedTunnel::~IntegratedTunnel()
{
    stop();
    if (tunnelInstance == this) tunnelInstance = nullptr;
}

void IntegratedTunnel::stop()
{
    if (QThread::currentThread() == thread()) {
        m_RetryTimer.stop();
        m_NetworkTimer.stop();
        if (m_StartupWorker && m_StartupWorker->isRunning()) m_StartupWorker->wait();
    }
    m_Stopping = true;
    if (m_Process.state() != QProcess::NotRunning) {
#ifdef Q_OS_WIN
        // CREATE_NO_WINDOW cores do not handle WM_CLOSE from terminate().
        // Kill the read-only core directly instead of freezing the UI for seconds.
        m_Process.kill();
        m_Process.waitForFinished(1500);
#else
        m_Process.terminate();
        if (!m_Process.waitForFinished(4000)) {
            TunnelDiagnostics::write("process.shutdown", "Graceful shutdown timed out; terminating core");
            m_Process.kill();
            m_Process.waitForFinished(1500);
        }
#endif
    }
#ifdef Q_OS_WIN
    if (m_Job) {
        CloseHandle(static_cast<HANDLE>(m_Job));
        m_Job = nullptr;
    }
#endif
    if (m_ConfigExists) {
        m_Config.remove();
        m_ConfigExists = false;
        m_Config.setAutoRemove(false);
    }
    QWriteLocker guard(&tunnelLock);
    tunnelPort = 0;
    tunnelUser.clear();
    tunnelPassword.clear();
}

bool IntegratedTunnel::start(QString* error)
{
    error->clear();
    if (active()) return true;
    stop();
    m_Stopping = false;
    m_LastOutput.clear();
    setRequired(true);
    updateState(tr("Starting Xray..."));
    m_RetryAllowed = true;
    QElapsedTimer total;
    total.start();
    bool succeeded = false;
    auto cleanup = qScopeGuard([&] {
        if (!succeeded) {
            *error = TunnelDiagnostics::sanitize(*error);
            updateState(tr("Xray startup failed"), *error);
            TunnelDiagnostics::write("startup.failed", QString("elapsedMs=%1 error=%2 recentOutput=%3")
                .arg(total.elapsed()).arg(*error, m_LastOutput));
            stop();
            emit stateChanged();
        }
    });
    TunnelDiagnostics::write("startup.begin", QString("core=%1 processTimeoutMs=%2 readyTimeoutMs=%3")
        .arg(corePath()).arg(ProcessStartTimeoutMs).arg(ReadyTimeoutMs));
    if (!hasBundledCore()) {
        *error = tr("Bundled Xray is missing. Extract the complete ZIP, including the Xray folder.");
        m_RetryAllowed = false;
        return false;
    }
    const QDir coreDir(IntegratedTunnel::coreDirectory());
    QFile profile(IntegratedTunnel::profilePath());
    if (!profile.open(QIODevice::ReadOnly)) {
        *error = tr("Cannot open the imported Xray profile: %1").arg(profile.errorString());
        m_RetryAllowed = false;
        return false;
    }
    QJsonObject config;
    if (!parseProfile(profile.readAll(), &config, error)) { m_RetryAllowed = false; return false; }
    TunnelDiagnostics::write("startup.profile", "Imported JSON parsed successfully");
    // Reserve an unused loopback port, then let Xray bind it. Readiness includes
    // password authentication, so another process cannot be mistaken for our core.
    QTcpServer reservation;
    if (!reservation.listen(QHostAddress::LocalHost, 0)) {
        *error = "Cannot allocate a local tunnel port: " + reservation.errorString();
        return false;
    }
    const auto port = reservation.serverPort();
    const auto user = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto password = QUuid::createUuid().toString(QUuid::WithoutBraces);
    config = runtimeConfig(config, port, user, password);
    m_Config.setFileTemplate(QDir::tempPath() + "/Moonlight-Xray-XXXXXX.json");
    m_Config.setAutoRemove(true);
    if (!m_Config.open()) {
        *error = "Cannot create the temporary tunnel configuration.";
        return false;
    }
    m_ConfigExists = true;
    m_Config.resize(0);
    const auto bytes = QJsonDocument(config).toJson();
    if (m_Config.write(bytes) != bytes.size() || !m_Config.flush()) {
        *error = "Cannot write the temporary tunnel configuration.";
        return false;
    }
    m_Config.close();
#ifdef Q_OS_WIN
    m_Job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!m_Job || !SetInformationJobObject(static_cast<HANDLE>(m_Job), JobObjectExtendedLimitInformation,
                                         &limits, sizeof(limits))) {
        *error = tr("Cannot create the Xray process lifetime job (Windows error %1).").arg(GetLastError());
        return false;
    }
    m_Process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    const auto program = corePath();
    m_Process.setWorkingDirectory(coreDir.absolutePath());
    reservation.close();
#ifdef Q_OS_DARWIN
    // macOS has no Windows Job Objects. The bundled native launcher watches
    // Moonlight's PID and terminates Xray even when the application crashes.
    const QString launcher = coreDir.filePath("xray-launcher");
    if (QFile::exists(launcher)) {
        m_Process.start(launcher, {QString::number(QCoreApplication::applicationPid()), program,
                                   "run", "-c", m_Config.fileName()});
    }
    else {
        m_Process.start(program, {"run", "-c", m_Config.fileName()});
    }
#else
    m_Process.start(program, {"run", "-c", m_Config.fileName()});
#endif
    if (!m_Process.waitForStarted(ProcessStartTimeoutMs)) {
        *error = "Cannot start bundled Xray: " + m_Process.errorString();
        return false;
    }
    TunnelDiagnostics::write("process.started", QString("pid=%1 elapsedMs=%2").arg(m_Process.processId()).arg(total.elapsed()));
#ifdef Q_OS_WIN
    HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                               static_cast<DWORD>(m_Process.processId()));
    const bool assigned = child && AssignProcessToJobObject(static_cast<HANDLE>(m_Job), child);
    const DWORD assignmentError = assigned ? 0 : GetLastError();
    if (child) CloseHandle(child);
    if (!assigned) {
        *error = tr("Cannot attach Xray to Moonlight's process lifetime job (Windows error %1).").arg(assignmentError);
        return false;
    }
#endif
    QElapsedTimer timer;
    timer.start();
    TunnelDiagnostics::write("loopback.wait", "Waiting for the private SOCKS5 listener and password authentication");
    while (timer.elapsed() < ReadyTimeoutMs && m_Process.state() == QProcess::Running) {
        QTcpSocket probe;
        probe.setProxy(QNetworkProxy::NoProxy);
        probe.connectToHost(QHostAddress::LocalHost, port);
        if (probe.waitForConnected(200)) {
            auto exchange = [&probe](const QByteArray& request, const QByteArray& expected) {
                if (probe.write(request) != request.size()) return false;
                if (probe.bytesToWrite() && !probe.waitForBytesWritten(500)) return false;
                QByteArray reply;
                while (reply.size() < expected.size()) {
                    if (!probe.bytesAvailable() && !probe.waitForReadyRead(500)) return false;
                    reply += probe.read(expected.size() - reply.size());
                }
                return reply == expected;
            };
            QByteArray auth(1, '\x01');
            auth += char(user.toUtf8().size()); auth += user.toUtf8();
            auth += char(password.toUtf8().size()); auth += password.toUtf8();
            if (exchange(QByteArray::fromHex("050102"), QByteArray::fromHex("0502")) &&
                exchange(auth, QByteArray::fromHex("0100"))) {
                {
                    QWriteLocker guard(&tunnelLock);
                    tunnelUser = user;
                    tunnelPassword = password;
                    tunnelPort = port;
                }
                succeeded = true;
                updateState(tr("Xray ready (local proxy authenticated)"));
                TunnelDiagnostics::write("startup.ready", QString("elapsedMs=%1 loopbackPort=%2").arg(total.elapsed()).arg(port));
                emit stateChanged();
                qInfo() << "Integrated VLESS/Reality tunnel ready (TCP and UDP), local port:" << port
                        << "core PID:" << m_Process.processId();
                return true;
            }
        }
        m_Process.waitForReadyRead(50);
    }
    *error = tr("Xray did not become ready within %1 seconds. %2\nOpen Network Settings to retry or export a diagnostic report.")
        .arg(ReadyTimeoutMs / 1000).arg(m_LastOutput.isEmpty() ? m_Process.errorString() : m_LastOutput.right(1500));
    return false;
}

bool IntegratedTunnel::active() { QReadLocker guard(&tunnelLock); return tunnelPort != 0; }

bool IntegratedTunnel::required() { QReadLocker guard(&tunnelLock); return tunnelRequired; }
void IntegratedTunnel::setRequired(bool required) { QWriteLocker guard(&tunnelLock); tunnelRequired = required; }
bool IntegratedTunnel::hasProfile() { return QFile::exists(profilePath()); }

QString IntegratedTunnel::profilePath()
{
#ifdef Q_OS_DARWIN
    // Keep the .app bundle immutable, including when launched from a read-only DMG.
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/Xray/profile.json";
#else
    return QCoreApplication::applicationDirPath() + "/Xray/profile.json";
#endif
}

QString IntegratedTunnel::coreDirectory()
{
#ifdef Q_OS_DARWIN
    const QString resources = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../Resources/Xray");
    if (QFile::exists(resources + "/xray")) return resources;
#endif
    return QCoreApplication::applicationDirPath() + "/Xray";
}

bool IntegratedTunnel::hasBundledCore() { return QFile::exists(corePath()); }

bool IntegratedTunnel::importProfile(const QUrl& source, QString* error)
{
    error->clear();
    QElapsedTimer elapsed;
    elapsed.start();
    bool succeeded = false;
    auto logResult = qScopeGuard([&] {
        *error = TunnelDiagnostics::sanitize(*error);
        TunnelDiagnostics::write(succeeded ? "import.saved" : "import.failed",
            QString("elapsedMs=%1 error=%2").arg(elapsed.elapsed()).arg(*error));
    });
    TunnelDiagnostics::write("import.begin", "Importing a local JSON profile; content is not logged");
    if (!source.isLocalFile()) {
        *error = QObject::tr("Choose a local JSON profile file.");
        return false;
    }
    QFile file(source.toLocalFile());
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QObject::tr("Cannot read the selected profile: %1").arg(file.errorString());
        return false;
    }
    if (file.size() > 1024 * 1024) {
        *error = QObject::tr("The profile is too large (maximum 1 MiB).");
        return false;
    }
    QJsonObject config;
    if (!parseProfile(file.readAll(), &config, error)) return false;

    // Validate the effective configuration, including our private inbound,
    // before replacing any existing user profile. No connections are started.
    QTemporaryFile validationConfig(QDir::tempPath() + "/Moonlight-Xray-Import-XXXXXX.json");
    const auto validationBytes = QJsonDocument(runtimeConfig(config, 10808, "validate", "validate")).toJson();
    if (!validationConfig.open() || validationConfig.write(validationBytes) != validationBytes.size() ||
        !validationConfig.flush()) {
        *error = QObject::tr("Cannot create a temporary profile validation file.");
        return false;
    }
    validationConfig.close();
    QProcess validator;
    validator.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    validator.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    validator.setWorkingDirectory(IntegratedTunnel::coreDirectory());
    validator.start(corePath(), {"run", "-test", "-c", validationConfig.fileName()});
    if (!validator.waitForStarted(ProcessStartTimeoutMs)) {
        *error = QObject::tr("Cannot start bundled Xray to validate the profile: %1").arg(validator.errorString());
        return false;
    }
    TunnelDiagnostics::write("import.validate", QString("Validation process started; timeoutMs=%1").arg(ValidationTimeoutMs));
    if (!validator.waitForFinished(ValidationTimeoutMs)) {
        validator.kill();
        validator.waitForFinished(1000);
        *error = QObject::tr("Profile validation timed out after %1 seconds. Export a diagnostic report from Network Settings.").arg(ValidationTimeoutMs / 1000);
        return false;
    }
    if (validator.exitStatus() != QProcess::NormalExit || validator.exitCode() != 0) {
        *error = QObject::tr("Xray rejected this profile:\n%1").arg(QString::fromUtf8(validator.readAll()).left(2000));
        return false;
    }
    const QString destinationPath = IntegratedTunnel::profilePath();
    if (!QDir().mkpath(QFileInfo(destinationPath).absolutePath())) {
        *error = QObject::tr("Cannot create the profile storage directory.");
        return false;
    }
    QSaveFile destination(destinationPath);
    const auto storedBytes = QJsonDocument(config).toJson(QJsonDocument::Indented);
    if (!destination.open(QIODevice::WriteOnly) || destination.write(storedBytes) != storedBytes.size() ||
        !destination.commit()) {
        *error = QObject::tr("Cannot save the imported profile: %1").arg(destination.errorString());
        return false;
    }
    succeeded = true;
    return true;
}

QNetworkProxy IntegratedTunnel::proxy()
{
    QReadLocker guard(&tunnelLock);
    return QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", tunnelPort ? tunnelPort : 1,
                         tunnelUser, tunnelPassword);
}

void IntegratedTunnel::configureStreaming()
{
    const auto configured = proxy();
    const auto user = configured.user().toUtf8();
    const auto password = configured.password().toUtf8();
    LiSetSocks5Proxy(true, "127.0.0.1", configured.port(), user.constData(), password.constData());
}

IntegratedTunnel* IntegratedTunnel::instance() { return tunnelInstance; }
QString IntegratedTunnel::status() { QReadLocker guard(&tunnelLock); return tunnelInstance ? tunnelInstance->m_Status : QObject::tr("Not started"); }
QString IntegratedTunnel::lastError() { QReadLocker guard(&tunnelLock); return tunnelInstance ? tunnelInstance->m_LastError : QString(); }

void IntegratedTunnel::updateState(const QString& status, const QString& error)
{
    { QWriteLocker guard(&tunnelLock); m_Status = status; m_LastError = error; }
    emit stateChanged();
}

void IntegratedTunnel::startAsync()
{
    if (m_Starting || (active() && !m_RestartRequested)) return;
    m_RetryTimer.stop();
    m_NetworkTimer.stop();
    m_Starting = true;
    emit stateChanged();
    auto result = std::make_shared<QPair<bool, QString>>();
    auto ownerThread = thread();
    auto worker = new QThread(this);
    m_StartupWorker = worker;
    m_Process.moveToThread(worker);
    m_Config.moveToThread(worker);
    connect(worker, &QThread::started, [this, worker, ownerThread, result] {
        if (m_RestartRequested && active()) stop();
        result->first = start(&result->second);
        m_Process.moveToThread(ownerThread);
        m_Config.moveToThread(ownerThread);
        worker->quit();
    });
    connect(worker, &QThread::finished, this, [this, worker, result] {
        m_Starting = false;
        if (m_StartupWorker == worker) m_StartupWorker = nullptr;
        worker->deleteLater();
        if (result->first && m_Process.state() == QProcess::Running) {
            m_RetryAttempt = 0;
            m_RestartRequested = false;
            QNetworkProxy::setApplicationProxy(proxy());
            m_NetworkTimer.start();
            emit connectionRecovered();
        } else {
            if (active()) stop();
            scheduleRetry();
        }
        emit stateChanged();
    });
    worker->start();
}

void IntegratedTunnel::scheduleRetry()
{
    if (!m_AutomaticRetry || !required() || !m_RetryAllowed || m_Starting) return;
    static const int delays[] = {1000, 2000, 5000, 10000, 20000, 30000};
    const int delay = delays[qMin(m_RetryAttempt, 5)];
    ++m_RetryAttempt;
    TunnelDiagnostics::write("reconnect.scheduled", QString("attempt=%1 delayMs=%2").arg(m_RetryAttempt).arg(delay));
    updateState(tr("Reconnecting Xray in %1 seconds (attempt %2)...").arg(delay / 1000).arg(m_RetryAttempt), lastError());
    m_RetryTimer.start(delay);
    m_NetworkTimer.start();
}

void IntegratedTunnel::requestReconnect()
{
    if (!required() || !m_AutomaticRetry || !hasProfile() || m_Starting) return;
    m_RetryAttempt = 0;
    m_RestartRequested = active();
    m_RetryAllowed = true;
    TunnelDiagnostics::write("reconnect.requested", "Recovering tunnel after a network change without relaunching Moonlight");
    scheduleRetry();
}

void IntegratedTunnel::setAutomaticRetryEnabled(bool enabled)
{
    m_AutomaticRetry = enabled;
    if (!enabled) m_RetryTimer.stop();
}

void IntegratedTunnel::retryNow()
{
    if (m_Starting) return;
    m_RetryAttempt = 0;
    m_RestartRequested = active();
    m_RetryAllowed = true;
    startAsync();
}

QString IntegratedTunnel::connectionDiagnostics()
{
    QFile file(profilePath());
    QString error;
    if (!file.open(QIODevice::ReadOnly)) return QObject::tr("Cannot read the imported profile.");
    QJsonObject config;
    if (!parseProfile(file.readAll(), &config, &error)) return error;
    QStringList results;
    const auto outbounds = config.value("outbounds").toArray();
    for (int index = 0; index < outbounds.size(); ++index) {
        const auto outbound = outbounds[index].toObject();
        const auto settings = outbound.value("settings").toObject();
        auto servers = settings.value("vnext").toArray();
        if (servers.isEmpty()) servers = settings.value("servers").toArray();
        if (servers.isEmpty()) continue;
        const auto server = servers.first().toObject();
        const auto address = server.value("address").toString();
        const int port = server.value("port").toInt();
        if (address.isEmpty() || port <= 0 || port > 65535) continue;
        QElapsedTimer timer;
        timer.start();
        QTcpSocket socket;
        socket.setProxy(QNetworkProxy::NoProxy);
        socket.connectToHost(address, quint16(port));
        const bool connected = socket.waitForConnected(8000);
        const auto result = QObject::tr("Outbound %1 (%2), port %3: %4 (%5 ms).")
            .arg(index).arg(outbound.value("protocol").toString()).arg(port)
            .arg(connected ? QObject::tr("TCP reachable") : TunnelDiagnostics::sanitize(socket.errorString())).arg(timer.elapsed());
        results << result;
        TunnelDiagnostics::write("endpoint.tcp", result);
        if (results.size() >= 3) break;
    }
    if (results.isEmpty()) results << QObject::tr("This profile has no supported remote endpoint to probe.");
    results << QObject::tr("TCP reachability does not verify VLESS/Reality credentials or streaming UDP. Endpoint addresses and credentials are omitted from the report.");
    return results.join('\n');
}
