import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs

// Texture Packer tool window, opened from Main.qml when the
// "Texture packer" button is clicked.
Window {
    id: texturePackerWindow
    title: qsTr("Texture Packer")
    width: 560
    height: 620
    minimumWidth: 420
    minimumHeight: 500
    color: "#080911"

    property color accentColor: "#36CC00"
    property color panelColor: "black"
    property color textColor: "#e0e0e0"

    TexturePackerBridge {
        id: texPackerBridge
    }

    // A "set" is one asset's files, keyed by the shared file stem:
    // finn.thb + finn.tbb + finn.tszip is one set named "finn". Drop
    // one set or twenty; each is unpacked/repacked on its own, and an
    // incomplete one is named in the log instead of silently running.
    // Each entry: { name, thb, tbb, tszip }.
    property var fileSets: []
    property int activeSetIndex: 0

    property string selectedPlatform: "PS3"
    property bool logVisible: false

    // Explicit user toggle for "does this file even have streamed
    // textures" -- some .thb/.tbb files have none at all, so there's
    // no .tszip to give. The backend already handles tszipPath being
    // empty gracefully (falls back per-texture, warns only if a
    // texture actually needed one), so when this is unchecked,
    // .tszip simply isn't required to enable Unpack/Repack.
    property bool hasTstream: true
    readonly property int requiredFileCount: hasTstream ? 3 : 2

    readonly property var activeSet: (activeSetIndex >= 0 && activeSetIndex < fileSets.length)
        ? fileSets[activeSetIndex] : null
    // Kept so the preview pane and anything else that wants "the one
    // being looked at" still has a single path to read.
    readonly property string thbPath: activeSet ? activeSet.thb : ""
    readonly property string tbbPath: activeSet ? activeSet.tbb : ""
    readonly property string tstreamZipPath: activeSet ? activeSet.tszip : ""

    function setIsComplete(s) {
        return s.thb !== "" && s.tbb !== "" && (!hasTstream || s.tszip !== "")
    }

    // [".tbb", ".tszip"] for whatever this set still needs.
    function missingOf(s) {
        let miss = []
        if (s.thb === "") miss.push(".thb")
        if (s.tbb === "") miss.push(".tbb")
        if (hasTstream && s.tszip === "") miss.push(".tszip")
        return miss
    }

    function haveOf(s) {
        let have = []
        if (s.thb !== "") have.push(".thb")
        if (s.tbb !== "") have.push(".tbb")
        if (s.tszip !== "") have.push(".tszip")
        return have
    }

    readonly property int readySetCount: {
        let n = 0
        for (let i = 0; i < fileSets.length; i++)
            if (setIsComplete(fileSets[i])) n++
        return n
    }
    readonly property bool allFilesSelected: fileSets.length > 0 && readySetCount === fileSets.length
    readonly property int filesLoadedCount: activeSet ? haveOf(activeSet).length : 0

    // Writes every incomplete set to the log, one line each, naming
    // what it has and what it's missing.
    function logIncompleteSets() {
        for (let i = 0; i < fileSets.length; i++) {
            const s = fileSets[i]
            if (setIsComplete(s))
                continue
            const have = haveOf(s)
            logArea.append(qsTr("%1: missing %2%3").arg(s.name).arg(missingOf(s).join(", "))
                .arg(have.length ? qsTr("  (has %1)").arg(have.join(", ")) : qsTr("  (nothing usable)")))
        }
    }

    function dirOf(localPath) {
        return localPath.substring(0, localPath.lastIndexOf("/"))
    }

    // <folder of the .thb>/unpacked, plus a per-set subfolder once
    // more than one set is loaded so batches don't overwrite each other.
    function outputDirFor(s, kind) {
        if (!s || s.thb === "")
            return ""
        return dirOf(s.thb) + "/" + kind + (fileSets.length > 1 ? "/" + s.name : "")
    }

    // Turns a dropped/browsed file's URL into a normal local path.
    // File URLs escape anything unusual (spaces, brackets, etc), so a
    // real path with those characters comes through mangled unless we
    // decode it back first.
    function toLocalPath(fileUrl) {
        return decodeURIComponent(fileUrl.toString().replace("file:///", ""))
    }

    // Just the filename ("mcqueen.thb"), not the full path.
    function fileNameOf(localPath) {
        if (localPath === "")
            return ""
        const parts = localPath.split(/[\\/]/)
        return parts[parts.length - 1]
    }

    // "C:/g/finn.thb" -> "finn"
    function stemOf(localPath) {
        const base = fileNameOf(localPath)
        const dot = base.lastIndexOf(".")
        return dot > 0 ? base.substring(0, dot) : base
    }

    // Shared by both drag-and-drop and the Browse dialog: sorts each
    // file into the set named after its stem, creating that set on
    // first sight. Files of other types are skipped with a log line.
    function assignFile(localPath) {
        const lower = localPath.toLowerCase()
        let slot = ""
        if (lower.endsWith(".thb")) slot = "thb"
        else if (lower.endsWith(".tbb")) slot = "tbb"
        else if (lower.endsWith(".tszip")) slot = "tszip"
        else {
            logArea.append(qsTr("Unrecognized file type, skipped: %1").arg(localPath))
            return
        }

        const name = stemOf(localPath)
        let sets = fileSets.slice()
        let idx = -1
        for (let i = 0; i < sets.length; i++)
            if (sets[i].name.toLowerCase() === name.toLowerCase()) { idx = i; break }
        if (idx < 0) {
            sets.push({ name: name, thb: "", tbb: "", tszip: "" })
            idx = sets.length - 1
        }
        if (sets[idx][slot] !== "" && sets[idx][slot] !== localPath)
            logArea.append(qsTr("%1: replacing the .%2 already loaded (%3)")
                .arg(name).arg(slot).arg(fileNameOf(sets[idx][slot])))
        sets[idx][slot] = localPath
        fileSets = sets
        if (activeSetIndex >= fileSets.length)
            activeSetIndex = 0
        logArea.append(qsTr("Loaded .%1 for %2: %3").arg(slot).arg(name).arg(localPath))
    }

    // Drops one file from a set; removes the set once it's empty.
    function clearSlot(setIndex, slot) {
        if (setIndex < 0 || setIndex >= fileSets.length)
            return
        let sets = fileSets.slice()
        sets[setIndex][slot] = ""
        if (sets[setIndex].thb === "" && sets[setIndex].tbb === "" && sets[setIndex].tszip === "")
            sets.splice(setIndex, 1)
        fileSets = sets
        if (activeSetIndex >= fileSets.length)
            activeSetIndex = Math.max(0, fileSets.length - 1)
    }

    // Header "x": drop the whole set at once.
    function clearSet(setIndex) {
        if (setIndex < 0 || setIndex >= fileSets.length)
            return
        let sets = fileSets.slice()
        sets.splice(setIndex, 1)
        fileSets = sets
        if (activeSetIndex >= fileSets.length)
            activeSetIndex = Math.max(0, fileSets.length - 1)
    }

    function clearAllSets() {
        fileSets = []
        activeSetIndex = 0
    }

    // One dialog, multi-select - pick any of the 3 needed files
    // together or one at a time, each gets sorted into the right slot.
    FileDialog {
        id: browseFilesDialog
        title: qsTr("Locate files (.thb, .tbb, .tszip)")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Texture packer files (*.thb *.tbb *.tszip)"), qsTr("All files (*)")]
        onAccepted: {
            for (let i = 0; i < selectedFiles.length; i++)
                texturePackerWindow.assignFile(texturePackerWindow.toLocalPath(selectedFiles[i]))
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 16

        Label {
            text: qsTr("Texture Packer")
            font.pixelSize: 26
            font.bold: true
            color: texturePackerWindow.textColor
        }

        Label {
            text: qsTr("Unpack .thb, .tbb and .tstream files, or repack modified textures back.")
            font.pixelSize: 13
            color: texturePackerWindow.textColor
            opacity: 0.7
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        // -- Status banner -------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 32
            color: "#141414"
            radius: 4

            Label {
                anchors.centerIn: parent
                // One set: the familiar "2 of 3 files loaded." Several:
                // how many are ready to run out of how many were dropped.
                text: {
                    const n = texturePackerWindow.fileSets.length
                    if (n === 0)
                        return qsTr("No files loaded.")
                    if (n === 1) {
                        return qsTr("%1 of %2 files loaded.")
                            .arg(Math.min(texturePackerWindow.filesLoadedCount, texturePackerWindow.requiredFileCount))
                            .arg(texturePackerWindow.requiredFileCount) +
                            (!texturePackerWindow.hasTstream && texturePackerWindow.tstreamZipPath === ""
                                ? qsTr("  (.tszip not needed)") : "")
                    }
                    return qsTr("%1 of %2 sets ready.").arg(texturePackerWindow.readySetCount).arg(n) +
                        (texturePackerWindow.readySetCount < n ? qsTr("  (see the list for what's missing)") : "")
                }
                color: texturePackerWindow.textColor
                font.bold: true
            }
        }

        // -- Main area: file list (left) + platform list (right) -------
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            // -- Left: loaded-files list + Add/Clear buttons ------------
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 8

                TabBar {
                    id: fileTabBar
                    Layout.fillWidth: true
                    background: Rectangle { color: "transparent" }

                    TabButton {
                        text: qsTr("Drop Files")
                        contentItem: Text {
                            text: parent.text
                            color: fileTabBar.currentIndex === 0 ? "white" : "#888"
                            horizontalAlignment: Text.AlignHCenter
                        }
                        background: Rectangle {
                            color: fileTabBar.currentIndex === 0 ? texturePackerWindow.panelColor : "transparent"
                            radius: 4
                        }
                    }
                    TabButton {
                        text: qsTr("Texture Preview")
                        contentItem: Text {
                            text: parent.text
                            color: fileTabBar.currentIndex === 1 ? "white" : "#888"
                            horizontalAlignment: Text.AlignHCenter
                        }
                        background: Rectangle {
                            color: fileTabBar.currentIndex === 1 ? texturePackerWindow.panelColor : "transparent"
                            radius: 4
                        }
                        onClicked: unpackedTexturesBox.refreshDdsFiles()
                    }
                }

                Rectangle {
                    id: fileListBox
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 100
                    color: texturePackerWindow.panelColor
                    radius: 4
                    border.width: 1
                    border.color: "#333"
                    visible: fileTabBar.currentIndex === 0

                    // Selected row, as "<setIndex>:<slot>" -- lets the
                    // user clear one file and drag in a replacement
                    // without resetting everything.
                    property string selectedKey: ""

                    function clearSelected() {
                        if (fileListBox.selectedKey === "")
                            return
                        const parts = fileListBox.selectedKey.split(":")
                        texturePackerWindow.clearSlot(parseInt(parts[0]), parts[1])
                        fileListBox.selectedKey = ""
                    }

                    focus: true
                    Keys.onDeletePressed: fileListBox.clearSelected()
                    Keys.onEscapePressed: fileListBox.selectedKey = ""
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Backspace) {
                            fileListBox.clearSelected()
                            event.accepted = true
                        }
                    }

                    DropArea {
                        id: dropArea
                        anchors.fill: parent
                        onDropped: function(drop) {
                            if (!drop.hasUrls)
                                return
                            for (let i = 0; i < drop.urls.length; i++)
                                texturePackerWindow.assignFile(texturePackerWindow.toLocalPath(drop.urls[i]))
                            texturePackerWindow.logIncompleteSets()
                        }
                    }

                    // Click empty space to deselect, without stealing
                    // clicks meant for a row.
                    MouseArea {
                        anchors.fill: parent
                        z: -1
                        onClicked: {
                            fileListBox.selectedKey = ""
                            fileListBox.forceActiveFocus()
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: texturePackerWindow.fileSets.length === 0
                        text: qsTr("Drop .thb, .tbb and .tszip files here.")
                        horizontalAlignment: Text.AlignHCenter
                        color: "#666"
                    }

                    ScrollView {
                        id: setListScroll
                        anchors.fill: parent
                        anchors.margins: 8
                        clip: true
                        // Never scroll sideways: the rows should wrap to the
                        // pane's width, not stretch it.
                        contentWidth: availableWidth

                        ColumnLayout {
                            // Can't use parent.width here -- inside a
                            // ScrollView, "parent" is the flickable's
                            // content item, and its width comes from
                            // its content, not the viewport. Bind to
                            // this instead, or every row shrinks down
                            // to a few pixels wide.
                            width: setListScroll.availableWidth
                            spacing: 6

                            Repeater {
                                model: texturePackerWindow.fileSets
                                delegate: ColumnLayout {
                                    id: setBlock
                                    required property int index
                                    required property var modelData
                                    // Inner Repeater shadows `index`, so keep
                                    // the set's own index under its own name.
                                    readonly property int setIndex: index
                                    readonly property var setData: modelData
                                    Layout.fillWidth: true
                                    spacing: 2

                                    // Set header: name on the left, status on
                                    // the right, click to preview this set.
                                    //
                                    // Uses anchors instead of a RowLayout on
                                    // purpose: a Text always reports its full
                                    // text width as its layout minimum, so
                                    // packed into a row the pieces either
                                    // overlap or the name gets squashed down
                                    // to nothing. Anchoring just gives each
                                    // piece the space it needs instead.
                                    Rectangle {
                                        Layout.fillWidth: true
                                        implicitHeight: setNameLabel.implicitHeight + 8
                                        color: "transparent"
                                        clip: true

                                        MouseArea {
                                            anchors.fill: parent
                                            onClicked: {
                                                texturePackerWindow.activeSetIndex = setBlock.setIndex
                                                fileListBox.selectedKey = ""
                                                fileListBox.forceActiveFocus()
                                            }
                                        }

                                        Label {
                                            id: setNameLabel
                                            anchors.left: parent.left
                                            anchors.leftMargin: 4
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: setBlock.setData.name
                                            color: "white"
                                            font.bold: true
                                        }

                                        Label {
                                            id: setClearLabel
                                            anchors.right: parent.right
                                            anchors.rightMargin: 4
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: "\u2715"
                                            color: "#888"
                                            MouseArea {
                                                anchors.fill: parent
                                                anchors.margins: -4
                                                onClicked: texturePackerWindow.clearSet(setBlock.setIndex)
                                            }
                                        }

                                        Label {
                                            anchors.left: setNameLabel.right
                                            anchors.leftMargin: 8
                                            anchors.right: setClearLabel.left
                                            anchors.rightMargin: 8
                                            anchors.verticalCenter: parent.verticalCenter
                                            elide: Text.ElideRight
                                            text: texturePackerWindow.setIsComplete(setBlock.setData)
                                                ? "" : qsTr("missing %1")
                                                    .arg(texturePackerWindow.missingOf(setBlock.setData).join(", "))
                                            color: "#e0a030"
                                            font.pixelSize: 12
                                        }
                                    }

                                    Repeater {
                                        model: [
                                            { slot: "thb", path: setBlock.setData.thb },
                                            { slot: "tbb", path: setBlock.setData.tbb },
                                            { slot: "tszip", path: setBlock.setData.tszip }
                                        ]
                                        delegate: Rectangle {
                                            required property var modelData
                                            readonly property string rowKey: setBlock.setIndex + ":" + modelData.slot
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 12
                                            implicitHeight: rowLabel.implicitHeight + 8
                                            visible: modelData.path !== ""
                                            color: fileListBox.selectedKey === rowKey ? "#2a2a2a" : "transparent"
                                            radius: 3

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.leftMargin: 4
                                                anchors.rightMargin: 4
                                                spacing: 6

                                                Label {
                                                    id: rowLabel
                                                    Layout.fillWidth: true
                                                    text: texturePackerWindow.fileNameOf(modelData.path)
                                                    color: "white"
                                                }
                                                Label {
                                                    text: "\u2715"
                                                    color: "#888"
                                                    MouseArea {
                                                        anchors.fill: parent
                                                        anchors.margins: -4
                                                        onClicked: texturePackerWindow.clearSlot(setBlock.setIndex, modelData.slot)
                                                    }
                                                }
                                            }

                                            MouseArea {
                                                anchors.fill: parent
                                                z: -1
                                                onClicked: {
                                                    fileListBox.selectedKey = rowKey
                                                    fileListBox.forceActiveFocus()
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    id: unpackedTexturesBox
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 100
                    color: texturePackerWindow.panelColor
                    radius: 4
                    border.width: 1
                    border.color: "#333"
                    visible: fileTabBar.currentIndex === 1

                    // Which set's textures are on screen: an index into
                    // fileSets, or -1 for "All sets" (every set's folder
                    // concatenated). Set explicitly rather than bound
                    // through a function call, so it can't go stale.
                    property int previewIndex: 0
                    property bool previewAll: false
                    property string unpackedDir: ""
                    property var ddsFiles: []
                    // Bumped on every refresh and appended to each image's
                    // source URL below -- forces QML to actually re-fetch
                    // the image from the provider instead of assuming an
                    // unchanged-looking source means an unchanged picture.
                    property int reloadToken: 0

                    function refreshDdsFiles() {
                        const sets = texturePackerWindow.fileSets
                        let files = []
                        if (previewAll) {
                            for (let i = 0; i < sets.length; i++) {
                                const d = texturePackerWindow.outputDirFor(sets[i], "unpacked")
                                if (d !== "")
                                    files = files.concat(texPackerBridge.listDdsFiles(d))
                            }
                            unpackedTexturesBox.unpackedDir = sets.length
                                ? texturePackerWindow.dirOf(sets[0].thb) + "/unpacked" : ""
                        } else {
                            const idx = Math.min(previewIndex, sets.length - 1)
                            const d = idx >= 0 ? texturePackerWindow.outputDirFor(sets[idx], "unpacked") : ""
                            unpackedTexturesBox.unpackedDir = d
                            if (d !== "")
                                files = texPackerBridge.listDdsFiles(d)
                        }
                        unpackedTexturesBox.ddsFiles = files
                        unpackedTexturesBox.reloadToken++
                    }

                    // Re-read whenever the loaded sets change, the user picks
                    // another set in the list on the left, or a run finishes.
                    Connections {
                        target: texturePackerWindow
                        function onFileSetsChanged() {
                            if (unpackedTexturesBox.previewIndex >= texturePackerWindow.fileSets.length)
                                unpackedTexturesBox.previewIndex = 0
                            unpackedTexturesBox.refreshDdsFiles()
                        }
                        function onActiveSetIndexChanged() {
                            if (!unpackedTexturesBox.previewAll)
                                unpackedTexturesBox.previewIndex = texturePackerWindow.activeSetIndex
                            unpackedTexturesBox.refreshDdsFiles()
                        }
                    }
                    Component.onCompleted: refreshDdsFiles()

                    FolderWatcher {
                        id: unpackedFolderWatcher
                        path: unpackedTexturesBox.unpackedDir
                        onChanged: unpackedTexturesBox.refreshDdsFiles()
                    }

                    // Which set is being previewed. Hidden when only one
                    // set is loaded, so a single character looks unchanged.
                    RowLayout {
                        id: previewPicker
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.topMargin: 6
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 6
                        visible: texturePackerWindow.fileSets.length > 1

                        Label {
                            text: qsTr("Showing:")
                            color: texturePackerWindow.textColor
                            opacity: 0.7
                            font.pixelSize: 11
                        }
                        ComboBox {
                            id: previewCombo
                            Layout.fillWidth: true
                            Layout.maximumWidth: 220
                            font.pixelSize: 11
                            implicitHeight: 22
                            topPadding: 0
                            bottomPadding: 0
                            leftPadding: 8
                            rightPadding: 22
                            model: {
                                let names = [qsTr("All sets")]
                                for (let i = 0; i < texturePackerWindow.fileSets.length; i++)
                                    names.push(texturePackerWindow.fileSets[i].name)
                                return names
                            }
                            // index 0 = "All sets", 1.. = fileSets[index - 1]
                            currentIndex: unpackedTexturesBox.previewAll
                                ? 0 : unpackedTexturesBox.previewIndex + 1
                            onActivated: function(index) {
                                unpackedTexturesBox.previewAll = (index === 0)
                                if (index > 0) {
                                    unpackedTexturesBox.previewIndex = index - 1
                                    texturePackerWindow.activeSetIndex = index - 1
                                }
                                unpackedTexturesBox.refreshDdsFiles()
                            }

                            contentItem: Text {
                                text: previewCombo.displayText
                                color: "white"
                                font: previewCombo.font
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }
                            background: Rectangle {
                                radius: 3
                                color: previewCombo.pressed ? "#202020" : "#141414"
                                border.width: 1
                                border.color: "#333"
                            }
                            indicator: Text {
                                x: previewCombo.width - width - 7
                                y: (previewCombo.height - height) / 2
                                text: "\u25BE"
                                color: "#888"
                                font.pixelSize: 9
                            }
                            delegate: ItemDelegate {
                                required property int index
                                required property var modelData
                                width: previewCombo.width
                                height: 22
                                padding: 0
                                contentItem: Text {
                                    text: modelData
                                    color: "white"
                                    font.pixelSize: 11
                                    leftPadding: 8
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                }
                                background: Rectangle {
                                    color: previewCombo.highlightedIndex === index ? "#2a2a2a" : "#0d0d0d"
                                }
                            }
                            popup: Popup {
                                y: previewCombo.height
                                width: previewCombo.width
                                implicitHeight: Math.min(contentItem.implicitHeight, 220)
                                padding: 1
                                contentItem: ListView {
                                    clip: true
                                    implicitHeight: contentHeight
                                    model: previewCombo.popup.visible ? previewCombo.delegateModel : null
                                    currentIndex: previewCombo.highlightedIndex
                                }
                                background: Rectangle {
                                    color: "#0d0d0d"
                                    border.width: 1
                                    border.color: "#333"
                                    radius: 3
                                }
                            }
                        }
                        Label {
                            text: qsTr("%1 file(s)").arg(unpackedTexturesBox.ddsFiles.length)
                            color: texturePackerWindow.textColor
                            opacity: 0.7
                            font.pixelSize: 11
                        }
                        Item { Layout.fillWidth: true }
                    }

                    GridView {
                        id: thumbGrid
                        anchors.top: previewPicker.visible ? previewPicker.bottom : parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: 8
                        anchors.topMargin: previewPicker.visible ? 6 : 8
                        clip: true
                        cellWidth: 128
                        cellHeight: 148
                        model: unpackedTexturesBox.ddsFiles
                        delegate: Item {
                            width: 128
                            height: 148
                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 4

                                Rectangle {
                                    Layout.preferredWidth: 112
                                    Layout.preferredHeight: 112
                                    Layout.alignment: Qt.AlignHCenter
                                    color: "black"
                                    border.width: 1
                                    border.color: "#444"

                                    Image {
                                        anchors.fill: parent
                                        anchors.margins: 2
                                        source: "image://dds/" + encodeURIComponent(modelData) +
                                                "?t=" + unpackedTexturesBox.reloadToken
                                        fillMode: Image.PreserveAspectFit
                                        asynchronous: true
                                        cache: false
                                    }
                                }
                                Label {
                                    Layout.preferredWidth: 120
                                    Layout.alignment: Qt.AlignHCenter
                                    text: texturePackerWindow.fileNameOf(modelData)
                                    color: "white"
                                    font.pixelSize: 10
                                    elide: Text.ElideMiddle
                                    horizontalAlignment: Text.AlignHCenter
                                }
                            }
                        }
                    }

                    Label {
                        visible: unpackedTexturesBox.ddsFiles.length === 0
                        anchors.centerIn: parent
                        text: texturePackerWindow.fileSets.length === 0
                            ? qsTr("No unpacked textures yet.")
                            : qsTr("No .dds files in\n%1").arg(unpackedTexturesBox.unpackedDir)
                        horizontalAlignment: Text.AlignHCenter
                        color: texturePackerWindow.textColor
                        opacity: 0.6
                    }
                }
            }

            // -- Right: platform list ------------------------------------
            ColumnLayout {
                spacing: 4
                Layout.alignment: Qt.AlignTop

                Label { text: qsTr("Platform:"); color: texturePackerWindow.textColor }

                ButtonGroup { id: platformGroup }

                RadioButton {
                    id: ps3Radio
                    text: qsTr("PS3")
                    checked: texturePackerWindow.selectedPlatform === "PS3"
                    ButtonGroup.group: platformGroup
                    onCheckedChanged: if (checked) texturePackerWindow.selectedPlatform = "PS3"
                    indicator: Rectangle {
                        implicitWidth: 18
                        implicitHeight: 18
                        radius: 4
                        x: ps3Radio.leftPadding
                        y: ps3Radio.topPadding + (ps3Radio.availableHeight - height) / 2
                        color: "black"
                        border.width: 2
                        border.color: ps3Radio.checked ? "#82d478" : "#555"

                        Text {
                            anchors.centerIn: parent
                            text: "\u2713"
                            color: "#82d478"
                            font.pixelSize: 13
                            font.bold: true
                            visible: ps3Radio.checked
                        }
                    }
                }
                RadioButton {
                    id: xboxRadio
                    text: qsTr("Xbox 360")
                    checked: texturePackerWindow.selectedPlatform === "X360"
                    ButtonGroup.group: platformGroup
                    onCheckedChanged: if (checked) texturePackerWindow.selectedPlatform = "X360"
                    indicator: Rectangle {
                        implicitWidth: 18
                        implicitHeight: 18
                        radius: 4
                        x: xboxRadio.leftPadding
                        y: xboxRadio.topPadding + (xboxRadio.availableHeight - height) / 2
                        color: "black"
                        border.width: 2
                        border.color: xboxRadio.checked ? "#82d478" : "#555"

                        Text {
                            anchors.centerIn: parent
                            text: "\u2713"
                            color: "#82d478"
                            font.pixelSize: 13
                            font.bold: true
                            visible: xboxRadio.checked
                        }
                    }
                }
                RadioButton {
                    id: wiiRadio
                    text: qsTr("Wii")
                    checked: texturePackerWindow.selectedPlatform === "Wii"
                    ButtonGroup.group: platformGroup
                    onCheckedChanged: if (checked) texturePackerWindow.selectedPlatform = "Wii"
                    indicator: Rectangle {
                        implicitWidth: 18
                        implicitHeight: 18
                        radius: 4
                        x: wiiRadio.leftPadding
                        y: wiiRadio.topPadding + (wiiRadio.availableHeight - height) / 2
                        color: "black"
                        border.width: 2
                        border.color: wiiRadio.checked ? "#82d478" : "#555"

                        Text {
                            anchors.centerIn: parent
                            text: "\u2713"
                            color: "#82d478"
                            font.pixelSize: 13
                            font.bold: true
                            visible: wiiRadio.checked
                        }
                    }
                }

                Item { implicitHeight: 10 }

                Label {
                    text: qsTr("Has .tszip:")
                    color: texturePackerWindow.textColor
                    Layout.alignment: Qt.AlignHCenter
                }

                CheckBox {
                    id: hasTstreamCheck
                    Layout.alignment: Qt.AlignHCenter
                    // Box only -- the "Has .tstream:" label above already
                    // says what it means, so a "Yes" beside it is noise.
                    text: ""
                    padding: 0
                    checked: texturePackerWindow.hasTstream
                    onCheckedChanged: texturePackerWindow.hasTstream = checked
                    indicator: Rectangle {
                        implicitWidth: 18
                        implicitHeight: 18
                        radius: 4
                        x: hasTstreamCheck.leftPadding
                        y: hasTstreamCheck.topPadding + (hasTstreamCheck.availableHeight - height) / 2
                        color: "black"
                        border.width: 2
                        border.color: hasTstreamCheck.checked ? "#82d478" : "#555"

                        Text {
                            anchors.centerIn: parent
                            text: "\u2713"
                            color: "#82d478"
                            font.pixelSize: 13
                            font.bold: true
                            visible: hasTstreamCheck.checked
                        }
                    }
                }
            }
        }

        // -- Actions -------------------------------------------------------
        RowLayout {
            spacing: 12

            Button {
                id: unpackButton
                text: qsTr("Unpack")
                Layout.fillWidth: true
                implicitHeight: 55
                enabled: texturePackerWindow.allFilesSelected
                hoverEnabled: true
                opacity: enabled ? 1.0 : 1.0
                contentItem: Text {
                    text: unpackButton.text
                    color: "white"
                    font.pixelSize: 17
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    id: unpackOuterRect
                    radius: 10
                    gradient: Gradient {
                        orientation: Gradient.Vertical
                        GradientStop { position: 0.0; color: "#82d478" }
                        GradientStop { position: 1.0; color: "#3c6e36" }
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        radius: unpackOuterRect.radius - 2
                        color: unpackButton.pressed ? "#202020" : (unpackButton.hovered ? "#141414" : "black")
                    }
                }
                // Routes to the correct platform's backend based on
                // texturePackerWindow.selectedPlatform. Each platform
                // gets its own bridge call here as it's implemented --
                // never call the wrong platform's backend on a file,
                // since the .thb/.tbb layout differs per platform.
                onClicked: {
                    texturePackerWindow.logVisible = true
                    const sets = texturePackerWindow.fileSets

                    // Belt and braces: the button is disabled unless every
                    // set is complete, but say which are short if it ever
                    // gets clicked anyway.
                    if (texturePackerWindow.readySetCount < sets.length) {
                        logArea.append(qsTr("Nothing unpacked - %1 of %2 set(s) are incomplete:")
                            .arg(sets.length - texturePackerWindow.readySetCount).arg(sets.length))
                        texturePackerWindow.logIncompleteSets()
                        return
                    }

                    for (let i = 0; i < sets.length; i++) {
                        const set = sets[i]
                        const outDir = texturePackerWindow.outputDirFor(set, "unpacked")
                        if (sets.length > 1)
                            logArea.append(qsTr("--- %1 (%2 of %3) ---").arg(set.name).arg(i + 1).arg(sets.length))

                        let result
                        switch (texturePackerWindow.selectedPlatform) {
                        case "PS3":
                            result = texPackerBridge.unpackPs3(
                                set.thb, set.tbb, outDir, set.tszip)
                            break
                        case "X360":
                            result = texPackerBridge.unpackX360(
                                set.thb, set.tbb, outDir, set.tszip, true)
                            break
                        case "Wii":
                            result = texPackerBridge.unpackWii(
                                set.thb, set.tbb, outDir, set.tszip)
                            break
                        default:
                            result = qsTr("Unrecognized platform selected.")
                        }
                        logArea.append(result)
                    }
                    unpackedTexturesBox.refreshDdsFiles()
                }
            }

            Button {
                id: repackButton
                text: qsTr("Repack")
                Layout.fillWidth: true
                implicitHeight: 55
                enabled: texturePackerWindow.allFilesSelected
                hoverEnabled: true
                opacity: enabled ? 1.0 : 1.0
                contentItem: Text {
                    text: repackButton.text
                    color: "white"
                    font.pixelSize: 17
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    id: repackOuterRect
                    radius: 10
                    gradient: Gradient {
                        orientation: Gradient.Vertical
                        GradientStop { position: 0.0; color: "#82d478" }
                        GradientStop { position: 1.0; color: "#3c6e36" }
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        radius: repackOuterRect.radius - 2
                        color: repackButton.pressed ? "#202020" : (repackButton.hovered ? "#141414" : "black")
                    }
                }
                // Same routing pattern as Unpack above -- add a real
                // case here for each platform's repack backend as it's
                // built, keyed on selectedPlatform.
                onClicked: {
                    texturePackerWindow.logVisible = true
                    const sets = texturePackerWindow.fileSets

                    if (texturePackerWindow.readySetCount < sets.length) {
                        logArea.append(qsTr("Nothing repacked - %1 of %2 set(s) are incomplete:")
                            .arg(sets.length - texturePackerWindow.readySetCount).arg(sets.length))
                        texturePackerWindow.logIncompleteSets()
                        return
                    }

                    let updated = sets.slice()
                    for (let i = 0; i < sets.length; i++) {
                        const set = sets[i]
                        const inputDir = texturePackerWindow.outputDirFor(set, "unpacked")
                        const outDir = texturePackerWindow.outputDirFor(set, "repacked")
                        const prefix = set.name
                        if (sets.length > 1)
                            logArea.append(qsTr("--- %1 (%2 of %3) ---").arg(set.name).arg(i + 1).arg(sets.length))

                        let result
                        switch (texturePackerWindow.selectedPlatform) {
                        case "PS3":
                            result = texPackerBridge.repackPs3(
                                set.thb, set.tbb, inputDir, outDir, set.tszip)
                            break
                        case "X360":
                            result = texPackerBridge.repackX360(
                                set.thb, set.tbb, inputDir, outDir, set.tszip)
                            break
                        case "Wii":
                            result = texPackerBridge.repackWii(
                                set.thb, set.tbb, inputDir, outDir, set.tszip)
                            break
                        default:
                            result = qsTr("Unrecognized platform selected.")
                        }
                        logArea.append(result)

                        // Repack writes to <original dir>/repacked, but the
                        // set still points at the original input -- left
                        // alone, clicking Unpack next just re-unpacks the
                        // untouched original, which looks exactly like the
                        // repack was silently discarded. Point each set at
                        // its fresh output so Unpack verifies what was
                        // actually just written.
                        if (result.indexOf("ERROR:") === -1) {
                            updated[i] = {
                                name: set.name,
                                thb: outDir + "/" + prefix + ".thb",
                                tbb: outDir + "/" + prefix + ".tbb",
                                tszip: set.tszip === "" ? ""
                                    : ((texturePackerWindow.selectedPlatform === "PS3")
                                        ? outDir + "/" + prefix.toUpperCase() + ".TSZIP"
                                        : outDir + "/" + prefix + ".tszip")
                            }
                            logArea.append(qsTr("Now pointing at the repacked files in %1 -- click Unpack to verify them.").arg(outDir))
                        }
                    }
                    texturePackerWindow.fileSets = updated
                    unpackedTexturesBox.refreshDdsFiles()
                }
            }
        }

        // -- Log output ------------------------------------------------
        Label {
            text: qsTr("Log")
            color: texturePackerWindow.textColor
            font.bold: true
            visible: texturePackerWindow.logVisible
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 100
            color: texturePackerWindow.panelColor
            radius: 6
            border.color: "#333"
            visible: texturePackerWindow.logVisible

            ScrollView {
                anchors.fill: parent
                anchors.margins: 8
                TextArea {
                    id: logArea
                    readOnly: true
                    wrapMode: TextArea.Wrap
                    color: "white"
                    background: null
                    text: qsTr("Ready.")
                }
            }
        }
    }
}
