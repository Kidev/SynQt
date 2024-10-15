# SynQt

## Concept

#### Server
Server.qml
```qml
import SynQt
import QtQuick
import QtQuick.Controls

Item {
    id: server

    property ListModel todoList
    property var request

    function addTodo(content: string) {
        server.todoList.append({todo: content})
    }

    function removeTodo(index) {
        if (index >= 0 && index < todoList.length) {
            server.todoList.remove(index, 1)
        }
    }

    Route on request {
        query: "*"
        condition: SynQt.registered(request.user)
        ok: Todo {
            title: request.params[0]
        }
    }
}
```
Todo.qml
```qml
import SynQt
import QtQuick
import QtQuick.Controls

Window {
    width: 300
    height: 400

    required property string title

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10

        TextField {
            id: newTodoInput
            placeholderText: "Enter new todo"
            Layout.fillWidth: true
        }

        Button {
            text: "Add Todo"
            Layout.fillWidth: true
            onClicked: {
                if (newTodoInput.text.trim() !== "") {
                    Server.addTodo(newTodoInput.text.trim())
                    newTodoInput.text = ""
                }
            }
        }

        ListView {
            id: todoListView
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: Server.todoList
            delegate: RowLayout {
                width: parent.width
                Label {
                    text: model.todo
                    Layout.fillWidth: true
                }
                Button {
                    text: "Remove"
                    onClicked: Server.removeTodo(index)
                }
            }
        }
    }
}
```
Client.qml
```qml
import SynQt

Loader {
    source: Server.request("/");
}
```
