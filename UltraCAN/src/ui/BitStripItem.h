#pragma once

#include <QColor>
#include <QQuickPaintedItem>
#include <QVariantList>
#include <QVector>
#include <QtQml/qqmlregistration.h>

class CanAnalyzerModel;

/*!
    \brief One row of the analyzer heatmap, painted rather than assembled.

    \section1 Why this is not QML

    The obvious implementation is a Repeater of 64 Rectangles per row. With
    twenty-five rows on screen that is 1600 QQuickItems, each with its own
    colour binding, re-evaluated ten times a second - 16000 binding
    evaluations and 1600 scene-graph nodes for what is visually a strip of
    coloured squares. It works, and it makes the whole application stutter.

    One QQuickPaintedItem per row is twenty-five items total. Each paints its
    cells with QPainter into a cached texture that is only re-rasterised when
    the row's data actually changes, and it reads the floats straight out of
    the model - no QVariantList marshalling, no per-cell property binding.

    \section1 Where the colours come from

    Nowhere in this file. \l ramp is set from Theme, and the item interpolates
    between the stops it is given, so the heat scale stays a design token like
    every other colour in the application.
*/
class BitStripItem : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT

    /*! The analyzer to read from. Not owned. */
    Q_PROPERTY(CanAnalyzerModel *source READ source WRITE setSource NOTIFY sourceChanged FINAL)
    /*! Which display row this strip draws. */
    Q_PROPERTY(int row READ row WRITE setRow NOTIFY rowChanged FINAL)

    /*! Colour stops, coolest first. Interpolated across activity 0..1. */
    Q_PROPERTY(QVariantList ramp READ ramp WRITE setRamp NOTIFY rampChanged FINAL)
    /*! Fill for a cell with no activity at all. */
    Q_PROPERTY(QColor inactiveColor READ inactiveColor WRITE setInactiveColor
               NOTIFY inactiveColorChanged FINAL)
    /*! Glyph colour on an inactive cell; active cells pick black or white by
        the luminance of their own fill. */
    Q_PROPERTY(QColor glyphColor READ glyphColor WRITE setGlyphColor
               NOTIFY glyphColorChanged FINAL)
    Q_PROPERTY(QColor selectionColor READ selectionColor WRITE setSelectionColor
               NOTIFY selectionColorChanged FINAL)

    /*! Draw the bit values (0 / I) or byte values over the cells. Suppressed
        automatically when the cells get too narrow to read. */
    Q_PROPERTY(bool showValues READ showValues WRITE setShowValues NOTIFY showValuesChanged FINAL)

public:
    explicit BitStripItem(QQuickItem *parent = nullptr);

    void paint(QPainter *painter) override;

    CanAnalyzerModel *source() const { return m_source; }
    void setSource(CanAnalyzerModel *source);

    int row() const { return m_row; }
    void setRow(int row);

    QVariantList ramp() const { return m_rampSource; }
    void setRamp(const QVariantList &ramp);

    QColor inactiveColor() const { return m_inactiveColor; }
    void setInactiveColor(const QColor &color);

    QColor glyphColor() const { return m_glyphColor; }
    void setGlyphColor(const QColor &color);

    QColor selectionColor() const { return m_selectionColor; }
    void setSelectionColor(const QColor &color);

    bool showValues() const { return m_showValues; }
    void setShowValues(bool show);

    /*! Display cell under \a x, or -1. Used by the view to turn a click into a
        bit index - the geometry lives here, so only this file knows it. */
    Q_INVOKABLE int cellAt(qreal x) const;

signals:
    void sourceChanged();
    void rowChanged();
    void rampChanged();
    void inactiveColorChanged();
    void glyphColorChanged();
    void selectionColorChanged();
    void showValuesChanged();

private:
    QColor heatColor(float activity) const;
    int cellCount() const;

    CanAnalyzerModel *m_source = nullptr;
    int m_row = -1;

    QVariantList m_rampSource;
    QVector<QColor> m_ramp;
    QColor m_inactiveColor { QStringLiteral("#000000") };
    QColor m_glyphColor { QStringLiteral("#8A8A8A") };
    QColor m_selectionColor { QStringLiteral("#FFFFFF") };
    bool m_showValues = true;
};
