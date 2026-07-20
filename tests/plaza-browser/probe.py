# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Add a telemetry timer to a copy of examples/plaza/client/app/Main.qml.

The client draws to a canvas, so the browser test reads its state from the console. The
copy's window gets a timer that logs, four times a second, what the walker thinks, who else
it sees, and whether any kept snapshot is not a number.

    python3 probe.py <client/app/Main.qml>
"""

import sys
from pathlib import Path

PROBE = """
    // Added by tests/plaza-browser to its own copy. Not part of the example.
    Timer {
        interval: 250
        repeat: true
        running: true
        onTriggered: {
            const seen = [];
            for (let index = 0; index < others.count; ++index) {
                const entry = others.get(index);
                const pose = root.poseAt(entry.walkerId, root.renderNow);
                seen.push({"name": entry.name, "hue": entry.hue, "x": pose.x, "z": pose.z});
            }
            let notANumber = 0;
            for (const id in root.trails) {
                for (const sample of root.trails[id]) {
                    if (typeof sample.x !== "number" || typeof sample.z !== "number"
                            || isNaN(sample.x) || isNaN(sample.z)) {
                        ++notANumber;
                    }
                }
            }
            console.log("PLAZA " + JSON.stringify({
                "user": Session.hasScope("user"),
                "x": me.position.x, "z": me.position.z,
                "forward": root.forward, "side": root.side,
                "others": seen, "notANumber": notANumber
            }));
        }
    }
"""


def main(argv):
    path = Path(argv[1])
    text = path.read_text(encoding="utf-8").rstrip()
    if not text.endswith("}"):
        raise SystemExit(f"{path}: the window does not end where it should")
    path.write_text(text[:-1].rstrip() + "\n" + PROBE + "}\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
