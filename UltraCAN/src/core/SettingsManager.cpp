#include "SettingsManager.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>

namespace {
// Key strings live here and NOWHERE else. See the class note.
constexpr auto KeyPlugin       = "bus/plugin";
constexpr auto KeyInterface    = "bus/interface";
constexpr auto KeyBitrate      = "bus/bitrate";
constexpr auto KeySimulator    = "bus/simulator";
constexpr auto KeyDbcUrl       = "dbc/lastUrl";
constexpr auto KeyDbcAutoLoad  = "dbc/autoLoad";
constexpr auto KeyLogCapacity  = "logger/capacity";
constexpr auto KeyExportFolder = "logger/lastExportFolder";
constexpr auto KeyExportFormat = "logger/lastExportFormat";

constexpr int SyncDelayMs = 500;

/*! A ring buffer this small is useless and this big is a memory bomb. The
    stored value is clamped on the way in, so a hand-edited .ini cannot take
    the application down. */
constexpr int MinCapacity = 100;
constexpr int MaxCapacity = 2000000;

QString configFileFor()
{
    // AppConfigLocation already includes the organization/application names
    // set in main(), so this lands on
    // %APPDATA%/Basri Arzuoglu/UltraCAN/UltraCAN.ini.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/UltraCAN.ini");
}
}

SettingsManager *SettingsManager::s_instance = nullptr;

SettingsManager::SettingsManager(QObject *parent)
    : QObject(parent)
    , m_settings(configFileFor(), QSettings::IniFormat)
{
    if (!s_instance) {
        s_instance = this;
    } else {
        qCritical("SettingsManager: a SECOND instance was created (%p; first was %p).",
                  static_cast<void *>(this), static_cast<void *>(s_instance));
    }

    m_syncTimer = new QTimer(this);
    m_syncTimer->setSingleShot(true);
    m_syncTimer->setInterval(SyncDelayMs);
    connect(m_syncTimer, &QTimer::timeout, this, [this] { m_settings.sync(); });

    load();
}

SettingsManager::~SettingsManager()
{
    // Last chance: whatever the coalescer was still holding.
    m_settings.sync();
    if (s_instance == this)
        s_instance = nullptr;
}

SettingsManager *SettingsManager::instance()
{
    return s_instance;
}

QString SettingsManager::configFilePath() const
{
    return m_settings.fileName();
}

void SettingsManager::load()
{
    m_canPlugin      = m_settings.value(QLatin1String(KeyPlugin)).toString();
    m_canInterface   = m_settings.value(QLatin1String(KeyInterface)).toString();
    m_bitrate        = m_settings.value(QLatin1String(KeyBitrate), m_bitrate).toInt();
    m_simulatorEnabled = m_settings.value(QLatin1String(KeySimulator),
                                          m_simulatorEnabled).toBool();
    // Stored as text, not as a QVariant(QUrl): QSettings would write a
    // @Variant(...) base64 blob into the .ini and undo the whole reason for
    // choosing a human-readable format.
    m_lastDbcUrl     = QUrl(m_settings.value(QLatin1String(KeyDbcUrl)).toString());
    m_autoLoadDbc    = m_settings.value(QLatin1String(KeyDbcAutoLoad), m_autoLoadDbc).toBool();
    m_logCapacity    = m_settings.value(QLatin1String(KeyLogCapacity), m_logCapacity).toInt();
    m_lastExportFolder = QUrl(m_settings.value(QLatin1String(KeyExportFolder)).toString());
    m_lastExportFormat = m_settings.value(QLatin1String(KeyExportFormat), m_lastExportFormat).toInt();

    // Never trust the file: it is plain text a user can edit.
    if (m_bitrate <= 0)
        m_bitrate = 500000;
    m_logCapacity = qBound(MinCapacity, m_logCapacity, MaxCapacity);
    if (m_lastExportFormat < 0)
        m_lastExportFormat = 0;
}

void SettingsManager::store(const QString &key, const QVariant &value)
{
    m_settings.setValue(key, value);
    scheduleSync();
}

void SettingsManager::scheduleSync()
{
    // start() on a running single-shot timer restarts it, which is exactly the
    // coalescing we want: a burst of setters costs one write.
    m_syncTimer->start();
}

void SettingsManager::flush()
{
    m_syncTimer->stop();
    m_settings.sync();
}

// ------------------------------------------------------------------ setters

void SettingsManager::setCanPlugin(const QString &plugin)
{
    if (m_canPlugin == plugin)
        return;
    m_canPlugin = plugin;
    store(QLatin1String(KeyPlugin), plugin);
    emit canPluginChanged();
}

void SettingsManager::setCanInterface(const QString &interfaceName)
{
    if (m_canInterface == interfaceName)
        return;
    m_canInterface = interfaceName;
    store(QLatin1String(KeyInterface), interfaceName);
    emit canInterfaceChanged();
}

void SettingsManager::setBitrate(int bitrate)
{
    if (bitrate <= 0 || m_bitrate == bitrate)
        return;
    m_bitrate = bitrate;
    store(QLatin1String(KeyBitrate), bitrate);
    emit bitrateChanged();
}

void SettingsManager::setSimulatorEnabled(bool enabled)
{
    if (m_simulatorEnabled == enabled)
        return;
    m_simulatorEnabled = enabled;
    store(QLatin1String(KeySimulator), enabled);
    emit simulatorEnabledChanged();
}

void SettingsManager::setLastDbcUrl(const QUrl &url)
{
    if (m_lastDbcUrl == url)
        return;
    m_lastDbcUrl = url;
    store(QLatin1String(KeyDbcUrl), url.toString());
    emit lastDbcUrlChanged();
}

void SettingsManager::setAutoLoadDbc(bool enabled)
{
    if (m_autoLoadDbc == enabled)
        return;
    m_autoLoadDbc = enabled;
    store(QLatin1String(KeyDbcAutoLoad), enabled);
    emit autoLoadDbcChanged();
}

void SettingsManager::setLogCapacity(int capacity)
{
    const int clamped = qBound(MinCapacity, capacity, MaxCapacity);
    if (m_logCapacity == clamped)
        return;
    m_logCapacity = clamped;
    store(QLatin1String(KeyLogCapacity), clamped);
    emit logCapacityChanged();
}

void SettingsManager::setLastExportFolder(const QUrl &folder)
{
    if (m_lastExportFolder == folder)
        return;
    m_lastExportFolder = folder;
    store(QLatin1String(KeyExportFolder), folder.toString());
    emit lastExportFolderChanged();
}

void SettingsManager::setLastExportFormat(int format)
{
    if (format < 0 || m_lastExportFormat == format)
        return;
    m_lastExportFormat = format;
    store(QLatin1String(KeyExportFormat), format);
    emit lastExportFormatChanged();
}

// ------------------------------------------------------------------ helpers

void SettingsManager::rememberConnection(const QString &plugin, const QString &interfaceName,
                                         int bitrate)
{
    setCanPlugin(plugin);
    setCanInterface(interfaceName);
    setBitrate(bitrate);
    // A connection that came up is worth not losing to a hard kill.
    flush();
    qInfo("Settings: remembered %s/%s @ %d bit/s",
          qPrintable(plugin), qPrintable(interfaceName), bitrate);
}

bool SettingsManager::hasRestorableDbc() const
{
    if (!m_autoLoadDbc || m_lastDbcUrl.isEmpty())
        return false;
    if (!m_lastDbcUrl.isLocalFile())
        return false;
    return QFileInfo::exists(m_lastDbcUrl.toLocalFile());
}

void SettingsManager::resetToDefaults()
{
    m_settings.clear();
    m_settings.sync();

    m_canPlugin.clear();
    m_canInterface.clear();
    m_bitrate = 500000;
    m_simulatorEnabled = false;
    m_lastDbcUrl = QUrl();
    // Must track the member's declared default, or "reset to defaults" would
    // hand back a state the application never starts in.
    m_autoLoadDbc = false;
    m_logCapacity = 20000;
    m_lastExportFolder = QUrl();
    m_lastExportFormat = 0;

    emit canPluginChanged();
    emit canInterfaceChanged();
    emit bitrateChanged();
    emit simulatorEnabledChanged();
    emit lastDbcUrlChanged();
    emit autoLoadDbcChanged();
    emit logCapacityChanged();
    emit lastExportFolderChanged();
    emit lastExportFormatChanged();
}
