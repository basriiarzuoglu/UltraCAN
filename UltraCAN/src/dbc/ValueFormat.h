#pragma once

#include <QMap>
#include <QString>

#include <cmath>
#include <limits>

/*!
    \file ValueFormat.h
    \brief The ONE rule for turning a decoded signal value into display text.

    Two unrelated objects show decoded values - the Raw Analyzer (through its
    own private CanDecoder) and the DBC Editor grid (through DbcSignalObject) -
    and they must agree, or the same signal reads "32 (P)" on one page and "P"
    on the other. Neither can reach the other's types, so the rule lives here
    as a free function they both call.
*/
namespace UltraCAN {

/*! Decimal places for a plain numeric readout. Three everywhere, trailing
    zeros included: a column that jumps between "3500" and "3500.125" as values
    cross an integer is unreadable when you are scanning for the one that moved. */
inline constexpr int ValueDecimals = 3;

/*!
    Raw code behind a physical value, or \c false when there is not an exact one.

    VAL_ tables are keyed on the RAW, pre-scaling integer - that is what the
    file stores and what the bus carries - so the scaling has to be undone
    before the lookup. Reading the table with the scaled number would work only
    while factor is 1 and offset 0, which is the trap: it keeps working for
    every case anyone tests.

    Only an EXACT integer counts. A value half-way between two codes is not
    "nearly" either of them, and rounding to the nearest would confidently
    mislabel it - the worst possible failure for a display whose whole job is
    telling you which gear the car is in.
*/
inline bool rawCodeFor(double physical, double factor, double offset, int *code)
{
    const double f = (factor == 0.0) ? 1.0 : factor;
    const double raw = (physical - offset) / f;
    const double rounded = std::round(raw);

    if (std::abs(raw - rounded) > 1e-6)
        return false;
    if (rounded < double(std::numeric_limits<int>::min())
        || rounded > double(std::numeric_limits<int>::max())) {
        return false;
    }
    if (code)
        *code = int(rounded);
    return true;
}

/*!
    Display text for one decoded value.

    \list
    \li mapped by a VAL_ entry -> \c{"32 (P)"}
    \li anything else           -> \c{"32.000"}
    \endlist

    The padding zeros are dropped for a named code deliberately: an enumeration
    member is a code, not a measurement, and "32.000 (P)" reads as a precision
    claim about something that has none.

    The NUMBER shown is the physical value, not the raw code, so it still agrees
    with the minimum, maximum and unit displayed beside it. For an enumerated
    signal those are almost always the same number (factor 1, offset 0); where
    they are not, the physical value is the one that matches the rest of the row.
*/
inline QString formatSignalValue(double physical, double factor, double offset,
                                 const QMap<int, QString> &valueDescriptions)
{
    if (!valueDescriptions.isEmpty()) {
        int code = 0;
        if (rawCodeFor(physical, factor, offset, &code)) {
            const auto it = valueDescriptions.constFind(code);
            if (it != valueDescriptions.constEnd()) {
                // 'g' with 12 significant digits: renders 32 as "32" rather
                // than "32.000000", without letting binary-float noise print
                // as "0.30000000000000004".
                return QStringLiteral("%1 (%2)")
                       .arg(QString::number(physical, 'g', 12), it.value());
            }
        }
    }
    return QString::number(physical, 'f', ValueDecimals);
}

/*! The description alone, empty when the value is not a named code. For a
    caller that wants to decide its own layout. */
inline QString describeSignalValue(double physical, double factor, double offset,
                                   const QMap<int, QString> &valueDescriptions)
{
    if (valueDescriptions.isEmpty())
        return {};
    int code = 0;
    if (!rawCodeFor(physical, factor, offset, &code))
        return {};
    return valueDescriptions.value(code);
}

/*! True when \a valueDescriptions names this value - lets a caller suppress the
    unit, because a named enumeration member does not have one. */
inline bool isNamedValue(double physical, double factor, double offset,
                         const QMap<int, QString> &valueDescriptions)
{
    return !describeSignalValue(physical, factor, offset, valueDescriptions).isEmpty();
}

} // namespace UltraCAN
