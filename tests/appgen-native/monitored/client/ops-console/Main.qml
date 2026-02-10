// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The monitoring console. One page. What every entity is doing right now, what the monitor
// itself is doing, and a way to ask a different question.
//
// `Server` is the monitor, which is this client's edge; `Ops` names its contract, which
// is what an attached signal handler is written against. Every value shown arrives through
// the framework's own `Console` contract, so this file knows nothing about the system it
// watches.
import SynQt
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window

    readonly property color accent: "#00a6ed"
    readonly property color line: "#2b3358"
    readonly property color muted: "#8f96c4"
    readonly property color surface: "#161c33"
    readonly property color surfaceHigh: "#1e2542"
    readonly property color textColor: "#d7dafa"

    function ask(): void {
        Server.ask(search.text, entityFilter.text, severityFilter.currentValue, 500);
    }

    function severityColor(severity: string): color {
        if (severity === "error" || severity === "fatal") {
            return "#ff6b6b";
        }
        if (severity === "warning") {
            return "#e6b450";
        }
        return window.muted;
    }

    color: "#0d1224"
    height: 800
    title: qsTr("SynQt monitor")
    visible: true
    width: 1280

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            // What the monitor itself is doing. `dropped` is the half that admits loss. The
            // pipeline drops rather than blocks, so a quiet period and a hole in the record
            // look the same until something says which it was.
            Repeater {
                model: [
                    { "label": qsTr("received"), "value": Server.received, "warn": false },
                    { "label": qsTr("stored"), "value": Server.stored, "warn": false },
                    { "label": qsTr("dropped"), "value": Server.dropped, "warn": true }
                ]

                delegate: Rectangle {
                    id: counter

                    required property var modelData

                    border.color: window.line
                    border.width: 1
                    color: window.surface
                    implicitHeight: 64
                    implicitWidth: 150
                    radius: 10

                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 2

                        Label {
                            color: window.muted
                            font.pixelSize: 12
                            text: counter.modelData.label
                        }

                        Label {
                            color: counter.modelData.warn && counter.modelData.value > 0
                                   ? "#e6b450" : window.textColor
                            font.pixelSize: 22
                            text: counter.modelData.value
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
            }

            // One row per entity the monitor has heard from. An entity that has stopped
            // reporting is the case a monitor exists to notice, and silence is
            // indistinguishable from health without a row that says when it was last heard.
            Repeater {
                model: Server.entities

                delegate: Rectangle {
                    id: health

                    required property string name
                    required property double events
                    required property double refusals
                    required property bool live

                    border.color: window.line
                    border.width: 1
                    color: window.surface
                    implicitHeight: 64
                    implicitWidth: 160
                    radius: 10

                    RowLayout {
                        anchors.centerIn: parent
                        spacing: 8

                        Rectangle {
                            color: health.live ? "#46f477" : "#ff6b6b"
                            height: 8
                            radius: 4
                            width: 8
                        }

                        ColumnLayout {
                            spacing: 2

                            Label {
                                color: window.textColor
                                font.pixelSize: 14
                                text: health.name
                            }

                            Label {
                                color: window.muted
                                font.pixelSize: 11
                                text: qsTr("%1 events, %2 refused").arg(health.events)
                                                                   .arg(health.refusals)
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: search

                Layout.fillWidth: true
                placeholderText: qsTr("search what was said, or paste a trace id")

                onAccepted: window.ask()
            }

            // A field rather than a list of entities. The list lives in the `entities`
            // model, which is a QAbstractItemModel and not something QML can turn into a
            // combo box's array without the Source publishing a second copy of it.
            TextField {
                id: entityFilter

                Layout.preferredWidth: 160
                placeholderText: qsTr("every entity")

                onAccepted: window.ask()
            }

            ComboBox {
                id: severityFilter

                model: [
                    { "text": qsTr("everything"), "value": "trace" },
                    { "text": qsTr("info and worse"), "value": "info" },
                    { "text": qsTr("warnings and worse"), "value": "warning" },
                    { "text": qsTr("errors only"), "value": "error" }
                ]
                textRole: "text"
                valueRole: "value"

                onActivated: window.ask()
            }
        }

        Rectangle {
            Layout.fillHeight: true
            Layout.fillWidth: true
            border.color: window.line
            border.width: 1
            color: window.surface
            radius: 10

            ListView {
                id: tail

                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: Server.events

                ScrollBar.vertical: ScrollBar {}

                delegate: Rectangle {
                    id: row

                    required property int index
                    required property double ts
                    required property string severity
                    required property string category
                    required property string entity
                    required property string message
                    required property double durationMs
                    required property string traceId

                    color: row.index % 2 === 0 ? "transparent" : window.surfaceHigh
                    height: 30
                    width: tail.width

                    RowLayout {
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 10

                        Label {
                            Layout.preferredWidth: 90
                            color: window.muted
                            font.family: "monospace"
                            font.pixelSize: 12
                            text: new Date(row.ts).toLocaleTimeString(Qt.locale(),
                                                                      "HH:mm:ss.zzz")
                        }

                        Label {
                            Layout.preferredWidth: 70
                            color: window.severityColor(row.severity)
                            font.pixelSize: 12
                            text: row.severity
                        }

                        Label {
                            Layout.preferredWidth: 100
                            color: window.accent
                            font.pixelSize: 12
                            text: row.entity
                        }

                        Label {
                            Layout.preferredWidth: 110
                            color: window.muted
                            font.pixelSize: 12
                            text: row.category
                        }

                        Label {
                            Layout.fillWidth: true
                            color: window.textColor
                            elide: Text.ElideRight
                            font.pixelSize: 13
                            text: row.message
                        }

                        Label {
                            color: window.muted
                            font.pixelSize: 11
                            text: row.durationMs > 0 ? row.durationMs.toFixed(1) + " ms" : ""
                        }

                        // One click's whole story, across every entity it touched.
                        Label {
                            color: window.accent
                            font.pixelSize: 11
                            text: row.traceId.length > 0 ? qsTr("trace") : ""

                            TapHandler {
                                onTapped: Server.follow(row.traceId)
                            }
                        }
                    }
                }
            }
        }
    }

    // What the last question could not answer, said to the operator who asked it. The
    // attached handler names the contract, which is the monitor's own name.
    Ops.onRefused: reason => {
        search.placeholderText = reason;
    }
}
