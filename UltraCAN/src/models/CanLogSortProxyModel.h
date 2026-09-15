#pragma once

#include <QSortFilterProxyModel>
#include <QtQml/qqmlregistration.h>

class CanLogModel;

/*!
    \class CanLogSortProxyModel
    \brief Column sorting for the Logger's trace, on top of \c CanLogModel.

    \section1 dynamicSortFilter is OFF, and that is the design

    The obvious build - a proxy with dynamic sorting on a live model - produces
    a table nobody can use, and the reason is already written down in
    \c CanAnalyzerModel:

    \quotation
    Sorting REINDEXES; it never moves rows. beginMoveRows at 10 Hz would make
    the view unusable - the scroll position fights the data and a delegate the
    user is pointing at slides out from under the cursor.
    \endquotation

    Here it would be worse than 10 Hz. In overwrite mode Count rises on every
    frame and Frequency drifts continuously, so a dynamic sort on either column
    would reorder the list several times a second, forever - the row under the
    pointer would never be the row that gets clicked.

    So the sort is a SNAPSHOT: \l toggleSort() re-sorts once, and the rows then
    stay where they are while their numbers keep updating in place. Clicking
    the same header again re-sorts and flips the direction. That is also what
    the question actually is - "which ids are busiest right now" is answered by
    an ordering taken at a moment, not by a list that is still rearranging
    itself while you read it.

    A newly discovered id appends at the bottom rather than sorting itself in.
    That is deliberate too: it is the one row that genuinely is new, and having
    it appear where you are looking beats having it shuffle the rest.

    \section1 Off entirely in chronological mode

    \l CanLogModel in chronological mode is a time-ordered record, and its row
    order IS information. Sorting it by id would silently destroy the one
    property a trace has. \l sortingAvailable follows the source's
    \c overwriteMode and the proxy is a pass-through whenever it is false.
*/
class CanLogSortProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT
    QML_ANONYMOUS

    /*! False in chronological mode - see the class note. QML greys the headers
        from this. */
    Q_PROPERTY(bool sortingAvailable READ sortingAvailable NOTIFY sortStateChanged FINAL)
    /*! Role name currently sorted on, empty when unsorted. */
    Q_PROPERTY(QString sortRoleName READ sortRoleName NOTIFY sortStateChanged FINAL)
    /*! Direction of the active sort. */
    Q_PROPERTY(bool sortDescending READ sortDescending NOTIFY sortStateChanged FINAL)
    /*! Mirrors the source, so QML can bind one object instead of two. */
    Q_PROPERTY(bool overwriteMode READ overwriteMode NOTIFY sortStateChanged FINAL)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged FINAL)

public:
    explicit CanLogSortProxyModel(QObject *parent = nullptr);

    void setLogModel(CanLogModel *model);

    bool sortingAvailable() const;
    bool overwriteMode() const;
    QString sortRoleName() const { return m_sortRoleName; }
    bool sortDescending() const { return m_descending; }

    /*!
        Sorts on \a roleName, flipping direction if it is already the active
        column. Passing the active column a third time is still a re-sort, not
        a no-op: the numbers have moved since, and re-clicking is how the user
        asks for a fresh ordering.

        An unknown or unsortable role clears the sort rather than throwing.
    */
    Q_INVOKABLE void toggleSort(const QString &roleName);
    /*! Drops the sort and returns to source order. */
    Q_INVOKABLE void clearSort();
    /*! Re-runs the current sort against the values as they are now. */
    Q_INVOKABLE void resort();

    /*! Source row for \a proxyRow, or -1.

        QML needs this because the trace's detail panel reads the SOURCE model
        by index (\c trace.get(row)) while the delegate now hands out PROXY
        indexes. Without the mapping the panel silently describes a different
        frame - it still shows something, which is what makes it dangerous. */
    Q_INVOKABLE int sourceRow(int proxyRow) const;

signals:
    void sortStateChanged();
    void countChanged();

protected:
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    /*! Model role for \a roleName, or -1 when it is not one we sort on. */
    int roleFor(const QString &roleName) const;

    CanLogModel *m_log = nullptr;
    QString m_sortRoleName;
    bool m_descending = false;
};
