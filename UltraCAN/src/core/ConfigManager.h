#pragma once

#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class CanAnalyzerModel;
class DbcModel;
class GraphDataModel;
class MathChannelManager;
class CanLogModel;

/*!
    \class ConfigManager
    \brief Reads and writes a whole session - databases, analyzer setup and the
           graph workspace - as one JSON document.

    \section1 What this is not

    It is not a settings store. \c SettingsManager already remembers what the
    last launch looked like, silently, in QSettings. This is the opposite kind
    of persistence: an explicit, portable, user-named file that a technician
    mails to a colleague along with the .dbc files, so both are looking at the
    same twelve signals on the same eight panes. The two never touch each
    other, and importing a config deliberately does NOT change what the next
    launch restores.

    \section1 Who owns which half

    Two of the three sections are C++'s to produce, because C++ is where the
    state lives: the loaded database paths, and the analyzer's top-bar
    controls. The third - the graph workspace - is a tree of QML items and
    exists nowhere in C++. Rather than mirror that tree into the backend just
    so this class could read it, the QML side hands it over on demand:

    \list
    \li \b Export: \l exportConfig() emits \l graphStateRequested() and, because
        a direct signal call is synchronous, the QML handler's
        \l setGraphState() call has already landed by the time emit returns.
        The document is written after that, in the same call.
    \li \b Import: the file is parsed, the databases are reloaded, the analyzer
        is restored, and only THEN is \l graphStateRestored() emitted - so the
        QML rebuild resolves its saved signal names against databases that are
        already in place. Getting that order wrong is the difference between a
        restored workspace and eight empty panes.
    \endlist

    \section1 The databases travel inside the file

    A config used to name its .dbc files by absolute path, which made it
    useless as the thing a technician actually wants to send: the colleague on
    the other end has no C:/Users/basri. So the file now carries the databases
    themselves, under \c databases.embedded_files, keyed by bare filename.

    Three details that are not obvious:

    \list
    \li \b {Bytes, not text.} Most .dbc files in the wild are Windows-1252 -
        \c DbcParser explicitly falls back to Latin-1 for exactly that reason.
        Pushing those bytes through a JSON string as if they were UTF-8 turns
        every degree sign into U+FFFD and the round trip silently corrupts the
        units. An entry is therefore a plain JSON string only when its bytes
        really are valid UTF-8; otherwise it is
        \c {{ "encoding": "base64", "data": ... }} and comes back byte-exact.
    \li \b {The original wins when it is still there.} \c sources records where
        each file came from, and an import that finds that path on disk uses it
        instead of the extracted copy. On the machine that wrote the config
        nothing changes: the editor keeps pointing at the real file, so saving
        edits it rather than a copy. Only a machine that lacks the file gets
        the extraction.
    \li \b {Extracted to app data, not to the temp directory.} DbcModel's
        sourceUrl is persisted by SettingsManager on every change, and
        \c saveDbc() writes back to it - so a path under \c QDir::tempPath()
        would be remembered into the next launch, by which time the file is
        gone, and any Save the user did in between would have landed somewhere
        the OS is free to delete. The extraction lives beside the settings
        instead, one folder per config file, wiped and rewritten on each
        import of that config.
    \endlist

    \section1 Names, not indices

    Tracks are stored as (database, message, signal) NAMES and resolved through
    \c GraphDataModel::findSignal(). The indices \c tracksForPane() hands out
    are positions in the loaded-database list, so they only mean anything while
    exactly the same files are loaded in exactly the same order - which is
    precisely what a config file carried to another machine cannot promise.
*/
class ConfigManager : public QObject
{
    Q_OBJECT
    // Registered by instance in main(), like every other backend singleton -
    // see the long note there. QML_ANONYMOUS keeps the metatype without
    // claiming the name.
    QML_ANONYMOUS

    /*! Empty when the last operation succeeded. */
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged FINAL)
    /*! Human-readable outcome for the status bar, success or failure. */
    Q_PROPERTY(QString lastMessage READ lastMessage NOTIFY lastMessageChanged FINAL)
    /*! Databases named in the imported file that could not be opened. */
    Q_PROPERTY(QStringList missingFiles READ missingFiles NOTIFY missingFilesChanged FINAL)

public:
    explicit ConfigManager(QObject *parent = nullptr);

    /*! The version written into every file, and the highest this build reads.

        2 embeds the databases; 1 only referenced them by absolute path. The
        reader handles both - a reference that is not a key of
        \c embedded_files is treated as a path, which is exactly what a
        version 1 file contains. The bump exists so that an OLDER build
        refuses a version 2 file outright rather than trying to open
        "DHB_V2.0.dbc" as a relative path and reporting a missing file. */
    static constexpr int FormatVersion = 2;

    // -- wiring, from main() ---------------------------------------------------
    void setDbcModel(DbcModel *model);
    void setGraphModel(GraphDataModel *model);
    void setAnalyzerModel(CanAnalyzerModel *model);
    /*! Optional. Without one, a session simply carries no math channels
        and every other section behaves exactly as before. */
    void setMathChannels(MathChannelManager *manager);
    /*! Optional. Carries the logger's pass filter and trigger setup. */
    void setLogModel(CanLogModel *model);

    QString lastError() const { return m_lastError; }
    QString lastMessage() const { return m_lastMessage; }
    QStringList missingFiles() const { return m_missingFiles; }

    // -- the two operations ----------------------------------------------------
    Q_INVOKABLE bool exportConfig(const QUrl &fileUrl);
    Q_INVOKABLE bool importConfig(const QUrl &fileUrl);

    /*!
        The graph section, as QML left it. Written by the QML export handler,
        read by the QML import handler; this class only carries it.
    */
    Q_INVOKABLE void setGraphState(const QVariantMap &state);
    Q_INVOKABLE QVariantMap graphState() const;

signals:
    void lastErrorChanged();
    void lastMessageChanged();
    void missingFilesChanged();

    /*! Emitted synchronously from \l exportConfig(), before anything is
        written. The QML handler must respond with \l setGraphState(). */
    void graphStateRequested();
    /*! Emitted after a successful \l importConfig(), once every database in the
        file has been (re)loaded. The QML handler rebuilds the workspace from
        \l graphState(). */
    void graphStateRestored();

private:
    QJsonObject buildDocument();
    bool applyDocument(const QJsonObject &root, const QString &configPath);

    /*! Where \a configPath's databases are extracted to. Created if needed and
        emptied of stale .dbc files. Empty on failure. */
    static QString sessionDirFor(const QString &configPath);
    /*! Strips everything that could make an embedded_files key escape the
        session directory. A config file is something users e-mail to each
        other, so a key of "../../autoexec.dbc" has to be a filename and
        nothing more. */
    static QString safeFileName(const QString &key);
    /*! Reads \a absPath and returns the JSON value to store: a plain string
        when the bytes are valid UTF-8, otherwise a base64 envelope. Returns a
        null value when the file cannot be read. */
    static QJsonValue encodeFile(const QString &absPath);
    /*! Inverse of \l encodeFile(). Returns false when \a value is neither. */
    static bool decodeFile(const QJsonValue &value, QByteArray *out);

    void setLastError(const QString &message);
    void setLastMessage(const QString &message);
    void noteMissing(const QString &path);

    /*! Absolute local path for \a url, or an empty string for anything that is
        not a local file - a config that named a qrc: or http: database would
        be unopenable on the machine that reads it. */
    static QString localPathOf(const QUrl &url);

    DbcModel *m_dbcModel = nullptr;
    GraphDataModel *m_graphModel = nullptr;
    MathChannelManager *m_mathChannels = nullptr;
    CanLogModel *m_logModel = nullptr;
    CanAnalyzerModel *m_analyzerModel = nullptr;

    QVariantMap m_graphState;
    QString m_lastError;
    QString m_lastMessage;
    QStringList m_missingFiles;
};
