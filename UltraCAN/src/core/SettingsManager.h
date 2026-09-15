#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

class QTimer;

/*!
    \brief Everything the application should still know the next time it starts.

    A thin, typed facade over QSettings. The value of putting it here rather
    than calling QSettings from five places is that the KEY STRINGS exist once:
    a typo in a key is not a compile error, it is a setting that silently never
    comes back, and that class of bug is invisible until a user complains that
    "it forgot my bitrate again".

    \b {Storage format.} IniFormat under QStandardPaths::AppConfigLocation, not
    the Windows registry. An industrial tool gets support tickets, and
    "attach %APPDATA%/Basri Arzuoglu/UltraCAN.ini" is an instruction a technician
    follow; "export this registry key" is not. \l configFilePath is exposed so
    the UI can show exactly which file is in play.

    \b {Write policy.} QSettings only guarantees a flush on destruction, and a
    DAQ application is exactly the kind of program that gets killed rather than
    closed. Every setter therefore schedules a coalesced sync() 500 ms later:
    one disk write per burst of edits, and nothing is riding on a clean exit.

    \b {What gets remembered, and when.} Only state that actually took effect.
    \l rememberConnection() is called when the bus reaches Connected, never
    when the user picks something from a combo box - otherwise a plugin that
    was tried once and failed becomes the default forever. Same for
    \l lastDbcUrl, which follows DbcModel::sourceUrl and so is only written
    after a file really parsed.
*/
class SettingsManager : public QObject
{
    Q_OBJECT
    // Registered with qmlRegisterSingletonInstance() from main(), like the rest
    // of the backend - see the long note in main.cpp. QML_ANONYMOUS exposes the
    // TYPE (so properties and Q_ENUMs resolve) without claiming the NAME.
    QML_ANONYMOUS

    // -- hardware ------------------------------------------------------------
    Q_PROPERTY(QString canPlugin READ canPlugin WRITE setCanPlugin NOTIFY canPluginChanged FINAL)
    Q_PROPERTY(QString canInterface READ canInterface WRITE setCanInterface
               NOTIFY canInterfaceChanged FINAL)
    Q_PROPERTY(int bitrate READ bitrate WRITE setBitrate NOTIFY bitrateChanged FINAL)

    // -- database ------------------------------------------------------------
    /*! The last .dbc that parsed. Still recorded even while \l autoLoadDbc is
        off - it costs nothing, and it is what makes turning the switch back on
        take effect immediately instead of only after the next load. */
    Q_PROPERTY(QUrl lastDbcUrl READ lastDbcUrl WRITE setLastDbcUrl NOTIFY lastDbcUrlChanged FINAL)

    /*! Reopen \l lastDbcUrl at start-up. \b {Off by default.}

        It was on, and it made the DBC Editor lie about its own state: the
        application came up showing a document nobody had opened this session,
        so an edit made "to a blank file" was really an edit to last week's
        database, and Save wrote it back without asking. An editor that starts
        with something in it has to be something the user asked for.

        The switch survives because the behaviour is defensible for someone who
        works on one database all day; it just cannot be the default. Nothing
        sets it yet - there is no UI - so flipping it means editing the .ini
        that \l configFilePath() reports. */
    Q_PROPERTY(bool autoLoadDbc READ autoLoadDbc WRITE setAutoLoadDbc
               NOTIFY autoLoadDbcChanged FINAL)

    /*! Off by default. The built-in traffic generator is a development aid, and
        an aid that runs unasked is indistinguishable from a fault. */
    Q_PROPERTY(bool simulatorEnabled READ simulatorEnabled WRITE setSimulatorEnabled
               NOTIFY simulatorEnabledChanged FINAL)

    // -- logger --------------------------------------------------------------
    Q_PROPERTY(int logCapacity READ logCapacity WRITE setLogCapacity
               NOTIFY logCapacityChanged FINAL)
    Q_PROPERTY(QUrl lastExportFolder READ lastExportFolder WRITE setLastExportFolder
               NOTIFY lastExportFolderChanged FINAL)
    Q_PROPERTY(int lastExportFormat READ lastExportFormat WRITE setLastExportFormat
               NOTIFY lastExportFormatChanged FINAL)

    Q_PROPERTY(QString configFilePath READ configFilePath CONSTANT FINAL)

public:
    explicit SettingsManager(QObject *parent = nullptr);
    ~SettingsManager() override;

    static SettingsManager *instance();

    QString canPlugin() const { return m_canPlugin; }
    void setCanPlugin(const QString &plugin);

    QString canInterface() const { return m_canInterface; }
    void setCanInterface(const QString &interfaceName);

    int bitrate() const { return m_bitrate; }
    void setBitrate(int bitrate);

    bool simulatorEnabled() const { return m_simulatorEnabled; }
    void setSimulatorEnabled(bool enabled);

    QUrl lastDbcUrl() const { return m_lastDbcUrl; }
    void setLastDbcUrl(const QUrl &url);

    bool autoLoadDbc() const { return m_autoLoadDbc; }
    void setAutoLoadDbc(bool enabled);

    int logCapacity() const { return m_logCapacity; }
    void setLogCapacity(int capacity);

    QUrl lastExportFolder() const { return m_lastExportFolder; }
    void setLastExportFolder(const QUrl &folder);

    int lastExportFormat() const { return m_lastExportFormat; }
    void setLastExportFormat(int format);

    QString configFilePath() const;

    /*! Records a connection that actually came up. Call from the Connected
        transition, not from the UI - see the class note. */
    Q_INVOKABLE void rememberConnection(const QString &plugin, const QString &interfaceName,
                                        int bitrate);

    /*! True when there is a remembered .dbc that still exists on disk. Checked
        before the startup auto-load so a moved file is not an error dialog. */
    Q_INVOKABLE bool hasRestorableDbc() const;

    /*! Flushes immediately. Called on shutdown; otherwise the 500 ms coalescer
        does it. */
    Q_INVOKABLE void flush();

    /*! Wipes the store and returns every property to its compiled-in default. */
    Q_INVOKABLE void resetToDefaults();

signals:
    void canPluginChanged();
    void canInterfaceChanged();
    void bitrateChanged();
    void simulatorEnabledChanged();
    void lastDbcUrlChanged();
    void autoLoadDbcChanged();
    void logCapacityChanged();
    void lastExportFolderChanged();
    void lastExportFormatChanged();

private:
    void load();
    void store(const QString &key, const QVariant &value);
    void scheduleSync();

    static SettingsManager *s_instance;

    QSettings m_settings;
    QTimer *m_syncTimer = nullptr;

    // Defaults are the values a first run should behave sensibly with, NOT
    // empty strings: an empty plugin means "ask CanBusManager what it prefers".
    QString m_canPlugin;
    QString m_canInterface;
    int m_bitrate = 500000;
    bool m_simulatorEnabled = false;
    QUrl m_lastDbcUrl;
    // Off: see the autoLoadDbc property note. This declaration IS the effective
    // default - load() passes the member as QSettings' fallback, and nothing
    // writes the key unless setAutoLoadDbc() is called.
    bool m_autoLoadDbc = false;
    int m_logCapacity = 20000;
    QUrl m_lastExportFolder;
    int m_lastExportFormat = 0;          // LogExporter::Csv
};
