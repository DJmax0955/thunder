import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

ApplicationWindow {
    id: window
    width: 740
    height: 580
    minimumWidth: 200
    minimumHeight: 250
    visible: true
    title: qsTr("Thunder")

    // Github link (Homepage button)
    property string githubUrl: "https://github.com/DJmax0955/thunder"

    // -- Theme
    property bool lightMode: Application.styleHints.colorScheme === Qt.Light
    property color reallyDark: "#080911"
    property color dark: "#0E1226"
    property color reallyLight: "#e7e7e7"
    property color light: "#e0e0e0"
    property color colorPS3: "#00CCBB"
    property color colorArcade: "#FFFFFF"
    property color colorX360: "#36CC00"
    property color colorWii: "#FFFFFF"
    property real uiHeightScale: 0.8

    // Buttons and their gradient colors cuz cool addition -- need to add feature that pointer to menu visible
    property var toolButtons: [
        { label: qsTr("Texture packer"), top: "#82d478", bottom: "#3c6e36", action: "texturePacker" },
        { label: qsTr("Model converter"), top: "#f04545", bottom: "#932222" },
        { label: qsTr("DLC Package"), top: "#f0e245", bottom: "#938622" },
        { label: qsTr("Other tools"), top: "#353fce", bottom: "#190c62" }
    ]

    property var featureRows: [
        {
            title: qsTr("Texture packer"),
            color: "#36CC00",
            lines: [
                qsTr("| Unpack .thb, .tbb and .tstream files"),
                qsTr("| Repack modified textures back"),
                qsTr("| Supports <font color='%1'>PS3</font>, <font color='%2'>X360</font>, and <font color='%3'>Wii</font>")
                    .arg(window.colorPS3).arg(window.colorX360).arg(window.colorWii)
            ]
        },
        {
            title: qsTr("Model converter"),
            color: "#CC0000",
            lines: [
                qsTr("| Convert models between platforms"),
                qsTr("| e.g. <font color='%1'>Arcade</font> -> <font color='%2'>PS3</font>")
                    .arg(window.colorArcade).arg(window.colorPS3)
            ]
        },
        {
            title: qsTr("DLC Package"),
            color: "#CCB100",
            lines: [
                qsTr("| Create DLC character packs"),
                qsTr("| Supports <font color='%1'>PS3</font>, <font color='%2'>X360</font> Only")
                    .arg(window.colorPS3).arg(window.colorX360)
            ]
        },
        {
            title: qsTr("Other tools"),
            color: "#00CCBB",
            lines: [
                qsTr("| Modify Octane Engine's .xml spreadsheets")
            ]
        }
    ]

    // Fonts
    FontLoader {
        id: appFontRegular
        source: "qrc:/qt/qml/Thunder/fonts/segoeui.ttf"
    }
    FontLoader {
        id: appFontBold
        source: "qrc:/qt/qml/Thunder/fonts/segoeui.ttf"
    }

    font.family: segoeUI.name

    menuBar: MenuBar {
        delegate: MenuBarItem {
            id: menuBarItem
            topPadding: 2
            bottomPadding: 2
            leftPadding: 10
            rightPadding: 10

            contentItem: Text {
                text: menuBarItem.text
                font: menuBarItem.font
                color: "white"
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: menuBarItem.highlighted ? "#333" : "transparent"
            }
        }
        background: Rectangle {
            color: "#111"
        }

        Menu {
            title: qsTr("File")
            MenuItem { text: qsTr("Exit"); onTriggered: Qt.quit() }
        }
        Menu {
            title: qsTr("Help")
            MenuItem { text: qsTr("Credits"); onTriggered: creditsDialog.open() }
            MenuItem { text: qsTr("About Thunder") }
        }
    }

    // Tool windows - created once, hidden until opened from a button.
    TexturePacker {
        id: texturePackerWindow
        visible: false
    }

    Rectangle {
        anchors.fill: parent
        color: window.lightMode ? window.reallyLight : window.reallyDark

        // Homepage button stays at the window corner directly, so resizing the window it stays in same pos
        Button {
            id: githubButton
            text: qsTr("Homepage")
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: 12
            implicitWidth: 90
            implicitHeight: 32

            contentItem: Text {
                text: parent.text
                color: "white"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 6
                color: "#222"
                border.color: "#444"
            }
            onClicked: Qt.openUrlExternally(window.githubUrl)
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            Label {
                text: qsTr("Welcome To Thunder!")
                font.pixelSize: 42
                font.bold: true
                color: window.lightMode ? window.dark : window.light
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 48
            }

            Label {
                text: qsTr("You are now on the latest version")
                font.pixelSize: 16
                color: window.lightMode ? window.dark : window.light
                opacity: 0.6
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 12
            }

            // Info panel - grows/shrinks with the window width instead
            // of staying locked at a fixed pixel width.
            Rectangle {
                id: infoPanel
                radius: 10
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 24
                Layout.fillWidth: true
                Layout.minimumWidth: 300
                Layout.maximumWidth: 600
                implicitHeight: infoContent.implicitHeight + (32 * window.uiHeightScale)
                color: "black"

                ColumnLayout {
                    id: infoContent
                    anchors.fill: parent
                    anchors.margins: 16 * window.uiHeightScale
                    spacing: 18 * window.uiHeightScale

                    Label {
                        text: qsTr("Thunder Octane Engine Game modding toolkit, version 1.0\nSelect an option to get started.")
                        color: "white"
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        Layout.bottomMargin: 6
                    }

                    // One RowLayout per entry in window.featureRows -
                    // add/remove/edit a tool by editing that list above,
                    // not by copy-pasting a RowLayout block here.
                    Repeater {
                        model: window.featureRows

                        RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 12

                            Label {
                                text: modelData.title
                                color: modelData.color
                                font.bold: true
                                Layout.alignment: Qt.AlignTop
                                Layout.preferredWidth: 110
                            }
                            ColumnLayout {
                                spacing: 2
                                Repeater {
                                    model: modelData.lines
                                    Label {
                                        required property var modelData
                                        text: modelData
                                        textFormat: Text.StyledText
                                        color: "white"
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Item { Layout.fillHeight: true }

            // Bottom button row - one Button per entry in
            // window.toolButtons. Each button uses Layout.fillWidth so
            // they grow/shrink together and stay evenly spaced.
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.bottomMargin: 56 * window.uiHeightScale
                spacing: 24

                Repeater {
                    model: window.toolButtons

                    Button {
                        id: toolButton
                        required property var modelData
                        text: modelData.label
                        Layout.fillWidth: true
                        Layout.minimumWidth: 110
                        implicitHeight: 55 * window.uiHeightScale

                        contentItem: Text {
                            text: toolButton.text
                            color: "white"
                            font.pixelSize: 17
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            wrapMode: Text.WordWrap
                        }
                        background: Rectangle {
                            id: outerRect
                            radius: 10
                            gradient: Gradient {
                                orientation: Gradient.Vertical
                                GradientStop { position: 0.0; color: toolButton.modelData.top }
                                GradientStop { position: 1.0; color: toolButton.modelData.bottom }
                            }
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: 2
                                radius: outerRect.radius - 2
                                color: "black"
                            }
                        }

                        onClicked: {
                            if (toolButton.modelData.action === "texturePacker") {
                                texturePackerWindow.show()
                                texturePackerWindow.raise()
                                texturePackerWindow.requestActivate()
                            } else {
                                console.log("No screen wired up yet for: " + toolButton.modelData.label)
                            }
                        }
                    }
                }
            }
        }
    }
}
