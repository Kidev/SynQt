# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt design``: the visual editor, served to a browser on this machine only.

The editor is a page in a browser, and what it drives is a directory on the developer's
disk. Every other page in that browser can reach a localhost port too, so the whole of this
module's guard is about telling this editor's requests apart from anybody else's:

* the socket is bound to the loopback address, so nothing off the machine can connect;
* every ``/api`` request carries a token minted for this run, compared in constant time.
  It travels in the URL fragment, which no browser ever sends to a server and no proxy or
  history file ever records, so the page can read it while a request for the page cannot
  carry it. That is why the shell itself is served without one: it holds nothing about the
  project, and everything that does is behind ``/api``;
* a request that says where it came from has to say this editor. An ``Origin`` or a
  ``Referer`` naming another page is refused, which is what stops a page the developer has
  open in another tab from posting a design here;
* what the request calls this server is checked too. A name the attacker controls, pointed
  at 127.0.0.1, is how a page gets the browser to treat this server as its own origin and
  send nothing worth refusing; a ``Host`` that is not the loopback address this server
  bound is not this server.

Nothing is written until the editor asks for it by digest: it computes a plan, the page
shows the diff, and applying it names the plan that was shown. A design that ``synqt
check`` refuses is refused here, so the editor is not a way around the rules the command
line holds.
"""

from __future__ import annotations

import hmac
import json
import os
import secrets
import socket
import threading
import webbrowser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Dict, Optional, Tuple
from urllib.parse import unquote, urlparse

from . import check as checkmod
from . import config as configmod
from . import designdoc, designplan, infer, typebackend

TOKEN_HEADER = "X-SynQt-Token"

ASSETS = Path(__file__).resolve().parent / "assets" / "design"

# The names a browser on this machine can call this server. Anything else is either off the
# machine or a name somebody else controls.
LOOPBACK_HOSTS = frozenset({"127.0.0.1", "localhost", "::1", "[::1]"})

# A design document is a topology and its contracts: kilobytes, not megabytes. The cap is
# here so a request cannot make this process hold an arbitrary amount of memory.
MAX_BODY_BYTES = 4 * 1024 * 1024

_CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".woff2": "font/woff2",
}

# The editor loads nothing from anywhere else, so it is served under a policy that allows
# nothing else: no inline script, no framing, and no destination for a form.
_CSP = ("default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
        "font-src 'self'; connect-src 'self'; base-uri 'none'; form-action 'none'; "
        "frame-ancestors 'none'")


class DesignError(Exception):
    """A design-server error surfaced to the CLI (no traceback for the user)."""


class _Refused(Exception):
    """A request that will not be served, and the status and reason to answer with."""

    def __init__(self, status: int, reason: str) -> None:
        super().__init__(reason)
        self.status = status
        self.reason = reason


# The routes


def _project(server: "_DesignServer", _body: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """The project as a design document, with what `synqt check` makes of it as it is."""
    document = designdoc.read(server.project_dir, profile=server.profile)
    ok, findings = checkmod.check_project(server.project_dir, profile=server.profile)
    return {"document": document, "ok": ok, "findings": findings}


def _validate(server: "_DesignServer", body: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """What `synqt check` would make of a document, without writing any of it.

    The cheap answer, for the editor to draw while somebody is still moving things around:
    the topology rules only, and no walk of the disk to work out what the change set is.
    """
    document = _document(body)
    base = configmod.load(server.project_dir, profile=server.profile)
    ok, findings = checkmod.validate(designdoc.to_config(document, base=base),
                                     project_dir=server.project_dir)
    return {"ok": ok, "findings": findings}


def _infer(server: "_DesignServer", _body: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """The contracts the project's own QML already implies, as a document to draw.

    The same reading `synqt infer` prints, handed to the canvas instead of the terminal:
    a link nobody has written a contract for arrives with the members both ends of it
    already use, and the answer is a document like any other, so nothing is written until
    somebody has read a change set and applied it.
    """
    config = configmod.load(server.project_dir, profile=server.profile)
    backend = typebackend.resolve("auto", server.project_dir)
    try:
        edges = infer.collect(server.project_dir, config, backend=backend)
    except infer.InferError as error:
        raise _Refused(HTTPStatus.BAD_REQUEST, str(error)) from error
    document = infer.to_document(edges, config)
    # The document is drawn and then applied like any other, and applying names the
    # configuration it was read from. Without this the page would be holding a document
    # that says nothing about which synqt.yaml it describes.
    document["sourceHash"] = designdoc.source_hash(server.project_dir)
    return {"document": document, "typedBy": typebackend.name_of(backend)}


def _plan(server: "_DesignServer", body: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """The whole change set a document implies, with the digest that names it."""
    plan = _computed(server, _document(body))
    return _plan_json(plan)


def _apply(server: "_DesignServer", body: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """Apply the change set somebody has read, and answer with the project as it now is.

    The plan is worked out again here rather than kept from the last request: what is
    applied is what the project implies now, and the digest the editor sends is how it says
    which plan it showed. A digest that no longer matches means the answer to "what would
    this do" has changed since it was asked, so nothing is written.
    """
    document = _document(body)
    digest = body.get("digest") if isinstance(body, dict) else None
    if not isinstance(digest, str) or not digest:
        raise _Refused(HTTPStatus.BAD_REQUEST,
                       "no digest: apply the plan that was shown, by its digest")
    plan = _computed(server, document)
    if not hmac.compare_digest(digest, designplan.digest(plan)):
        raise _Refused(HTTPStatus.CONFLICT,
                       "this is no longer the change set that digest named; read the plan "
                       "again and have another look at what it would do")
    try:
        applied = designplan.execute(server.project_dir, plan)
    except designplan.DesignPlanError as error:
        raise _Refused(HTTPStatus.BAD_REQUEST, str(error)) from error
    designdoc.write_layout(server.project_dir, document)
    ok, findings = checkmod.check_project(server.project_dir, profile=server.profile)
    return {"applied": applied.splitlines(),
            "document": designdoc.read(server.project_dir, profile=server.profile),
            "ok": ok, "findings": findings}


ROUTES: Dict[Tuple[str, str], Callable[..., Dict[str, Any]]] = {
    ("GET", "/api/project"): _project,
    ("POST", "/api/infer"): _infer,
    ("POST", "/api/validate"): _validate,
    ("POST", "/api/plan"): _plan,
    ("POST", "/api/apply"): _apply,
}


def _document(body: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """The design document out of a request body, or a refusal naming what was missing."""
    document = body.get("document") if isinstance(body, dict) else None
    if not isinstance(document, dict):
        raise _Refused(HTTPStatus.BAD_REQUEST,
                       "no design document in the request body")
    for key in ("entities", "links"):
        if not isinstance(document.get(key, []), list):
            raise _Refused(HTTPStatus.BAD_REQUEST,
                           f"the design document's '{key}' is not a list")
    return document


def _computed(server: "_DesignServer", document: Dict[str, Any]) -> designplan.Plan:
    try:
        return designplan.compute(server.project_dir, document, profile=server.profile)
    except designplan.DesignPlanError as error:
        raise _Refused(HTTPStatus.BAD_REQUEST, str(error)) from error


def _plan_json(plan: designplan.Plan) -> Dict[str, Any]:
    return {
        "ok": plan.ok,
        "stale": plan.stale,
        "git": plan.git,
        "findings": list(plan.findings),
        "digest": designplan.digest(plan),
        "diff": designplan.diff(plan),
        "changes": [{"action": change.action, "path": change.path,
                     "reason": change.reason, "before": change.before,
                     "after": change.after}
                    for change in plan.changes],
    }


# The server


class _DesignServer(ThreadingHTTPServer):
    """The editor's server, and the one project it is allowed to touch."""

    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address: Tuple[str, int], project_dir: Path, token: str,
                 profile: Optional[str]) -> None:
        super().__init__(address, _Handler)
        self.project_dir = project_dir
        self.token = token
        self.profile = profile
        # One project on one disk: two requests writing it at once is not a case worth
        # having, so the routes take a turn each.
        self.lock = threading.Lock()


class _Handler(BaseHTTPRequestHandler):
    server_version = "SynQtDesign"
    sys_version = ""
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        self._serve("GET")

    def do_POST(self) -> None:
        self._serve("POST")

    def log_request(self, code: Any = "-", size: Any = "-") -> None:
        """Say nothing about a request that was served: one per redraw is not news.

        What was refused still goes to the terminal, because a developer whose editor has
        stopped answering is owed the reason on the screen they started it from.
        """
        status = str(getattr(code, "value", code))
        if status.startswith(("4", "5")):
            super().log_request(code, size)

    # answering

    def _serve(self, method: str) -> None:
        path = urlparse(self.path).path
        try:
            self._check_host()
            self._check_source()
            if not path.startswith("/api/"):
                if method != "GET":
                    raise _Refused(HTTPStatus.NOT_FOUND, f"no such route: {method} {path}")
                self._send_asset(path)
                return
            # Authorized before it is read: a request nobody has vouched for does not get
            # this process to hold its body in memory, or to say which routes exist.
            self._check_token()
            route = ROUTES.get((method, path))
            if route is None:
                raise _Refused(HTTPStatus.NOT_FOUND, f"no such route: {method} {path}")
            body = self._body(method)
            with self.server.lock:
                self._send_json(HTTPStatus.OK, route(self.server, body))
        except _Refused as refusal:
            self._send_json(refusal.status, {"error": refusal.reason})
        except Exception as error:
            # Answered rather than raised: an unhandled error here would leave the page
            # waiting on a connection that was closed without saying anything.
            self._send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": str(error)})

    def _body(self, method: str) -> Optional[Dict[str, Any]]:
        if method != "POST":
            return None
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError as error:
            raise _Refused(HTTPStatus.BAD_REQUEST, "a Content-Length that is "
                                                   "not a number") from error
        if length <= 0:
            raise _Refused(HTTPStatus.BAD_REQUEST, "an empty request body")
        if length > MAX_BODY_BYTES:
            raise _Refused(HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                           f"a request body over {MAX_BODY_BYTES} bytes")
        raw = self.rfile.read(length)
        try:
            return json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise _Refused(HTTPStatus.BAD_REQUEST,
                           f"a request body that is not JSON: {error}") from error

    def _send_json(self, status: int, payload: Dict[str, Any]) -> None:
        self._send(status, "application/json; charset=utf-8",
                   json.dumps(payload).encode("utf-8"))

    def _send(self, status: int, content_type: str, payload: bytes) -> None:
        if int(status) >= 400:
            # The body of a refused request may be unread, and a connection with bytes
            # left on it desynchronises the next request that reuses it.
            self.close_connection = True
        self.send_response(int(status))
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Content-Security-Policy", _CSP)
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(payload)

    def _send_asset(self, path: str) -> None:
        target = _asset_path(path)
        if target is None or not target.is_file():
            raise _Refused(HTTPStatus.NOT_FOUND, f"no such file: {path}")
        self._send(HTTPStatus.OK,
                   _CONTENT_TYPES.get(target.suffix, "application/octet-stream"),
                   target.read_bytes())

    # the guard

    def _check_token(self) -> None:
        token = self.headers.get(TOKEN_HEADER) or ""
        if not hmac.compare_digest(token, self.server.token):
            raise _Refused(HTTPStatus.FORBIDDEN,
                           f"this request carries no {TOKEN_HEADER} for this editor. It is "
                           "in the fragment of the URL 'synqt design' printed, and only a "
                           "page opened at that URL has it")

    def _check_host(self) -> None:
        """Refuse a request that calls this server by a name somebody else controls.

        The rebinding case: a name resolving to 127.0.0.1 makes the browser treat this
        server as the attacker's own origin, so it sends no Origin worth refusing. What it
        calls this server is the part that still gives it away.
        """
        host = self.headers.get("Host")
        if host is None:
            return                       # HTTP/1.0 without one; there is nothing to check
        if not _is_this_server(f"//{host}", self.server.server_port):
            raise _Refused(HTTPStatus.FORBIDDEN,
                           f"'{host}' is not this editor; it answers on 127.0.0.1 only")

    def _check_source(self) -> None:
        """Refuse a request that says it came from a page that is not this editor."""
        for header in ("Origin", "Referer"):
            value = self.headers.get(header)
            if value in (None, "", "null"):
                continue
            if not _is_this_server(value, self.server.server_port):
                raise _Refused(HTTPStatus.FORBIDDEN,
                               f"a request from {value} is not this editor's")


def _is_this_server(url: str, port: int) -> bool:
    """Whether `url` names the loopback address and the port this server bound."""
    parsed = urlparse(url)
    if parsed.scheme and parsed.scheme != "http":
        return False
    if (parsed.hostname or "") not in LOOPBACK_HOSTS:
        return False
    return parsed.port in (None, port)


def _asset_path(path: str) -> Optional[Path]:
    """The file `path` asks for inside the assets directory, or None if it is outside it.

    Resolved and then checked against the directory rather than filtered for '..': a path
    is only inside the directory if what it resolves to is, and that is the thing to ask.
    """
    relative = unquote(path).lstrip("/") or "index.html"
    if "\x00" in relative:
        return None
    target = (ASSETS / relative).resolve()
    root = ASSETS.resolve()
    if target != root and root not in target.parents:
        return None
    return target


# Serving


def _already_answering(port: int) -> bool:
    """Whether something is already listening on that loopback port.

    Asked rather than left to bind() because bind() does not give the same answer on every
    platform. `HTTPServer` sets `SO_REUSEADDR`, which on Windows means a second socket may
    take an address another socket already holds; there the bind succeeds, two servers split
    the port between them, and which one a request reaches is up to the operating system. A
    connection attempt is the same question on all three, and the answer arrives immediately
    on loopback whether it is accepted or refused.
    """
    if port == 0:
        return False  # the operating system is picking a free one
    with socket.socket() as probe:
        probe.settimeout(0.25)
        return probe.connect_ex(("127.0.0.1", port)) == 0


def make_server(project_dir: os.PathLike[str] | str, *, port: int, token: str,
                profile: Optional[str] = None) -> ThreadingHTTPServer:
    """A server for one project, bound to the loopback address and not yet serving.

    `port` 0 lets the operating system pick one, which is what the tests use; the port that
    was bound is on the returned server as ``server_port``.
    """
    root = Path(project_dir)
    if not (root / "synqt.yaml").is_file():
        raise DesignError(f"{root} is not a SynQt project (no synqt.yaml)")
    if not token:
        raise DesignError("a design server needs a token")
    if _already_answering(port):
        raise DesignError(f"cannot serve the editor on port {port}: something is already "
                          "listening there. Pass --port to pick another one.")
    try:
        return _DesignServer(("127.0.0.1", port), root, token, profile)
    except OSError as error:
        raise DesignError(f"cannot serve the editor on port {port}: {error}") from error


def url_for(port: int, token: str) -> str:
    """The URL that opens the editor, with the token where a browser will not send it."""
    return f"http://127.0.0.1:{port}/#token={token}"


def serve(project_dir: os.PathLike[str] | str, *, port: int = 8181,
          open_browser: bool = True, profile: Optional[str] = None) -> str:
    """Serve the editor until interrupted, and open it in a browser.

    The token is minted per run, so a URL from an earlier one is worth nothing, and it is
    printed only here: closing the tab means opening the printed URL again, not restarting.
    """
    httpd = make_server(project_dir, port=port, token=secrets.token_urlsafe(24),
                        profile=profile)
    address = url_for(httpd.server_port, httpd.token)
    print(f"synqt design: editing {Path(project_dir).resolve()}")
    print(f"  {address}")
    print("  This machine only, and only from that URL: the token in it was minted for "
          "this run and is not in any log.")
    print("  Nothing is written until you have read a change set and applied it. Press "
          "Ctrl-C to stop.")
    if open_browser:
        webbrowser.open(address)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.shutdown()
        httpd.server_close()
    return "synqt design: stopped."
