#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

class CanLogModel;

/*!
    \brief Writes the captured trace to disk in the formats the Log Player reads.

    \b {Why a separate class.} CanLogModel is a model: it owns a ring buffer and
    feeds a view. File formats, atomic replace semantics and a worker thread are
    a different job with a different failure mode, and bolting them onto the
    model would mean every future format touches the class the acquisition path
    runs through. CanLogModel gained exactly one thing for this: snapshot().

    \b {The concurrency shape that is actually correct.} The obvious "export on
    a background thread" is wrong in a way that produces a plausible-looking
    file: the ring is mutated on the GUI thread, so a worker walking it races
    the flush and silently emits duplicated and missing frames. The split is
    therefore
      1. snapshot on the GUI thread - one allocation and a memcpy, ~1 ms for a
         full 20k ring, during which the producer keeps staging safely;
      2. format and write on a worker - the expensive part (string building
         dominates; a 500k-frame .asc is tens of megabytes of text).
    Nothing is dropped either way: enqueue() only takes a mutex and appends, so
    even a blocked GUI thread costs latency, never frames.

    Small traces skip the thread entirely (see \l SyncFrameLimit): handing 200
    frames to a thread pool costs more than writing them, and a synchronous
    finish means the status line updates in the same frame as the click.

    \b {Atomic replace.} QSaveFile, always. Half a trace file with the right
    name is worse than no file: it looks like a successful export until someone
    tries to replay it.

    The three formats are exactly the three CanPlaybackModel can read, so
    anything this application records, it can also replay.
*/
class LogExporter : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged FINAL)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged FINAL)
    /*! Labels for a format picker, indexed by \l Format. */
    Q_PROPERTY(QStringList formatNames READ formatNames CONSTANT FINAL)

public:
    enum Format {
        Csv = 0,    //!< Our own Time,Dir,Ch,ID,Type,DLC,Data - round-trips exactly.
        Asc,        //!< Vector ASCII trace.
        Trc         //!< PCAN trace, 2.0 dialect.
    };
    Q_ENUM(Format)

    explicit LogExporter(QObject *parent = nullptr);
    ~LogExporter() override;

    static LogExporter *instance();

    /*! The trace to export. Set once from main(). */
    void setSource(CanLogModel *model);

    bool isBusy() const { return m_busy; }
    QString lastError() const { return m_lastError; }
    QStringList formatNames() const;

    /*!
        Snapshots the trace and writes it to \a fileUrl.

        \a format is a \l Format value, or -1 to infer it from the file
        extension (the default, so the FileDialog's suffix decides and the user
        is never asked the same question twice).

        Returns false only for problems visible before any writing starts (no
        source, empty trace, unusable url). Everything else - including every
        I/O error - arrives through \l finished, because by then the work may
        be on another thread.
    */
    Q_INVOKABLE bool exportLog(const QUrl &fileUrl, int format = -1);

    /*! Format for a file extension; falls back to \l Csv. */
    Q_INVOKABLE int formatForSuffix(const QString &suffix) const;
    /*! Extension for a format, without the dot. */
    Q_INVOKABLE QString suffixFor(int format) const;

signals:
    void busyChanged();
    void lastErrorChanged();

    /*! Emitted before the write begins, with the count already fixed. */
    void started(const QString &fileName, int frameCount);
    /*! Always emitted exactly once per accepted exportLog(), on the GUI thread.
        Synchronously from inside exportLog() for a small trace. */
    void finished(bool ok, const QString &fileName, int frameCount, const QString &error);

private:
    /*! Below this many frames the write happens inline - see the class note. */
    static constexpr int SyncFrameLimit = 2000;

    void setBusy(bool busy);
    void setLastError(const QString &message);
    /*! GUI thread. Reports the outcome and clears \l busy. Q_INVOKABLE because
        the worker reaches it by name through a queued invokeMethod - a lambda
        could not, this being private. */
    Q_INVOKABLE void reportResult(bool ok, const QString &fileName, int frameCount,
                                  const QString &error);

    static LogExporter *s_instance;

    CanLogModel *m_source = nullptr;
    bool m_busy = false;
    QString m_lastError;
};
