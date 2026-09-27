# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt design``: the designer, served to a browser on this machine only.

Every page in the browser can reach a localhost port, so the guard is about telling this
editor's requests apart:

* the socket is bound to the loopback address;
* every ``/api`` request carries a per-run token, compared in constant time. It travels in
  the URL fragment, which browsers never send, so the page can read it and a request for the
  page cannot carry it. The shell is served without it and holds nothing about the project;
* the browser this command opens is handed neither: a command line is readable by every
  local user, so it gets the path of a file only this user can read, which sends it to the
  editor with a launch code. The page trades the code for the token once, and the file is
  deleted;
* an ``Origin`` or ``Referer`` naming another page is refused;
* a ``Host`` that is not the bound loopback address is refused, which stops DNS rebinding.

Nothing is written until the editor applies a plan by digest, after showing its diff. A
design `synqt check` refuses is refused here too.
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

from . import appmodel
from . import check as checkmod
from . import config as configmod
from . import designdoc, designplan, infer, typebackend

TOKEN_HEADER = "X-SynQt-Token"

ASSETS = Path(__file__).resolve().parent / "assets" / "design"

# The names a browser on this machine can call this server.
LOOPBACK_HOSTS = frozenset({"127.0.0.1", "localhost", "::1", "[::1]"})

# A cap on the request body. A design document is kilobytes.
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

# The editor loads nothing from elsewhere, so the policy allows nothing else: no inline
# script, no framing, no form destination. Without `unsafe-inline` in `style-src`,
# CodeMirror runs in a shadow root, where its stylesheet is a constructed CSSStyleSheet.
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
    """What `synqt check` makes of a document, without writing: the topology rules only, for
    drawing while editing.
    """
    document = _document(body)
    base = configmod.load(server.project_dir, profile=server.profile)
    ok, findings = checkmod.validate(designdoc.to_config(document, base=base),
                                     project_dir=server.project_dir)
    return {"ok": ok, "findings": findings}


def _infer(server: "_DesignServer", _body: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """The contracts the project QML implies, as a document to draw (the `synqt infer`
    reading). Nothing is written until a change set is applied.
    """
    config = configmod.load(server.project_dir, profile=server.profile)
    backend = typebackend.resolve("auto", server.project_dir)
    try:
        edges = infer.collect(server.project_dir, config, backend=backend)
    except infer.InferError as error:
        raise _Refused(HTTPStatus.BAD_REQUEST, str(error)) from error
    document = infer.to_document(edges, config)
    # Applying names the configuration the document was read from.
    document["sourceHash"] = designdoc.source_hash(server.project_dir)
    return {"document": document, "typedBy": typebackend.name_of(backend)}


def _plan(server: "_DesignServer", body: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """The whole change set a document implies, with the digest that names it."""
    plan = _computed(server, _document(body))
    return _plan_json(plan)


def _apply(server: "_DesignServer", body: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """Apply the change set that was shown, and answer with the project as it now is.

    The plan is recomputed here. A digest that no longer matches means the change set
    changed, so nothing is written.
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
                 profile: Optional[str], launch_code: Optional[str]) -> None:
        super().__init__(address, _Handler)
        self.project_dir = project_dir
        self.token = token
        self.profile = profile
        # Traded once for the token, then gone; the file that carried it goes with it.
        self.launch_code = launch_code
        self.launch_file: Optional[Path] = None
        # One project on one disk: requests are served one at a time.
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
        """Log refused requests only."""
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
            if (method, path) == ("POST", "/api/launch"):
                with self.server.lock:
                    self._send_json(HTTPStatus.OK, self._launch(self._body(method)))
                return
            # Authorized before the body is read.
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
            # Answered rather than raised, so the page is not left waiting.
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
            # An unread body would desynchronise the next request on the connection.
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

    def _launch(self, body: Optional[Dict[str, Any]]) -> Dict[str, Any]:
        """Trade the launch code for the token, once."""
        code = body.get("code") if isinstance(body, dict) else None
        expected = self.server.launch_code
        if (not isinstance(code, str) or expected is None
                or not hmac.compare_digest(code, expected)):
            raise _Refused(HTTPStatus.FORBIDDEN,
                           "this launch code opens nothing: it has been used, or it is not "
                           "this editor's. Open the URL 'synqt design' printed instead")
        self.server.launch_code = None
        _remove(self.server.launch_file)
        return {"token": self.server.token}

    def _check_token(self) -> None:
        token = self.headers.get(TOKEN_HEADER) or ""
        if not hmac.compare_digest(token, self.server.token):
            raise _Refused(HTTPStatus.FORBIDDEN,
                           f"this request carries no {TOKEN_HEADER} for this editor. It is "
                           "in the fragment of the URL 'synqt design' printed, and only a "
                           "page opened at that URL has it")

    def _check_host(self) -> None:
        """Refuse a request that calls this server by a name somebody else controls (DNS
        rebinding).
        """
        host = self.headers.get("Host")
        if host is None:
            return                       # HTTP/1.0 without one. There is nothing to check
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
    """The file `path` names inside the assets directory, or None. Resolved, then checked
    against the directory.
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
    """Whether something already listens on that loopback port.

    Asked by connecting, because with `SO_REUSEADDR` (set by `HTTPServer`) a second bind
    succeeds on Windows.
    """
    if port == 0:
        return False  # the operating system is picking a free one
    with socket.socket() as probe:
        probe.settimeout(0.25)
        return probe.connect_ex(("127.0.0.1", port)) == 0


def _remove(path: Optional[Path]) -> None:
    if path is not None:
        try:
            path.unlink()
        except FileNotFoundError:
            pass


def _write_launch_file(httpd: _DesignServer) -> Path:
    """A page only this user can read, which sends the browser to the editor with the launch
    code. It lives in the project's generated/ directory, which a browser confined to the
    user's files can still open.
    """
    directory = httpd.project_dir / appmodel.GENERATED_DIR
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"design-{secrets.token_hex(8)}.html"
    target = f"http://127.0.0.1:{httpd.server_port}/#launch={httpd.launch_code}"
    page = ("<!doctype html><meta charset=\"utf-8\"><title>synqt design</title>"
            f"<meta http-equiv=\"refresh\" content=\"0;url={target}\">"
            f"<p><a href=\"{target}\">Open the editor</a></p>\n")
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
        handle.write(page)
    return path


def make_server(project_dir: os.PathLike[str] | str, *, port: int, token: str,
                profile: Optional[str] = None,
                launch_code: Optional[str] = None) -> ThreadingHTTPServer:
    """A server for one project, bound to loopback and not yet serving. `port` 0 lets the OS
    pick; the bound port is ``server_port``. `launch_code`, when given, is traded once for
    the token at ``/api/launch``.
    """
    root = Path(project_dir).resolve()
    if not (root / "synqt.yaml").is_file():
        raise DesignError(f"{root} is not a SynQt project (no synqt.yaml)")
    if not token:
        raise DesignError("a design server needs a token")
    if _already_answering(port):
        raise DesignError(f"cannot serve the editor on port {port}: something is already "
                          "listening there. Pass --port to pick another one.")
    try:
        return _DesignServer(("127.0.0.1", port), root, token, profile, launch_code)
    except OSError as error:
        raise DesignError(f"cannot serve the editor on port {port}: {error}") from error


def url_for(port: int, token: str) -> str:
    """The URL that opens the editor, with the token where a browser will not send it."""
    return f"http://127.0.0.1:{port}/#token={token}"


def serve(project_dir: os.PathLike[str] | str, *, port: int = 8181,
          open_browser: bool = True, profile: Optional[str] = None) -> str:
    """Serve the editor until interrupted, and open it in a browser. The token is minted per
    run and printed only here.
    """
    httpd = make_server(project_dir, port=port, token=secrets.token_urlsafe(24),
                        profile=profile,
                        launch_code=secrets.token_urlsafe(24) if open_browser else None)
    address = url_for(httpd.server_port, httpd.token)
    print(f"synqt design: editing {Path(project_dir).resolve()}")
    print(f"  {address}")
    print("  This machine only, and only from that URL: the token in it was minted for "
          "this run and is not in any log.")
    print("  Nothing is written until you have read a change set and applied it. Press "
          "Ctrl-C to stop.")
    if open_browser:
        httpd.launch_file = _write_launch_file(httpd)
        webbrowser.open(httpd.launch_file.as_uri())
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.shutdown()
        httpd.server_close()
        _remove(httpd.launch_file)
    return "synqt design: stopped."
