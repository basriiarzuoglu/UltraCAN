pragma Singleton

import QtQuick
import UltraCAN.Views

/*!
    \qmltype Navigation
    \brief The application's single source of truth for routing.

    The shell (Main.qml), the Sidebar and any future command palette all read
    from this one list. Adding a module = adding one NavRoute entry here; no
    other file has to change.
*/
QtObject {
    id: root

    /*! All top-level destinations, in sidebar order. */
    readonly property list<QtObject> routes: [
        NavRoute {
            routeId: "dbcEditor"
            title: qsTr("DBC Editor")
            icon: "✎"
            description: qsTr("Author and edit CAN database (.dbc) definitions.")
            page: Component { DbcEditorView {} }
        },
        NavRoute {
            routeId: "graphPlot"
            title: qsTr("Graph Plot")
            icon: "↗"
            description: qsTr("Real-time and offline signal plotting.")
            page: Component { GraphPlotView {} }
            // Built at start-up: it is the only holder of the graph workspace,
            // and ConfigManager asks for that on export. See NavRoute::eager.
            eager: true
        },
        NavRoute {
            routeId: "canLogger"
            title: qsTr("CAN Logger")
            icon: "●"
            description: qsTr("Record raw and decoded bus traffic to disk.")
            page: Component { CanLoggerView {} }
        },
        NavRoute {
            routeId: "rawAnalyzer"
            title: qsTr("Raw Data Analyzer")
            icon: "▦"
            description: qsTr("Find moving bits on an undocumented bus and turn them into signals.")
            page: Component { RawAnalyzerView {} }
        },
        NavRoute {
            routeId: "canSender"
            title: qsTr("CAN Message Sender")
            icon: "↑"
            description: qsTr("Compose, schedule and transmit CAN frames.")
            page: Component { CanMessageSenderView {} }
        },
        NavRoute {
            routeId: "diagnostics"
            title: qsTr("Diagnostics")
            icon: "◈"
            description: qsTr("UDS / OBD-II tester: read and clear trouble codes over ISO-TP.")
            page: Component { DiagnosticsView {} }
        }
    ]

    /*! Index of the currently displayed route. The one piece of routing state. */
    property int currentIndex: 0

    /*! Convenience accessor for the active route object. */
    readonly property QtObject currentRoute: routes[currentIndex]

    /*! Navigate by index (used by the sidebar). */
    function navigateToIndex(index) {
        if (index >= 0 && index < routes.length && index !== currentIndex)
            currentIndex = index
    }

    /*! Navigate by stable id (used by deep links, shortcuts, C++ backend). */
    function navigateTo(routeId) {
        for (let i = 0; i < routes.length; ++i) {
            if (routes[i].routeId === routeId) {
                currentIndex = i
                return true
            }
        }
        console.warn("Navigation: unknown routeId", routeId)
        return false
    }
}
