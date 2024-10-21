# SynQt

## Concept

### Server
Server.qml
```qml
import SynQt
import QtQuick
import QtQuick.Controls

Server {
    id: server

    property ListModel todoList

    function addTodo(content: string) {
        server.todoList.append({
                todo: content
            });
    }

    function removeTodo(index) {
        if (index >= 0 && index < todoList.length) {
            server.todoList.remove(index, 1);
        }
    }

    // Default: [{path: "/", component: Page{}}]
    routes: [{
            path: "/",
            component: HomeComponent
        }, {
            path: "/todos",
            component: TodoComponent,
            scope: Server.User
        }, {
            path: "/auth",
            component: AuthComponent
        }, {
            path: "/admin",
            component: AdminComponent,
            scope: Server.Administrator,
            private: true
        }]

    // This is default
    scopes: {
        "": Server.Unauthorized,
        "user": Server.User,
        "mod": Server.Operator,
        "admin": Server.Administrator
    }

    onRequestComponent: (request) => {
        const route = Server.routes[request.path];
        if (!(Client.scope in route.scope)) {
            request.denied = true;
            return;
        }
        if (route.private) {
            request.item = Qt.createComponent(route.path);
        }
        request.denied = false;
    }


    Authentication {
        id: authentication

        property var emails: {
            "user": ["user@email.com", "another@email.com"],
            "admin": ["hello@iamki.dev"]
        }

        protocol: "OAuth2.0"

        providers: [
            Github {
                callback_url: "https://synqt.org/auth"
                client_id: "theclientidstring"
                client_secret: Server.env.GITHUB_CLIENT_SECRET
                home_url: "https://synqt.org"
            }
        ]

        // This is default
        onAuthentication: (user, scope) => {
            if (user.email in Server.authentication.emails[scope]) {
                Client.setScope(Server.scopes[scope]);
                return true;
            }
            Client.setScope(Server.Unauthorized);
            return false;
        }
    }
}

```
The component `AdminComponent` being flagged private, it will be shared by the server only to authorized users.
The other components, `HomeComponent`, `TodoComponent`, `AuthComponent`, will already be client side.
Note this does NOT mean that their contents is shared entirely with the client. It just means their structure is public.

### Client
Client.qml
```qml
import SynQt

Client {
    id: client

    //readonly property Item currentPage
    //readonly property routes: Server.routes

    onRequest: route => {
        if (Client.scope in route.scope) {
            if (route.private) {
                client.currentPage = Server.getPrivateComponent(route);
            } else {
                client.currentPage = Client.getComponent(route);
            }
        }
    }

    Loader {
        id: currentPage

        source: client.page
    }
}
```

TodoComponent.qml
```qml
import SynQt
import QtQuick
import QtQuick.Controls

Page {
    height: 400
    title: "TODO App"
    width: 300

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10

        TextField {
            id: newTodoInput

            Layout.fillWidth: true
            placeholderText: "Enter new todo"
        }

        Button {
            Layout.fillWidth: true
            text: "Add Todo"

            onClicked: {
                if (newTodoInput.text.trim() !== "") {
                    Server.addTodo(newTodoInput.text.trim());
                    newTodoInput.text = "";
                }
            }
        }

        ListView {
            id: todoListView

            Layout.fillHeight: true
            Layout.fillWidth: true
            model: Server.todoList

            delegate: RowLayout {
                width: parent.width

                Label {
                    Layout.fillWidth: true
                    text: model.todo
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
