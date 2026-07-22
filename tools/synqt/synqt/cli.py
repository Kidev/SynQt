# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The ``synqt`` command-line interface: the npm-shaped path from `synqt new` to
`synqt dev` to `synqt build`, plus the mesh certificate tooling and the scaffolders."""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

import yaml

from . import (addauth, addcontract, addentity, addprovider, build as buildmod,
               check as checkmod, clientbuild, doctor, mesh, newproject, run as runmod,
               version as versionmod)


def _load_config(project_dir: str) -> Dict[str, Any]:
    path = Path(project_dir) / "synqt.yaml"
    return yaml.safe_load(path.read_text()) if path.exists() else {}


def _service_entities(config: Dict[str, Any]) -> List[str]:
    return [e.get("name") for e in config.get("entities", [])
            if isinstance(e, dict) and e.get("kind") != "client"]


class _PrintVersionAction(argparse.Action):
    """Print `version.version_lines()` as three literal lines.

    argparse's own ``action="version"`` runs the version string through the parser's
    HelpFormatter, whose `_fill_text` collapses every embedded newline into a space
    before wrapping to the terminal width; the three lines from `version_lines()` would
    come out as one reflowed paragraph. Printing them directly keeps them three lines.
    """

    def __init__(self, option_strings: List[str], dest: str = argparse.SUPPRESS,
                default: str = argparse.SUPPRESS, help: Optional[str] = None) -> None:
        super().__init__(option_strings=option_strings, dest=dest, default=default,
                         nargs=0, help=help)

    def __call__(self, parser: argparse.ArgumentParser, namespace: argparse.Namespace,
                values: Any, option_string: Optional[str] = None) -> None:
        print("\n".join(versionmod.version_lines()))
        parser.exit()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="synqt", description="The SynQt CLI.")
    parser.add_argument("--version", "-V", action=_PrintVersionAction,
                        help="print the CLI version and the pinned toolchain")
    sub = parser.add_subparsers(dest="command", required=False)

    new = sub.add_parser("new", help="scaffold a new project")
    new.add_argument("name")
    new.add_argument("--origin-model", default="same_origin",
                     choices=["same_origin", "split_origin"])
    new.add_argument("--auth", default=None, help="provider to prime auth for (e.g. github)")
    new.add_argument("--blueprint", action="append", dest="blueprints", default=[],
                     help="a starting blueprint entity (repeatable)")
    new.add_argument("--parent-dir", default=".")

    for name, helptext in [("dev", "build, start locally, watch and hot reload"),
                           ("build", "production build of every entity artifact"),
                           ("serve", "run the built entities in dependency order"),
                           ("test", "build and run the project test suite"),
                           ("check", "validate config, lint contracts and QML"),
                           ("clean", "remove build outputs"),
                           ("doctor", "diagnose toolchain, certificates, versions"),
                           ("providers", "list bundled providers per family")]:
        p = sub.add_parser(name, help=helptext)
        if name != "providers":
            p.add_argument("--project-dir", default=".")
        if name in ("dev", "build"):
            p.add_argument("--release", action="store_true", default=(name == "build"))
            p.add_argument("--debug", action="store_true")
            p.add_argument("--client", default="wasm", choices=["wasm", "desktop", "all"])
            p.add_argument("--verbose", action="store_true",
                           help="echo each build command and stream its output")
        if name == "build":
            p.add_argument("--entity", default=None,
                           help="build one entity instead of every one")
            # Deliberately not on `dev`: dev re-reads synqt.yaml on every hot reload, so an
            # override held only in argv would be dropped mid-session, leaving a threaded
            # client served without the cross-origin isolation it needs (pitfall 13, and a
            # silent failure at that). For dev, set build.client_threads in synqt.yaml.
            p.add_argument("--threads", default=None, choices=list(clientbuild.MODES),
                           help="override build.client_threads for this build "
                                "(multi implies cross-origin isolation)")
        if name == "dev":
            p.add_argument("--desktop", action="store_true", help="run the client natively")
            p.add_argument("--port", type=int, default=8080, help="the local dev port")
            p.add_argument("--no-open", action="store_true", help="do not open a browser")
            p.add_argument("--no-watch", action="store_true",
                           help="serve once without watching for changes")

    meshp = sub.add_parser("mesh", help="the project CA and per-entity certificates")
    mesh_sub = meshp.add_subparsers(dest="mesh_command", required=True)
    mi = mesh_sub.add_parser("init"); mi.add_argument("--force", action="store_true")
    mc = mesh_sub.add_parser("cert"); mc.add_argument("entity", nargs="?")
    mc.add_argument("--all", action="store_true")
    mr = mesh_sub.add_parser("rotate"); mr.add_argument("entity", nargs="?")
    mesh_sub.add_parser("status")
    for mp in (mi, mc, mr):
        mp.add_argument("--project-dir", default=".")
    meshp.set_defaults(project_dir=".")

    add = sub.add_parser("add", help="add a capability to the project")
    add_sub = add.add_subparsers(dest="what", required=True)
    auth = add_sub.add_parser("auth"); auth.add_argument("provider")
    auth.add_argument("--required", action="store_true")
    auth.add_argument("--provider-entity", default="")
    entity = add_sub.add_parser("entity"); entity.add_argument("name")
    entity.add_argument("--blueprint", default="service"); entity.add_argument("--provider")
    provider = add_sub.add_parser("provider"); provider.add_argument("name")
    provider.add_argument("--family", required=True)
    contract = add_sub.add_parser("contract"); contract.add_argument("name")
    connect_point = add_sub.add_parser("connect-point"); connect_point.add_argument("name")
    connect_point.add_argument("--owner", required=True)
    connect_point.add_argument("--consumers", default="", help="comma-separated entity names")
    connect_point.add_argument("--contract", required=True)
    connect_point.add_argument("--instance", default="shared",
                               choices=["shared", "per_session", "per_peer"])
    for ap in (auth, entity, provider, contract, connect_point):
        ap.add_argument("--project-dir", default=".")
    return parser


def _run_add(args: argparse.Namespace) -> int:
    if args.what == "auth":
        message = addauth.scaffold(args.project_dir, args.provider, required=args.required,
                                   provider_entity=args.provider_entity)
    elif args.what == "entity":
        message = addentity.scaffold(args.project_dir, args.name, args.blueprint,
                                     provider=args.provider)
    elif args.what == "provider":
        message = addprovider.scaffold(args.project_dir, args.name, args.family)
    elif args.what == "contract":
        message = addcontract.scaffold_contract(args.project_dir, args.name)
    else:  # connect-point
        consumers = [c for c in args.consumers.split(",") if c]
        message = addcontract.scaffold_connect_point(
            args.project_dir, args.name, owner=args.owner, consumers=consumers,
            contract=args.contract, instance=args.instance)
    print(message)
    return 0


def _run_mesh(args: argparse.Namespace) -> int:
    config = _load_config(args.project_dir)
    if args.mesh_command == "init":
        print(mesh.init(args.project_dir, force=args.force))
    elif args.mesh_command == "cert":
        if args.all:
            print(mesh.cert_all(args.project_dir, _service_entities(config)))
        elif args.entity:
            print(mesh.cert(args.project_dir, args.entity))
        else:
            raise mesh.MeshError("give an entity name or --all")
    elif args.mesh_command == "rotate":
        print(mesh.rotate(args.project_dir, args.entity, _service_entities(config)))
    elif args.mesh_command == "status":
        print(mesh.status(args.project_dir))
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if not getattr(args, "command", None):
        parser.print_help()
        return 2
    try:
        if args.command == "new":
            print(newproject.scaffold(args.parent_dir, args.name,
                                      origin_model=args.origin_model, auth=args.auth,
                                      blueprints=args.blueprints))
        elif args.command == "providers":
            print(addentity.list_providers())
        elif args.command == "doctor":
            print(doctor.report(args.project_dir))
        elif args.command == "check":
            ok, messages = checkmod.check_project(args.project_dir)
            print("\n".join(messages))
            return 0 if ok else 1
        elif args.command == "clean":
            build_dir = Path(args.project_dir) / "build"
            if build_dir.exists():
                shutil.rmtree(build_dir)
            print("Removed build/ (kept the toolchain cache and the CA).")
        elif args.command in ("build", "dev"):
            release = args.release and not args.debug
            if args.command == "dev":
                # Development keeps mutual TLS with a throwaway dev CA.
                mesh.init(args.project_dir, dev=True, force=True)
                mesh.cert_all(args.project_dir, _service_entities(_load_config(args.project_dir)),
                              dev=True)
            print(buildmod.build(args.project_dir, release=release, client=args.client,
                                 entity=getattr(args, "entity", None),
                                 threads=getattr(args, "threads", None),
                                 verbose=args.verbose))
            if args.command == "dev":
                print()
                print(runmod.dev(args.project_dir, port=args.port,
                                 open_browser=not args.no_open, client=args.client,
                                 watch=not args.no_watch))
        elif args.command == "serve":
            print(runmod.serve(args.project_dir))
        elif args.command == "test":
            return runmod.test(args.project_dir)
        elif args.command == "mesh":
            return _run_mesh(args)
        elif args.command == "add":
            return _run_add(args)
        else:
            parser.error("unknown command")
    except (newproject.NewProjectError, addauth.AddAuthError, addentity.AddEntityError,
            addprovider.AddProviderError, addcontract.AddContractError, mesh.MeshError,
            buildmod.BuildError, FileNotFoundError) as error:
        print(f"synqt {args.command}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
