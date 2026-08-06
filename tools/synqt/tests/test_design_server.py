# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The local editor's API, and the guard on it.

Any page in any browser can post to a localhost port, so these are the tests that say a
random tab cannot drive someone's filesystem.
"""

from __future__ import annotations

import json
import shutil
import socket
import threading
import urllib.error
import urllib.request
from pathlib import Path

import pytest
import yaml

from synqt import design, designdoc

EXAMPLES = Path(__file__).resolve().parents[3] / "examples"
TOKEN = "test-token"


@pytest.fixture
def server(tmp_path):
    project = tmp_path / "gavel"
    shutil.copytree(EXAMPLES / "gavel", project,
                    ignore=shutil.ignore_patterns("build", ".synqt"))
    httpd = design.make_server(project, port=0, token=TOKEN)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{httpd.server_port}"
    _answering(base)
    yield base, project
    httpd.shutdown()
    httpd.server_close()
    thread.join(timeout=5)


def _request(url, *, data=None, token=TOKEN, origin=None, referer=None, host=None):
    body = None if data is None else json.dumps(data).encode("utf-8")
    request = urllib.request.Request(url, data=body)
    if body is not None:
        request.add_header("Content-Type", "application/json")
    if token is not None:
        request.add_header(design.TOKEN_HEADER, token)
    if origin is not None:
        request.add_header("Origin", origin)
    if referer is not None:
        request.add_header("Referer", referer)
    if host is not None:
        request.add_header("Host", host)
    return urllib.request.urlopen(request)


def _get(url, **headers):
    return _request(url, **headers)


def _post(url, payload, **headers):
    return _request(url, data=payload, **headers)


def _answering(base):
    """Wait until the server is serving, not merely listening.

    The socket is bound by the constructor, so a connection is accepted by the operating
    system before the thread reaches serve_forever; shutting a server down that never got
    there waits for a loop that will not end. One answered request settles it.
    """
    try:
        _request(f"{base}/api/project")
    except urllib.error.HTTPError:
        pass


def _json(response):
    return json.loads(response.read().decode("utf-8"))


def _refused(url, **arguments):
    """The status of a request that is expected not to be served."""
    with pytest.raises(urllib.error.HTTPError) as caught:
        _request(url, **arguments)
    return caught.value.code


def _config(project):
    return yaml.safe_load((project / "synqt.yaml").read_text())


@pytest.fixture
def assets(tmp_path, monkeypatch):
    """The editor's own files, with something next to them that is not one of them."""
    directory = tmp_path / "assets"
    directory.mkdir()
    (directory / "index.html").write_text("<!doctype html><title>the editor</title>")
    (tmp_path / "secret.txt").write_text("not the editor's to serve")
    monkeypatch.setattr(design, "ASSETS", directory)
    return directory


# What the editor reads


def test_the_project_reads_back_as_a_design_document(server):
    base, _ = server
    body = _json(_get(f"{base}/api/project"))
    assert [e["name"] for e in body["document"]["entities"]] == ["client", "web", "database"]
    assert "findings" in body
    assert body["ok"] is True


def test_the_project_reads_back_the_contract_behind_every_link(server):
    base, _ = server
    body = _json(_get(f"{base}/api/project"))
    ledger = next(link for link in body["document"]["links"] if link["name"] == "ledger")
    assert ledger["owner"] == "database"
    assert ledger["members"]


def test_infer_reads_the_contracts_back_out_of_the_qml(server):
    base, project = server
    before = (project / "shared" / "Auction.syn").read_text()
    body = _json(_post(f"{base}/api/infer", {}))
    auction = next(link for link in body["document"]["links"] if link["name"] == "auction")
    assert {member["name"] for member in auction["members"]} >= {"itemName", "placeBid"}
    assert body["document"]["sourceHash"] == designdoc.source_hash(project)
    assert body["typedBy"] in ("ts", "heuristic")
    # It reads and answers. Writing a contract is what applying a change set does.
    assert (project / "shared" / "Auction.syn").read_text() == before


# The guard


def test_no_token_is_refused(server):
    base, _ = server
    assert _refused(f"{base}/api/project", token=None) == 403


def test_a_wrong_token_is_refused(server):
    base, _ = server
    assert _refused(f"{base}/api/project", token="guessed") == 403


def test_a_foreign_origin_is_refused(server):
    base, _ = server
    assert _refused(f"{base}/api/project", origin="https://evil.example") == 403


def test_a_foreign_referer_is_refused(server):
    """A page that never sends an Origin still sends where it came from. Both are read,
    because a request that names another page as its source is not this editor's.
    """
    base, _ = server
    assert _refused(f"{base}/api/project", referer="https://evil.example/page") == 403


def test_a_host_header_that_is_not_this_server_is_refused(server):
    """The DNS rebinding case: a name the attacker controls, pointed at 127.0.0.1, so the
    browser believes their page and this server share an origin and sends no Origin worth
    refusing. What the request calls this server is checked as well as where it came from.
    """
    base, _ = server
    port = base.rsplit(":", 1)[1]
    assert _refused(f"{base}/api/project", host=f"rebound.example:{port}") == 403


def test_the_page_itself_is_not_what_the_token_protects(server, assets):
    """The token travels in the URL fragment, which no browser sends to a server, so the
    page that reads it cannot present it as it loads. The shell carries nothing about the
    project; everything that does is behind /api, and that is what the token guards. It is
    served under a policy that lets it load nothing from anywhere else and be framed by
    nobody, so a page that cannot read its replies cannot borrow its window either.
    """
    base, _ = server
    response = _get(f"{base}/", token=None)
    assert b"the editor" in response.read()
    assert "default-src 'none'" in response.headers["Content-Security-Policy"]
    assert "frame-ancestors 'none'" in response.headers["Content-Security-Policy"]


def test_a_path_that_escapes_the_project_is_refused(server, assets):
    """The file asked for is the one the path resolves to, and a path that resolves outside
    the editor's own directory reaches nothing, however it was spelled.
    """
    base, _ = server
    for path in ("/../secret.txt", "/..%2fsecret.txt", "/parts/../../secret.txt",
                 "/../../etc/passwd"):
        assert _refused(f"{base}{path}") in (400, 403, 404)


def test_only_the_editors_own_directory_answers(assets):
    assert design._asset_path("/") == assets / "index.html"
    assert design._asset_path("/parts/canvas.js") == assets / "parts" / "canvas.js"
    for escape in ("/../secret.txt", "/..%2Fsecret.txt", "/a/../../secret.txt",
                   "/\x00../secret.txt"):
        assert design._asset_path(escape) is None


def test_the_server_binds_loopback_only(server):
    base, _ = server
    httpd_host = base.split("//")[1].split(":")[0]
    assert httpd_host == "127.0.0.1"


# Validating, planning, applying


def test_validate_answers_without_touching_the_project(server):
    base, project = server
    before = (project / "synqt.yaml").read_text()
    document = designdoc.read(project)
    ledger = next(link for link in document["links"] if link["name"] == "ledger")
    ledger["consumers"] = ["web", "client"]
    body = _json(_post(f"{base}/api/validate", {"document": document}))
    assert body["ok"] is False
    assert any(message.startswith("error:") for message in body["findings"])
    assert (project / "synqt.yaml").read_text() == before


def test_a_plan_says_what_it_would_do_before_it_does_any_of_it(server):
    base, project = server
    document = designdoc.read(project)
    document["entities"].append({"id": "new", "name": "api", "kind": "service",
                                 "blueprint": "jobs", "x": 400, "y": 40})
    body = _json(_post(f"{base}/api/plan", {"document": document}))
    assert "api/Schedule.qml" in [change["path"] for change in body["changes"]]
    assert body["digest"] and body["diff"]
    assert not (project / "api").exists()


def test_apply_refuses_a_digest_that_does_not_match(server):
    """What is applied is what was shown. A document the editor changed after the plan was
    drawn no longer matches the diff somebody read and approved, so it is refused rather
    than applied and explained afterwards.
    """
    base, project = server
    document = designdoc.read(project)
    document["entities"].append({"id": "new", "name": "api", "kind": "service",
                                 "blueprint": "jobs", "x": 400, "y": 40})
    assert _refused(f"{base}/api/apply", data={"document": document,
                                               "digest": "stale"}) == 409
    assert not (project / "api").exists()


def test_plan_then_apply_writes_the_change(server):
    base, project = server
    document = designdoc.read(project)
    document["entities"].append({"id": "new", "name": "api", "kind": "service",
                                 "blueprint": "jobs", "x": 400, "y": 40})
    plan = _json(_post(f"{base}/api/plan", {"document": document}))
    body = _json(_post(f"{base}/api/apply", {"document": document,
                                             "digest": plan["digest"]}))
    assert (project / "api" / "Schedule.qml").is_file()
    assert [e["name"] for e in _config(project)["entities"]][-1] == "api"
    assert body["applied"]
    assert [e["name"] for e in body["document"]["entities"]][-1] == "api"


def test_applying_keeps_where_the_boxes_were_put(server):
    """The canvas is a drawing and stays out of synqt.yaml, but it is still somebody's
    work: a node dragged somewhere is there again the next time the project is opened.
    """
    base, project = server
    document = designdoc.read(project)
    document["entities"][0]["x"] = 137
    document["entities"].append({"id": "new", "name": "api", "kind": "service",
                                 "blueprint": "jobs", "x": 400, "y": 40})
    plan = _json(_post(f"{base}/api/plan", {"document": document}))
    _post(f"{base}/api/apply", {"document": document, "digest": plan["digest"]})
    places = json.loads(designdoc.layout_path(project).read_text())["entities"]
    assert places["client"]["x"] == 137
    assert places["api"] == {"x": 400, "y": 40}
    assert designdoc.read(project)["entities"][0]["x"] == 137


def test_apply_refuses_a_design_that_does_not_pass_check(server):
    """The browser reaching the database is the check the auction tutorial ends on, and
    the editor is not a way around it: a design that `synqt check` refuses is refused here
    too, before anything is written.
    """
    base, project = server
    document = designdoc.read(project)
    ledger = next(link for link in document["links"] if link["name"] == "ledger")
    ledger["consumers"] = ["web", "client"]
    plan = _json(_post(f"{base}/api/plan", {"document": document}))
    assert plan["ok"] is False
    assert _refused(f"{base}/api/apply", data={"document": document,
                                               "digest": plan["digest"]}) == 400
    assert _config(project)["connect_points"][2]["consumers"] == ["web"]


def test_a_body_that_is_not_a_document_is_refused_rather_than_guessed_at(server):
    base, project = server
    assert _refused(f"{base}/api/plan", data={"document": "the whole thing"}) == 400
    assert _refused(f"{base}/api/apply",
                    data={"document": designdoc.read(project)}) == 400


def test_the_token_travels_where_no_browser_sends_it():
    """In the fragment, not the query: a fragment reaches the page that reads it and never
    the server, a proxy log, or a Referer. A token in the query string is a token in
    somebody's history file.
    """
    address = design.url_for(8181, "s3cret")
    assert address.endswith("#token=s3cret")
    assert "?" not in address


def test_a_directory_that_is_not_a_project_is_refused_before_a_port_is_bound(tmp_path):
    with pytest.raises(design.DesignError):
        design.make_server(tmp_path, port=0, token=TOKEN)


def test_an_unknown_route_is_a_404_not_a_stack_trace(server):
    base, _ = server
    assert _refused(f"{base}/api/nothing-here") == 404


# What arrives in the body

def _raw(base, path, body, *, length=None, token=TOKEN):
    """POST bytes that urllib would not build for us, and read the status back.

    The framing cases below are about what the server does with a Content-Length and a body
    that do not agree, which means writing both by hand rather than through a request
    object that keeps them consistent.
    """
    with socket.create_connection(("127.0.0.1", int(base.rsplit(":", 1)[1]))) as sock:
        stated = length if length is not None else len(body)
        head = (f"POST {path} HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                f"{design.TOKEN_HEADER}: {token}\r\n"
                f"Content-Type: application/json\r\nContent-Length: {stated}\r\n"
                "Connection: close\r\n\r\n")
        sock.sendall(head.encode("ascii") + body)
        answer = b""
        while True:
            more = sock.recv(4096)
            if not more:
                break
            answer += more
    return int(answer.split(b" ", 2)[1])


def test_a_content_length_that_is_not_a_number_is_refused(server):
    base, _ = server
    assert _raw(base, "/api/plan", b"{}", length="a lot") == 400


def test_an_empty_body_is_refused_rather_than_read_as_an_empty_design(server):
    """A POST with nothing in it is a request that got lost, not a design of no entities,
    and applying the second reading would remove a project's whole topology."""
    base, _ = server
    assert _raw(base, "/api/plan", b"", length=0) == 400


def test_a_body_over_the_cap_is_refused_before_it_is_read(server):
    base, _ = server
    assert _raw(base, "/api/plan", b"{}",
                length=design.MAX_BODY_BYTES + 1) == 413


def test_a_body_that_is_not_json_is_refused(server):
    base, _ = server
    assert _raw(base, "/api/plan", b"not json at all") == 400


def test_a_document_whose_entities_are_not_a_list_is_refused(server):
    base, _ = server
    assert _refused(f"{base}/api/plan",
                    data={"document": {"version": 1, "entities": {}}}) == 400
    assert _refused(f"{base}/api/plan",
                    data={"document": {"version": 1, "links": "auction"}}) == 400


def test_a_request_the_server_cannot_answer_is_answered_anyway(server, monkeypatch):
    """A page waiting on a connection that was closed without a word is worse than a page
    told what went wrong: the editor would sit on Review until somebody reloaded it."""
    base, _ = server

    def broken(*arguments, **named):
        raise RuntimeError("the plan blew up")

    monkeypatch.setattr(design.designplan, "compute", broken)
    assert _refused(f"{base}/api/plan", data={"document": {"version": 1}}) == 500


def test_a_project_whose_qml_cannot_be_read_back_is_refused_with_the_reason(server,
                                                                           monkeypatch):
    base, _ = server

    def unreadable(*arguments, **named):
        raise design.infer.InferError("shared/Auction.syn does not parse")

    monkeypatch.setattr(design.infer, "collect", unreadable)
    assert _refused(f"{base}/api/infer", data={}) == 400


# Standing it up

def test_a_server_with_no_token_is_refused(tmp_path):
    """Every request carries the token, so a server without one is a server nothing can
    reach; making that a startup error says so rather than leaving it to be discovered."""
    project = tmp_path / "gavel"
    shutil.copytree(EXAMPLES / "gavel", project,
                    ignore=shutil.ignore_patterns("build", ".synqt"))
    with pytest.raises(design.DesignError):
        design.make_server(project, port=0, token="")


def test_a_port_already_taken_is_a_message_rather_than_a_traceback(tmp_path):
    project = tmp_path / "gavel"
    shutil.copytree(EXAMPLES / "gavel", project,
                    ignore=shutil.ignore_patterns("build", ".synqt"))
    held = design.make_server(project, port=0, token=TOKEN)
    try:
        with pytest.raises(design.DesignError) as refused:
            design.make_server(project, port=held.server_port, token=TOKEN)
        assert str(held.server_port) in str(refused.value)
    finally:
        held.server_close()


def test_serve_prints_where_it_is_and_stops_when_it_is_asked_to(tmp_path, capsys,
                                                                monkeypatch):
    """The URL is printed once and nowhere else, so this is the only place a reader gets
    the token, and it carries the token in the fragment even here."""
    project = tmp_path / "gavel"
    shutil.copytree(EXAMPLES / "gavel", project,
                    ignore=shutil.ignore_patterns("build", ".synqt"))
    made = {}
    real = design.make_server

    def remember(*arguments, **named):
        made["httpd"] = real(*arguments, **named)
        return made["httpd"]

    monkeypatch.setattr(design, "make_server", remember)

    opened = []

    def opening(address):
        # serve_forever blocks, so what ends it has to come from somewhere else. Asking the
        # real server to stop, rather than faking the loop, keeps shutdown and server_close
        # doing what they do: a shutdown() on a loop that never ran waits for ever.
        opened.append(address)
        threading.Thread(target=made["httpd"].shutdown, daemon=True).start()

    monkeypatch.setattr(design.webbrowser, "open", opening)

    assert design.serve(project, port=0) == "synqt design: stopped."
    printed = capsys.readouterr().out
    assert "http://127.0.0.1:" in printed and "#token=" in printed
    assert opened and "#token=" in opened[0]


if __name__ == "__main__":
    pytest.main([__file__])
