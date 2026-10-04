#include "integratedtunnel.h"
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
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
quint16 tunnelPort = 0;
QString tunnelUser;
QString tunnelPassword;
bool tunnelRequired = false;

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
    return true;
}

QJsonObject runtimeConfig(QJsonObject config, quint16 port, const QString& user, const QString& password)
{
    const QJsonObject settings{{"auth", "password"}, {"udp", true}, {"ip", "127.0.0.1"},
                              {"accounts", QJsonArray{QJsonObject{{"user", user}, {"pass", password}}}}};
    config["inbounds"] = QJsonArray{QJsonObject{{"tag", "moonlight"}, {"listen", "127.0.0.1"},
                                               {"port", port}, {"protocol", "socks"}, {"settings", settings}}};
    config["log"] = QJsonObject{{"loglevel", "warning"}};
    return config;
}
}

IntegratedTunnel::IntegratedTunnel(QObject* parent) : QObject(parent)
{
    m_Process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_Process, &QProcess::readyReadStandardOutput, this, [this]() {
        const auto output = m_Process.readAllStandardOutput().trimmed();
        if (!output.isEmpty()) qInfo().noquote() << "Xray:" << output;
    });
    connect(&m_Process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        if (!m_Stopping && tunnelPort != 0) {
            qCritical() << "Integrated tunnel stopped unexpectedly, exit code:" << code;
            // Keep the loopback proxy configured until exit; never switch to direct.
            QCoreApplication::exit(1);
        }
    });
}

IntegratedTunnel::~IntegratedTunnel()
{
    m_Stopping = true;
    if (m_Process.state() != QProcess::NotRunning) {
        m_Process.terminate();
        if (!m_Process.waitForFinished(1500)) {
            m_Process.kill();
            m_Process.waitForFinished(1500);
        }
    }
#ifdef Q_OS_WIN
    if (m_Job) CloseHandle(static_cast<HANDLE>(m_Job));
#endif
    tunnelPort = 0;
    tunnelUser.clear();
    tunnelPassword.clear();
}

bool IntegratedTunnel::start(QString* error)
{
    tunnelRequired = true;
    const QDir coreDir(IntegratedTunnel::coreDirectory());
    QFile profile(IntegratedTunnel::profilePath());
    if (!profile.open(QIODevice::ReadOnly)) {
        *error = tr("Cannot open the imported Xray profile: %1").arg(profile.errorString());
        return false;
    }
    QJsonObject config;
    if (!parseProfile(profile.readAll(), &config, error)) return false;
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
    if (!m_Config.open()) {
        *error = "Cannot create the temporary tunnel configuration.";
        return false;
    }
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
        *error = "Cannot create the Xray process lifetime job.";
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
    if (!m_Process.waitForStarted(5000)) {
        *error = "Cannot start bundled Xray: " + m_Process.errorString();
        return false;
    }
#ifdef Q_OS_WIN
    HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                               static_cast<DWORD>(m_Process.processId()));
    const bool assigned = child && AssignProcessToJobObject(static_cast<HANDLE>(m_Job), child);
    if (child) CloseHandle(child);
    if (!assigned) {
        *error = "Cannot attach Xray to Moonlight's process lifetime job.";
        return false;
    }
#endif
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000 && m_Process.state() == QProcess::Running) {
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
                tunnelUser = user;
                tunnelPassword = password;
                tunnelPort = port;
                qInfo() << "Integrated VLESS/Reality tunnel ready (TCP and UDP), local port:" << port
                        << "core PID:" << m_Process.processId();
                return true;
            }
        }
        m_Process.waitForReadyRead(50);
    }
    *error = "Xray did not become ready. Check the Moonlight log for the core error.";
    return false;
}

bool IntegratedTunnel::active() { return tunnelPort != 0; }

bool IntegratedTunnel::required() { return tunnelRequired; }
void IntegratedTunnel::setRequired(bool required) { tunnelRequired = required; }
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
    if (!validator.waitForStarted(3000)) {
        *error = QObject::tr("Cannot start bundled Xray to validate the profile: %1").arg(validator.errorString());
        return false;
    }
    if (!validator.waitForFinished(7000)) {
        validator.kill();
        validator.waitForFinished(1000);
        *error = QObject::tr("Profile validation timed out.");
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
    return true;
}

QNetworkProxy IntegratedTunnel::proxy()
{
    return QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", tunnelPort ? tunnelPort : 1,
                         tunnelUser, tunnelPassword);
}

void IntegratedTunnel::configureStreaming()
{
    const auto user = tunnelUser.toUtf8();
    const auto password = tunnelPassword.toUtf8();
    LiSetSocks5Proxy(true, "127.0.0.1", tunnelPort, user.constData(), password.constData());
}
