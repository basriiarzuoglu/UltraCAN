#include "BitStripItem.h"

#include <QFont>
#include <QPainter>

#include "models/CanAnalyzerModel.h"

namespace {
/*! Below this a glyph is a smudge, so the cell is drawn as colour only. */
constexpr qreal MinGlyphCellWidth = 7.0;
/*! Cells are separated by leaving a pixel of background, not by drawing a
    line: one fewer draw call per cell, sixty-four per row. */
constexpr qreal CellGap = 1.0;
}

BitStripItem::BitStripItem(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    // The strip is opaque and rectangular; antialiasing it buys nothing.
    setAntialiasing(false);
    // Image, not FramebufferObject. An FBO per strip would mean twenty-five
    // render targets and twenty-five context switches per frame for items a
    // few hundred pixels wide; software rasterising into one texture each is
    // cheaper at this size and works identically on every RHI backend.
    setRenderTarget(QQuickPaintedItem::Image);
    setOpaquePainting(true);
}

void BitStripItem::setSource(CanAnalyzerModel *source)
{
    if (m_source == source)
        return;

    if (m_source)
        disconnect(m_source, nullptr, this, nullptr);

    m_source = source;

    if (m_source) {
        // One signal for the whole table, once per refresh. Connecting to
        // dataChanged instead would repaint every strip once per role.
        connect(m_source, &CanAnalyzerModel::refreshed, this,
                [this] { update(); });
    }

    emit sourceChanged();
    update();
}

void BitStripItem::setRow(int row)
{
    if (m_row == row)
        return;
    m_row = row;
    emit rowChanged();
    update();
}

void BitStripItem::setRamp(const QVariantList &ramp)
{
    if (m_rampSource == ramp)
        return;
    m_rampSource = ramp;
    m_ramp.clear();
    m_ramp.reserve(ramp.size());
    for (const QVariant &value : ramp) {
        // Theme declares the ramp as a list of colour LITERALS, so what arrives
        // here is usually QString, not QColor. Accept both rather than
        // silently rendering an empty ramp (which looks like "no activity" -
        // the most confusing possible failure for this view).
        QColor color = value.value<QColor>();
        if (!color.isValid())
            color = QColor(value.toString());
        if (color.isValid())
            m_ramp.append(color);
    }
    if (m_ramp.isEmpty() && !ramp.isEmpty())
        qWarning("BitStripItem: none of the %lld ramp entries were colours.",
                 qint64(ramp.size()));
    emit rampChanged();
    update();
}

void BitStripItem::setInactiveColor(const QColor &color)
{
    if (m_inactiveColor == color)
        return;
    m_inactiveColor = color;
    emit inactiveColorChanged();
    update();
}

void BitStripItem::setGlyphColor(const QColor &color)
{
    if (m_glyphColor == color)
        return;
    m_glyphColor = color;
    emit glyphColorChanged();
    update();
}

void BitStripItem::setSelectionColor(const QColor &color)
{
    if (m_selectionColor == color)
        return;
    m_selectionColor = color;
    emit selectionColorChanged();
    update();
}

void BitStripItem::setShowValues(bool show)
{
    if (m_showValues == show)
        return;
    m_showValues = show;
    emit showValuesChanged();
    update();
}

int BitStripItem::cellCount() const
{
    return m_source ? m_source->cellCount() : 64;
}

int BitStripItem::cellAt(qreal x) const
{
    const int cells = cellCount();
    if (cells <= 0 || width() <= 0)
        return -1;
    const int cell = int(x / (width() / cells));
    return (cell < 0 || cell >= cells) ? -1 : cell;
}

QColor BitStripItem::heatColor(float activity) const
{
    if (m_ramp.isEmpty())
        return m_inactiveColor;
    if (activity <= 0.0f)
        return m_ramp.first();
    if (activity >= 1.0f)
        return m_ramp.last();

    // Piecewise-linear through the stops. Interpolating in HSV would rotate
    // through hues nobody asked for; a heat scale must go straight from one
    // stop to the next.
    const float scaled = activity * float(m_ramp.size() - 1);
    const int lower = int(scaled);
    const int upper = qMin(lower + 1, m_ramp.size() - 1);
    const qreal t = qreal(scaled - float(lower));

    const QColor &a = m_ramp[lower];
    const QColor &b = m_ramp[upper];
    return QColor::fromRgbF(a.redF()   + (b.redF()   - a.redF())   * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF()  + (b.blueF()  - a.blueF())  * t);
}

void BitStripItem::paint(QPainter *painter)
{
    if (!m_source || m_row < 0 || !m_source->rowIsValid(m_row))
        return;

    const float *activity = m_source->cellActivity(m_row);
    const quint8 *values = m_source->cellValues(m_row);
    if (!activity || !values)
        return;

    const int cells = cellCount();
    const qreal cellWidth = width() / qreal(cells);
    const qreal h = height();
    if (cellWidth <= 0.0 || h <= 0.0)
        return;

    const bool isByteMode = cells <= 8;
    const bool drawGlyphs = m_showValues && cellWidth >= MinGlyphCellWidth;

    painter->setPen(Qt::NoPen);
    painter->fillRect(QRectF(0, 0, width(), h), m_inactiveColor);

    QFont glyphFont = painter->font();
    glyphFont.setPixelSize(int(qBound(7.0, h * 0.62, 13.0)));
    glyphFont.setFamily(QStringLiteral("Consolas"));
    if (drawGlyphs)
        painter->setFont(glyphFont);

    // Selection is only meaningful on the row that owns it.
    const bool rowSelected = m_source->selectionRow() == m_row;
    const int firstCell = rowSelected ? m_source->firstBit() : -1;
    const int secondCell = rowSelected ? m_source->secondBit() : -1;

    for (int cell = 0; cell < cells; ++cell) {
        const QRectF rect(cell * cellWidth, 0.0,
                          qMax(1.0, cellWidth - CellGap), h);

        const QColor fill = heatColor(activity[cell]);
        painter->fillRect(rect, fill);

        if (drawGlyphs) {
            // Pick the glyph colour from the fill's own luminance: on a bright
            // yellow cell a light glyph is invisible, and this view is read by
            // scanning for the glyph that stopped changing.
            const qreal luminance = 0.299 * fill.redF() + 0.587 * fill.greenF()
                                    + 0.114 * fill.blueF();
            const bool lit = activity[cell] > 0.02f;
            painter->setPen(lit ? (luminance > 0.55 ? QColor(Qt::black) : QColor(Qt::white))
                                : m_glyphColor);
            painter->drawText(rect, Qt::AlignCenter,
                              isByteMode
                                  ? QStringLiteral("%1").arg(values[cell], 2, 16, QLatin1Char('0')).toUpper()
                                  : (values[cell] ? QStringLiteral("I") : QStringLiteral("0")));
            painter->setPen(Qt::NoPen);
        }

        if (cell == firstCell || cell == secondCell) {
            painter->setPen(QPen(m_selectionColor, 1.5));
            painter->drawRect(rect.adjusted(0.5, 0.5, -0.5, -0.5));
            painter->setPen(Qt::NoPen);
        }
    }

    // Tint the span between the two picks so the field being carved out reads
    // as one thing rather than two loose markers.
    if (firstCell >= 0 && secondCell >= 0) {
        const int low = qMin(firstCell, secondCell);
        const int high = qMax(firstCell, secondCell);
        QColor wash = m_selectionColor;
        wash.setAlphaF(0.16);
        painter->fillRect(QRectF(low * cellWidth, 0.0,
                                 (high - low + 1) * cellWidth, h), wash);
    }
}
