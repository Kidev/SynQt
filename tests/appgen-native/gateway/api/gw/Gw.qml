// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

pragma Singleton

import QtQuick

// The gateway entity itself. Its whole public surface is declared here, on `Api`, and the
// only reason these routes answer at all is that synqt.yaml gave this entity a
// `network.inbound` block. Who may call them was settled there too.
QtObject {
    // Proves the allowlist reached the runtime. The first URL is under the one prefix
    // network.outbound names, the second is not, and only one of them is sent.
    function probeOutbound() {
        Http.get("https://api.example.com/v1/ping").then(() => {}, message => {
            console.log("allowed:", message);
        });
        Http.get("https://elsewhere.example/steal").then(() => {}, message => {
            console.log("refused:", message);
        });
    }

    Component.onCompleted: {
        Api.get("/health", () => {
            return {
                ok: true
            };
        });

        Api.post("/echo/:id", request => {
            if (!request.body || !request.body.value) {
                request.fail(422, "value is required");
                return;
            }
            return {
                id: request.params.id,
                value: request.body.value
            };
        });

        probeOutbound();
    }
}
