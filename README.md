# SynQt

## Concept

This is a server/client web framework written in C++ with Qt, allowing developers to create complex client/server web application using only QML. On deploy, the QML is compiled by [Qt Quick Compiler](https://doc.qt.io/qt-6/qtqml-qtquick-compiler-tech.html) and then turned into [Wasm](https://doc.qt.io/qt-6/wasm.html). The server runs the contents of `server/` on private server instances(s). The client, upon connection, is delivered the contents of `client/`, and can communicate with the server. SynQt uses [Qt Remote Objects](https://doc.qt.io/qt-6/qtremoteobjects-index.html) and the features of QML to make available the attached property `Server` to the client, and the attached property `Client` to the server, as easily as if both were just instances of QObject locally. Both can bind to signals, events, function calls, property changes... of the other. Also, the server and the client can also reference themeselves using `Server` and `Client` respectively.
SynQt makes the development of any web application as easy as the creation of a QML application.

## Demo: A TODO application with authentication
This is all the code required to write a TODO application that only allows some users to post. The items in the TODO list are updated in real time without any refresh required.

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
        if (!(Server.User in Client.scope)) {
            return;
        }
        server.todoList.append({
                todo: content,
                author: Client.email
        });
    }

    function removeTodo(index) {
        if (!(Server.User in Client.scope)) {
            return;
        }
        if (index >= 0 && index < todoList.length && server.todoList[index].author == Client.email) {
            server.todoList.remove(index, 1);
        }
    }

    // Default: [{path: "/", component: Page{}}]
    routes: [{
            path: "/",
            component: "TodoComponent.qml"
        }, {
            path: "/todos",
            component: "TodoComponent.qml",
            scope: Server.User
        }, {
            path: "/admin",
            component: Page {},
            scope: Server.Administrator,
            private: true
        }
    ]

    identity: OAuth2 {

        property var emails: {
            "user": ["user@email.com", "another@email.com"],
            "admin": ["hello@iamki.dev"]
        }

        providers: [
            Github {
                callback_url: "https://synqt.org/auth"
                client_id: "theclientidstring"
                client_secret: Server.env.GITHUB_CLIENT_SECRET
                home_url: "https://synqt.org"
            }
        ]

        onAuthentication: (user, scope) => {
            if (user.email in Server.identity.emails[scope]) {
                Client.setScope(Server.scopes[scope]);
                return;
            }
            // This is default
            Client.setScope(Server.Unauthorized);
        }
    }
}

```


### Client
Client.qml
```qml
import SynQt

Client {}
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

        Button {
            Layout.fillWidth: true
            text: "Sign In"

            onClicked: {
                Server.requestAuthentication()
            }
        }

        TextField {
            id: newTodoInput
            enabled: Server.User in Client.scope

            Layout.fillWidth: true
            placeholderText: "Enter new todo"
        }

        Button {
            Layout.fillWidth: true
            text: "Add Todo"
            enabled: Server.User in Client.scope

            onClicked: {
                if (newTodoInput.text.trim() !== "") {
                    Server.addTodo(newTodoInput.text.trim());
                    newTodoInput.text = "";
                }
            }
        }

        ListView {
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
                    enabled: Server.User in Client.scope && model[index].author === Client.email

                    onClicked: Server.removeTodo(index)
                }
            }
        }
    }
}
```

## Behind the scenes of the demo
Now the code of the application's Server.qml and Client.qml including the default values of some properties, and explainations to more easily understand the logic. Still very quick! TodoComponent.qml is identical.

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
        if (!(Server.User in Client.scope)) {
            return;
        }
        Server.todoList.append({
                todo: content,
                author: Client.email
            });
    }

    function removeTodo(index) {
        if (!(Server.User in Client.scope)) {
            return;
        }
        if (index >= 0 && index < todoList.length && Server.todoList[index].author == Client.email) {
            Server.todoList.remove(index, 1);
        }
    }

    // Default: [{path: "/", component: Page{}}]
    routes: [{
            path: "/",
            component: "TodoComponent.qml"
        }, {
            path: "/todos",
            component: "TodoComponent.qml",
            scope: Server.User
        }, {
            path: "/admin",
            component: Page {},
            scope: Server.Administrator,
            private: true
        }
    ]

    // This is default
    scopes: {
        "": Server.Unauthorized,
        "user": Server.User,
        "mod": Server.Operator,
        "admin": Server.Administrator
    }

    // This is default
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


    identity: OAuth2 {

        property var emails: {
            "user": ["user@email.com", "another@email.com"],
            "admin": ["hello@iamki.dev"]
        }

        providers: [
            Github {
                callback_url: "https://synqt.org/auth"
                client_id: "theclientidstring"
                client_secret: Server.env.GITHUB_CLIENT_SECRET
                home_url: "https://synqt.org"
            }
        ]

        onAuthentication: (user, scope) => {
            if (user.email in Server.identity.emails[scope]) {
                Client.setScope(Server.scopes[scope]);
                return;
            }
            // This is default
            Client.setScope(Server.Unauthorized);
        }
    }
}

```
The phony (only here for the example) component `AdminComponent` being flagged private, it will be shared by the server only to authorized users.
The other component, `TodoComponent`, will already be client side.
Note this does NOT mean that their contents is shared entirely with the client. It just means their structure is public. Indeed, the `Server.todoList` requires an authorization and an answer from the server

### Client
Client.qml
```qml
import SynQt

Client {
    //readonly property Item serverComponent
    //readonly property string currentPage
    //readonly property routes: Server.routes

    // This is default
    onRequest: route => {
        if (Client.scope in route.scope) {
            if (route.private) {
                Client.serverComponent = Server.getPrivateComponent(route);
            } else {
                Client.currentPage = Client.routes[route].component;
            }
        }
    }

    // This is default
    Loader {
        id: currentPage

        source: Client.page
        sourceComponent: Client.serverComponent
    }
}
```
