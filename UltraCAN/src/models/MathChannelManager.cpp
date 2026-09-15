#include "MathChannelManager.h"

#include <QDebug>
#include <QSet>

#include <cmath>

namespace {

/*! The only functions a formula may call.

    Chosen for what a technician converting units or clamping a reading
    actually needs. Everything omitted is omitted on purpose: no Date (a
    formula must be a pure function of its inputs, or the same data would plot
    differently on a re-run), no random, nothing that touches the outside. */
const QStringList &allowedMathFunctions()
{
    static const QStringList names {
        QStringLiteral("abs"),   QStringLiteral("min"),   QStringLiteral("max"),
        QStringLiteral("sqrt"),  QStringLiteral("cbrt"),  QStringLiteral("pow"),
        QStringLiteral("sin"),   QStringLiteral("cos"),   QStringLiteral("tan"),
        QStringLiteral("asin"),  QStringLiteral("acos"),  QStringLiteral("atan"),
        QStringLiteral("atan2"), QStringLiteral("log"),   QStringLiteral("log2"),
        QStringLiteral("log10"), QStringLiteral("exp"),   QStringLiteral("floor"),
        QStringLiteral("ceil"),  QStringLiteral("round"), QStringLiteral("trunc"),
        QStringLiteral("sign"),  QStringLiteral("hypot")
    };
    return names;
}

/*! Constants a formula may name. PI and E only - they are values, not doors. */
const QStringList &allowedMathConstants()
{
    static const QStringList names { QStringLiteral("PI"), QStringLiteral("E") };
    return names;
}

/*! True for a name the language owns, so it can never be a signal variable.

    One predicate for the whole class: the scanner uses it to accept a bare
    call, variablesIn() uses it to refuse a mapping row, and createChannel()
    uses it to refuse a binding that a config file tried to smuggle in. Three
    call sites, one list - which is the only way they can agree. */
bool isReservedWord(const QString &word)
{
    return word == QLatin1String("Math")
           || allowedMathFunctions().contains(word)
           || allowedMathConstants().contains(word);
}

/*!
    Binds every allowed name into scope ONCE, around the compiled function.

    Two other shapes were considered and rejected:

    \list
    \li A regex rewriting \c sin( to \c {Math.sin(} before compiling. It edits
        text the user typed, so every error message afterwards quotes a formula
        they never wrote, and the stored expression and the compiled one drift
        apart.
    \li The same \c const block INSIDE the function body. Correct, but it
        re-runs 23 property lookups on every sample - 60 times a second per
        channel, to rebuild constants that never change.
    \endlist

    An immediately-invoked wrapper pays for the lookups once at compile time
    and hands back an inner function that closes over them. Per-sample cost:
    nothing.
*/
QString mathPrelude()
{
    static const QString prelude = [] {
        QStringList declarations;
        for (const QString &name : allowedMathFunctions())
            declarations << QStringLiteral("%1=Math.%1").arg(name);
        for (const QString &name : allowedMathConstants())
            declarations << QStringLiteral("%1=Math.%1").arg(name);
        return QStringLiteral("const ") + declarations.join(QLatin1Char(',')) + QLatin1Char(';');
    }();
    return prelude;
}

/*! The complete source compiled for a formula: the prelude, then the function
    the caller actually keeps. Shared by validate() and compile() so a formula
    can never pass the probe and then fail to build. */
QString wrapFormula(const QString &expression, const QStringList &variables)
{
    return QStringLiteral("(function(){ %1 return function(%2){ return (%3); }; })()")
           .arg(mathPrelude(), variables.join(QLatin1Char(',')), expression);
}

inline bool isIdentStart(QChar c) { return c.isLetter() || c == QLatin1Char('_'); }
inline bool isIdentChar(QChar c)  { return c.isLetterOrNumber() || c == QLatin1Char('_'); }

}   // namespace

MathChannelManager::MathChannelManager(QObject *parent)
    : QObject(parent)
{
    // Nothing is installed into the engine's global object. A compiled formula
    // can therefore see only its own arguments and the built-ins the whitelist
    // lets it name - and the whitelist is what stops it naming anything else.
}

QStringList MathChannelManager::allowedFunctions() const
{
    return allowedMathFunctions();
}

// ============================================================== the whitelist

QString MathChannelManager::scan(const QString &expression,
                                 const QStringList &variables) const
{
    if (expression.trimmed().isEmpty())
        return tr("The formula is empty.");

    int depth = 0;
    int i = 0;
    const int n = expression.size();

    while (i < n) {
        const QChar c = expression.at(i);

        if (c.isSpace()) {
            ++i;
            continue;
        }

        // -- numbers, including 1.5, .5 and 1e-3
        if (c.isDigit() || (c == QLatin1Char('.') && i + 1 < n && expression.at(i + 1).isDigit())) {
            bool seenDot = false;
            bool seenExp = false;
            while (i < n) {
                const QChar d = expression.at(i);
                if (d.isDigit()) { ++i; continue; }
                if (d == QLatin1Char('.') && !seenDot && !seenExp) { seenDot = true; ++i; continue; }
                if ((d == QLatin1Char('e') || d == QLatin1Char('E')) && !seenExp) {
                    seenExp = true;
                    ++i;
                    if (i < n && (expression.at(i) == QLatin1Char('+')
                                  || expression.at(i) == QLatin1Char('-')))
                        ++i;
                    if (i >= n || !expression.at(i).isDigit())
                        return tr("Malformed number: an exponent needs digits after it.");
                    continue;
                }
                break;
            }
            // A number may not run straight into a name: "2x" is a typo, and
            // JavaScript would read "2 x" as two expressions, not a product.
            if (i < n && isIdentStart(expression.at(i)))
                return tr("Missing operator between a number and “%1”.")
                       .arg(expression.mid(i, 8));
            continue;
        }

        // -- identifiers: a declared variable, or Math.<allowed>
        if (isIdentStart(c)) {
            const int start = i;
            while (i < n && isIdentChar(expression.at(i)))
                ++i;
            const QString word = expression.mid(start, i - start);

            if (word == QLatin1String("Math")) {
                if (i >= n || expression.at(i) != QLatin1Char('.'))
                    return tr("“Math” has to be followed by a function, e.g. Math.abs(x).");
                ++i;                                  // the dot
                const int memberStart = i;
                while (i < n && isIdentChar(expression.at(i)))
                    ++i;
                const QString member = expression.mid(memberStart, i - memberStart);
                if (allowedMathFunctions().contains(member)
                    || allowedMathConstants().contains(member))
                    continue;
                return tr("Math.%1 is not available. Allowed: %2.")
                       .arg(member.isEmpty() ? QStringLiteral("?") : member,
                            allowedMathFunctions().join(QStringLiteral(", ")));
            }

            if (variables.contains(word)) {
                // A variable wins over a reserved name only because
                // createChannel() refuses to create that clash in the first
                // place - see the guard there.
                continue;
            }

            // A bare call: sin(x) rather than Math.sin(x). The UI advertises
            // the short form, so the parser has to speak it - and a function
            // named without being called is a mistake worth naming, since
            // "sin" alone would otherwise reach the binding list as a variable.
            if (allowedMathFunctions().contains(word)) {
                int look = i;
                while (look < n && expression.at(look).isSpace())
                    ++look;
                if (look < n && expression.at(look) == QLatin1Char('('))
                    continue;
                return tr("“%1” is a function - write %1(x).").arg(word);
            }
            if (allowedMathConstants().contains(word))
                continue;

            // Everything else - a stray name, a keyword, a global. Named
            // rather than described, because the usual cause is a typo in a
            // variable and the user needs to see which one.
            return tr("“%1” is not one of the mapped variables (%2).")
                   .arg(word, variables.isEmpty() ? tr("none yet")
                                                  : variables.join(QStringLiteral(", ")));
        }

        // -- operators and grouping
        static const QString singles = QStringLiteral("+-*/%(),?:");
        if (c == QLatin1Char('(')) { ++depth; ++i; continue; }
        if (c == QLatin1Char(')')) {
            if (--depth < 0)
                return tr("Unbalanced brackets: a “)” with no “(”.");
            ++i;
            continue;
        }
        // Comparisons FIRST, and greedily: taking '<' as a single token leaves
        // the '=' of "<=" to be read on its own, and the assignment branch
        // below then rejects a perfectly ordinary clamp.
        if (c == QLatin1Char('<') || c == QLatin1Char('>')) {
            ++i;
            if (i < n && expression.at(i) == QLatin1Char('='))
                ++i;
            continue;
        }
        if (singles.contains(c)) { ++i; continue; }

        // Two-character comparisons, so a clamp like "A > 0 ? A : 0" works.
        if (c == QLatin1Char('=') || c == QLatin1Char('!')) {
            if (i + 1 < n && expression.at(i + 1) == QLatin1Char('=')) {
                i += 2;
                // "===" is fine too; a third '=' is just consumed below.
                if (i < n && expression.at(i) == QLatin1Char('='))
                    ++i;
                continue;
            }
            if (c == QLatin1Char('!')) { ++i; continue; }     // logical not
            return tr("Assignment is not allowed in a formula.");
        }
        if (c == QLatin1Char('&') || c == QLatin1Char('|')) {
            if (i + 1 < n && expression.at(i + 1) == c) { i += 2; continue; }
            return tr("Bitwise operators are not allowed; use && or ||.");
        }

        return tr("“%1” cannot be used in a formula.").arg(c);
    }

    if (depth != 0)
        return tr("Unbalanced brackets: %n “(” never closed.", nullptr, depth);

    return QString();
}

QStringList MathChannelManager::variablesIn(const QString &expression) const
{
    QStringList found;
    QSet<QString> seen;

    int i = 0;
    const int n = expression.size();
    while (i < n) {
        const QChar c = expression.at(i);
        if (!isIdentStart(c)) {
            ++i;
            continue;
        }
        const int start = i;
        while (i < n && isIdentChar(expression.at(i)))
            ++i;
        const QString word = expression.mid(start, i - start);

        // Math.abs -> neither "Math" nor "abs" is a variable to map.
        if (word == QLatin1String("Math")) {
            if (i < n && expression.at(i) == QLatin1Char('.')) {
                ++i;
                while (i < n && isIdentChar(expression.at(i)))
                    ++i;
            }
            continue;
        }
        // THE FIX for "sin(a) asked me to map sin": a reserved name is an
        // operation, not an input. Filtered here rather than in the dialog so
        // that the C++ that validates and the QML that draws the rows can
        // never disagree about what a variable is.
        if (isReservedWord(word))
            continue;

        if (seen.contains(word))
            continue;
        seen.insert(word);
        found.append(word);       // first-appearance order: the dialog's row order
    }
    return found;
}

QString MathChannelManager::validate(const QString &expression,
                                     const QStringList &variables) const
{
    const QString problem = scan(expression, variables);
    if (!problem.isEmpty())
        return problem;

    // Syntax the scanner cannot see - "A +" or "(A))" reversed - is caught by
    // asking the engine to parse it, WITHOUT running it. A SyntaxError from
    // evaluate() on a function literal is a parse failure; nothing executes,
    // because the literal is never called.
    QJSValue probe = m_engine.evaluate(wrapFormula(expression, variables));
    if (probe.isError())
        return tr("The formula does not parse: %1")
               .arg(probe.property(QStringLiteral("message")).toString());

    return QString();
}

// ================================================================== authoring

bool MathChannelManager::compile(Channel &channel)
{
    QStringList variables;
    for (const Binding &binding : std::as_const(channel.bindings))
        variables.append(binding.variable);

    channel.error = validate(channel.expression, variables);
    if (!channel.error.isEmpty()) {
        channel.valid = false;
        channel.function = QJSValue();
        return false;
    }

    // Compiled ONCE, here. The QJSValue this returns is a callable function;
    // evaluate() calls it per sample instead of re-parsing the string.
    channel.function = m_engine.evaluate(wrapFormula(channel.expression, variables));
    if (!channel.function.isCallable()) {
        channel.error = tr("The formula did not compile to a function.");
        channel.valid = false;
        channel.function = QJSValue();
        return false;
    }

    channel.valid = true;
    return true;
}

int MathChannelManager::createChannel(const QString &name, const QString &unit,
                                      const QString &expression,
                                      const QVariantList &bindings)
{
    const QString trimmedName = name.trimmed();
    if (trimmedName.isEmpty()) {
        setLastError(tr("A math channel needs a name."));
        return -1;
    }
    if (channelIdFor(trimmedName) >= 0) {
        // Names are how a channel is referenced from a saved session, so two
        // channels sharing one would make a restored file ambiguous.
        setLastError(tr("A math channel called “%1” already exists.").arg(trimmedName));
        return -1;
    }

    Channel channel;
    channel.id = m_nextId++;
    channel.name = trimmedName;
    channel.unit = unit.trimmed();
    channel.expression = expression.trimmed();

    for (const QVariant &entry : bindings) {
        const QVariantMap map = entry.toMap();
        Binding binding;
        binding.variable = map.value(QStringLiteral("variable")).toString();
        binding.dbcName = map.value(QStringLiteral("dbc")).toString();
        binding.messageName = map.value(QStringLiteral("message")).toString();
        binding.signalName = map.value(QStringLiteral("signal")).toString();
        if (binding.variable.isEmpty())
            continue;
        if (isReservedWord(binding.variable)) {
            // Only reachable from a hand-edited or imported session: the
            // dialog cannot offer this row. Refused rather than shadowed,
            // because a formula where "min" means a signal on one machine and
            // a function on another is not a formula.
            setLastError(tr("“%1” is a built-in name and cannot be a variable.")
                         .arg(binding.variable));
            return -1;
        }
        if (binding.signalName.isEmpty()) {
            setLastError(tr("“%1” is not mapped to a signal.").arg(binding.variable));
            return -1;
        }
        channel.bindings.append(binding);
    }

    // Every variable the formula names must have a binding, or the compiled
    // function would be called with one argument short and silently see
    // undefined - which becomes NaN, which becomes a trace that plots nothing
    // and explains nothing.
    const QStringList used = variablesIn(channel.expression);
    for (const QString &variable : used) {
        bool bound = false;
        for (const Binding &binding : std::as_const(channel.bindings))
            if (binding.variable == variable)
                bound = true;
        if (!bound) {
            setLastError(tr("“%1” appears in the formula but is not mapped.").arg(variable));
            return -1;
        }
    }

    if (!compile(channel)) {
        setLastError(channel.error);
        return -1;
    }

    m_channels.insert(channel.id, channel);
    m_order.append(channel.id);
    setLastError(QString());
    emit channelsChanged();
    emit bindingsChanged();
    return channel.id;
}

bool MathChannelManager::removeChannel(int channelId)
{
    if (!m_channels.contains(channelId))
        return false;
    m_channels.remove(channelId);
    m_order.removeAll(channelId);
    emit channelsChanged();
    emit bindingsChanged();
    return true;
}

void MathChannelManager::clearChannels()
{
    if (m_channels.isEmpty())
        return;
    m_channels.clear();
    m_order.clear();
    emit channelsChanged();
    emit bindingsChanged();
}

// ============================================================== session files

QJsonArray MathChannelManager::toJson() const
{
    QJsonArray out;
    // m_order, not the hash: the sidebar shows creation order, and a session
    // that reopens with the list shuffled looks like a different session.
    for (int id : m_order) {
        const auto it = m_channels.constFind(id);
        if (it == m_channels.constEnd())
            continue;

        QJsonArray bindings;
        for (const Binding &binding : it->bindings) {
            bindings.append(QJsonObject {
                { QStringLiteral("variable"), binding.variable },
                { QStringLiteral("dbc"),      binding.dbcName },
                { QStringLiteral("message"),  binding.messageName },
                { QStringLiteral("signal"),   binding.signalName }
            });
        }

        out.append(QJsonObject {
            { QStringLiteral("name"),     it->name },
            { QStringLiteral("unit"),     it->unit },
            // "formula" in the file, expression in the class. The file is read
            // by people with a text editor; expression is jargon.
            { QStringLiteral("formula"),  it->expression },
            { QStringLiteral("bindings"), bindings }
        });
    }
    return out;
}

int MathChannelManager::fromJson(const QJsonArray &array)
{
    // Cleared even when the array is empty: importing a session means "show me
    // what this file describes", and leaving yesterday's channels behind would
    // give a sidebar that matches neither the file nor what was open before.
    clearChannels();

    int failed = 0;
    for (const QJsonValue &value : array) {
        const QJsonObject entry = value.toObject();

        QVariantList bindings;
        for (const QJsonValue &raw : entry.value(QStringLiteral("bindings")).toArray()) {
            const QJsonObject binding = raw.toObject();
            bindings.append(QVariantMap {
                { QStringLiteral("variable"), binding.value(QStringLiteral("variable")).toString() },
                { QStringLiteral("dbc"),      binding.value(QStringLiteral("dbc")).toString() },
                { QStringLiteral("message"),  binding.value(QStringLiteral("message")).toString() },
                { QStringLiteral("signal"),   binding.value(QStringLiteral("signal")).toString() }
            });
        }

        // Deliberately the public path: same name check, same whitelist, same
        // compile. A formula that would be refused in the dialog is refused
        // here too, however the file came to contain it.
        const int id = createChannel(entry.value(QStringLiteral("name")).toString(),
                                     entry.value(QStringLiteral("unit")).toString(),
                                     entry.value(QStringLiteral("formula")).toString(),
                                     bindings);
        if (id < 0) {
            ++failed;
            qWarning("Math channel \"%s\" could not be restored: %s",
                     qPrintable(entry.value(QStringLiteral("name")).toString()),
                     qPrintable(m_lastError));
        }
    }
    return failed;
}

int MathChannelManager::channelIdFor(const QString &name) const
{
    for (int id : m_order)
        if (m_channels.value(id).name == name)
            return id;
    return -1;
}

QVariantMap MathChannelManager::channelAt(int channelId) const
{
    const auto it = m_channels.constFind(channelId);
    if (it == m_channels.constEnd())
        return {};

    QVariantList bindings;
    for (const Binding &binding : it->bindings) {
        bindings.append(QVariantMap {
            { QStringLiteral("variable"), binding.variable },
            { QStringLiteral("dbc"),      binding.dbcName },
            { QStringLiteral("message"),  binding.messageName },
            { QStringLiteral("signal"),   binding.signalName }
        });
    }
    return QVariantMap {
        { QStringLiteral("id"),         it->id },
        { QStringLiteral("name"),       it->name },
        { QStringLiteral("unit"),       it->unit },
        { QStringLiteral("expression"), it->expression },
        { QStringLiteral("valid"),      it->valid },
        { QStringLiteral("error"),      it->error },
        { QStringLiteral("bindings"),   bindings }
    };
}

QVariantList MathChannelManager::channels() const
{
    QVariantList out;
    out.reserve(m_order.size());
    for (int id : m_order)
        out.append(channelAt(id));
    return out;
}

// ================================================================ evaluation

QVector<MathChannelManager::Binding> MathChannelManager::bindingsOf(int channelId) const
{
    return m_channels.value(channelId).bindings;
}

QString MathChannelManager::nameOf(int channelId) const
{
    return m_channels.value(channelId).name;
}

QString MathChannelManager::unitOf(int channelId) const
{
    return m_channels.value(channelId).unit;
}

bool MathChannelManager::isValid(int channelId) const
{
    return m_channels.value(channelId).valid;
}

void MathChannelManager::publishSample(int channelId, double value, double timestampSeconds)
{
    const auto it = m_channels.constFind(channelId);
    if (it == m_channels.constEnd())
        return;
    emit virtualSignalUpdated(it->name, value, timestampSeconds);
}

bool MathChannelManager::evaluate(int channelId, const QVector<double> &arguments, double *out)
{
    if (!out)
        return false;

    const auto it = m_channels.find(channelId);
    if (it == m_channels.end() || !it->valid || !it->function.isCallable())
        return false;
    if (arguments.size() != it->bindings.size())
        return false;

    QJSValueList args;
    args.reserve(arguments.size());
    for (double value : arguments)
        args.append(QJSValue(value));

    const QJSValue result = it->function.call(args);
    if (result.isError())
        return false;

    const double value = result.toNumber();
    // Non-finite is a failure, not a value - see the header. One Infinity in a
    // series and the pane's auto-scale collapses around it.
    if (!std::isfinite(value))
        return false;

    *out = value;
    return true;
}

void MathChannelManager::setLastError(const QString &message)
{
    m_lastError = message;
}
