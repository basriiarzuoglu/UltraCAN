import QtQuick

/*!
    \qmltype NavRoute
    \brief A single, self-describing navigation destination.

    A route bundles together everything the shell needs to render a nav entry
    and instantiate its page. Because \l page is a \c Component (not a string
    path), routing is compile-time checked: a typo becomes a build/parse error
    instead of a silent blank screen at runtime.
*/
QtObject {
    /*! Stable machine identifier. Used for persistence / telemetry, never for lookup by string. */
    required property string routeId

    /*! Human readable label shown in the sidebar and page header. */
    required property string title

    /*! Short glyph or text badge used as an icon placeholder (swap for an SVG later). */
    property string icon: "■"

    /*! One-line description shown as the page subtitle / tooltip. */
    property string description: ""

    /*! The page to instantiate in the content Loader. */
    required property Component page

    /*!
        Build this page at start-up instead of on first visit.

        The shell keeps every visited page alive but creates none of them up
        front, which is right for a page that only draws itself. It is wrong
        for a page that OWNS state the rest of the application asks it for: the
        graph workspace lives nowhere but in Graph Plot's own item tree, so a
        session exported before that page was ever opened would carry an empty
        workspace and nobody would find out until the import.

        Use sparingly - each eager route is start-up cost paid by every user,
        whether they open it or not.
    */
    property bool eager: false
}
