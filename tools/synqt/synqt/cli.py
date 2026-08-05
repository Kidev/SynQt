# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The ``synqt`` command-line interface: the npm-shaped path from `synqt new` to
`synqt dev` to `synqt build`, plus the mesh certificate tooling and the scaffolders."""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

from . import (addauth, addcontract, addentity, addprovider, appmodel,
               build as buildmod, check as checkmod, clientbuild,
               config as configmod, create, deploy as deploymod, design as designmod,
               docker as dockermod, doctor, infer as infermod, mesh, newproject,
               run as runmod, typebackend, version as versionmod)


def _load_config(project_dir: str, profile: Optional[str] = None) -> Dict[str, Any]:
    return configmod.load(project_dir, profile=profile)


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
    # No --origin-model. A scaffolded project is same-origin, which is the only shape whose
    # session cookie is first-party and therefore the only one that keeps working as browsers
    # wind down third-party cookies. `split_origin` still exists and is still validated, but
    # reaching it takes a hand edit to synqt.yaml after reading what it costs; see the
    # "Serving the client from another origin" section of docs/project-layout-and-config.md.
    new.add_argument("--auth", default=None, help="provider to prime auth for (e.g. github)")
    new.add_argument("--blueprint", action="append", dest="blueprints", default=[],
                     help="a starting blueprint entity (repeatable)")
    new.add_argument("--parent-dir", default=".")

    # The interactive twin of `new`, as its own command rather than a mode of that one.
    # A command that prompts only when it happens to have a terminal behaves differently
    # in CI than in a shell under one name; these two say which you are getting, and
    # `create` refuses to run without a terminal rather than silently taking defaults.
    create_cmd = sub.add_parser("create", help="scaffold a new project, asking first")
    create_cmd.add_argument("name", nargs="?", default=None,
                            help="project name (asked for when omitted)")
    create_cmd.add_argument("--parent-dir", default=".")

    for name, helptext in [("dev", "build, start locally, watch and hot reload"),
                           ("design", "edit the topology as a graph, in a browser"),
                           ("build", "production build of every entity artifact"),
                           ("serve", "run the built entities in dependency order"),
                           ("test", "build and run the project test suite"),
                           ("check", "validate config, lint contracts and QML"),
                           ("infer", "read back the contracts the QML already implies"),
                           ("clean", "remove build outputs"),
                           ("doctor", "diagnose toolchain, certificates, versions"),
                           ("providers", "list bundled providers per family")]:
        p = sub.add_parser(name, help=helptext)
        if name != "providers":
            p.add_argument("--project-dir", default=".")
        if name in ("dev", "design", "build", "serve", "check", "infer", "doctor"):
            # The commands that read the topology take the profile that layers over it
            # (docs/project-layout-and-config.md, "Configuration resolution order").
            # `clean`, `providers`, and `test` read no configuration, so offering them a
            # profile would only suggest it changes something.
            p.add_argument("--profile", default=None, metavar="NAME",
                           help="layer synqt.<NAME>.yaml over synqt.yaml")
        if name == "check":
            # The rules that bind only a shipped artifact (TLS to the browser, mutual TLS
            # off-machine, a wss desktop edge URL) would reject a perfectly good localhost
            # topology, so they are opt-in here and automatic on `build --release`/`serve`.
            p.add_argument("--release", action="store_true",
                           help="also apply the rules a release build and serve apply")
        if name == "infer":
            # Reporting is what this does; writing is what you ask it for. A scan that
            # guessed at half its types has no business rewriting the contracts a
            # deployment is built from because somebody wanted to see what it found.
            p.add_argument("--write", action="store_true",
                           help="write shared/<Contract>.syn for every link it found")
            p.add_argument("--force", action="store_true",
                           help="with --write, overwrite a contract that is already there")
            p.add_argument("--json", action="store_true",
                           help="print the result as a design document instead of a report")
        if name in ("infer", "check"):
            # A literal is all a token scan can type, and most arguments are not literals.
            # `auto` takes TypeScript where it is installed, `ts` refuses rather than
            # quietly answering worse, and `heuristic` is the literal reader on its own.
            # `check` takes it for the same reason `infer` does: it compares a contract
            # with the calls that cross it, and an argument nobody could type is an
            # argument it says nothing about.
            p.add_argument("--types", default="auto", choices=list(typebackend.MODES),
                           help="who answers what type an expression has (default: auto)")
        if name in ("dev", "build"):
            p.add_argument("--release", action="store_true", default=(name == "build"))
            p.add_argument("--debug", action="store_true")
            # `none` builds the service entities and no client at all. It is what a
            # container image wants when the browser bundle is coming from somewhere else
            # (`synqt docker init --client host`), and it is the difference between a build
            # that needs an Emscripten kit and one that does not: with no wasm target
            # requested, the toolchain check stops asking for one.
            p.add_argument("--client", default="wasm",
                           choices=["wasm", "desktop", "all", "none"])
            p.add_argument("--verbose", action="store_true",
                           help="echo each build command and stream its output")
        if name == "build":
            p.add_argument("--entity", default=None,
                           help="build one entity instead of every one")
            # Off by default, and that default is the documented position (docs/desktop.md):
            # signing and notarization are not a framework's to choose, so the build produces
            # an artifact the platform step can be run against and names the command. This is
            # the opt-in for wanting the deployed tree from the one command anyway. It still
            # never signs.
            p.add_argument("--deploy", action="store_true",
                           help="also run the platform deploy step on a desktop client "
                                "(macdeployqt/windeployqt/portable layout); requires "
                                "--sign or --unsigned")
            # --deploy on its own is refused. What an unsigned build costs differs per
            # platform (refused by Gatekeeper / warned about by SmartScreen / entirely
            # normal on Linux), and the person who most needs to know their app will not
            # open on anyone else's Mac is exactly the one who would not read a note about
            # it. deploy.check_signing_choice says which applies here.
            p.add_argument("--sign", default=None, metavar="IDENTITY",
                           help="sign the deployed client with this identity (macOS: a "
                                "codesign identity; Windows: the certificate subject name)")
            p.add_argument("--unsigned", action="store_true",
                           help="deploy without signing, accepting what that means on this "
                                "platform")
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
        if name == "design":
            # A port of its own, so the editor and `synqt dev` can be up at the same time:
            # drawing a connect point and watching it come up is the whole point of having
            # both open.
            p.add_argument("--port", type=int, default=8181,
                           help="the loopback port the editor is served on")
            p.add_argument("--no-open", action="store_true",
                           help="print the URL instead of opening a browser")

    meshp = sub.add_parser("mesh", help="the project CA and per-entity certificates")
    mesh_sub = meshp.add_subparsers(dest="mesh_command", required=True)
    mi = mesh_sub.add_parser("init"); mi.add_argument("--force", action="store_true")
    mc = mesh_sub.add_parser("cert"); mc.add_argument("entity", nargs="?")
    mc.add_argument("--all", action="store_true")
    mr = mesh_sub.add_parser("rotate"); mr.add_argument("entity", nargs="?")
    ms = mesh_sub.add_parser("status")
    for mp in (mi, mc, mr, ms):
        # `status` takes --project-dir like its siblings: it reads what is on disk, and
        # "which certificates does that deployment hold" is a question worth asking about
        # a directory you are not standing in.
        mp.add_argument("--project-dir", default=".")
    for mp in (mi, mc, mr):
        # A profile may add an entity, and an entity with no certificate cannot join the
        # mesh, so `mesh cert --all` has to see the same entity list the build will.
        # `status` needs none of that: it reports the certificate files themselves, and
        # no profile changes which ones exist.
        mp.add_argument("--profile", default=None, metavar="NAME",
                        help="layer synqt.<NAME>.yaml over synqt.yaml")
    meshp.set_defaults(project_dir=".", profile=None)

    # `synqt docker`: generate the container setup for an existing project, and drive it.
    # `init` is the one that writes anything; `up` and `down` are `docker compose` with the
    # profile and the two checks worth making before it, so that neither has to be
    # remembered.
    dockerp = sub.add_parser("docker", help="run the whole project in containers")
    docker_sub = dockerp.add_subparsers(dest="docker_command", required=True)
    di = docker_sub.add_parser("init", help="generate the Dockerfile, compose file, profile")
    di.add_argument("--force", action="store_true",
                    help="regenerate files that already exist")
    di.add_argument("--subnet", default=dockermod.DEFAULT_SUBNET, metavar="CIDR",
                    help="the private network the entity containers address each other on")
    di.add_argument("--client", default="image", choices=list(dockermod.CLIENT_MODES),
                    help="image: build the browser bundle inside the image (needs nothing "
                         "installed); host: mount the one `synqt build` produced here")
    di.add_argument("--port", type=int, default=None,
                    help="publish the edge on this port instead of the one in synqt.yaml")
    # A generator that prompts when it has a terminal and picks defaults when it does not
    # is two behaviors under one name (see create.py). The flag says which you get, and the
    # questions are only ever about secrets that have to come from outside anyway.
    di.add_argument("--no-input", action="store_true",
                    help="ask nothing; leave a placeholder for every secret from outside")
    du = docker_sub.add_parser("up", help="build the images and start every container")
    du.add_argument("--detach", "-d", action="store_true", help="start in the background")
    du.add_argument("--no-build", action="store_true",
                    help="start what is already built instead of rebuilding first")
    dd = docker_sub.add_parser("down", help="stop every container")
    dd.add_argument("--volumes", action="store_true",
                    help="also remove the mesh CA and any engine data (a clean slate)")
    for dp in (di, du, dd):
        dp.add_argument("--project-dir", default=".")
    dockerp.set_defaults(project_dir=".")

    add = sub.add_parser("add", help="add a capability to the project")
    add_sub = add.add_subparsers(dest="what", required=True)
    auth = add_sub.add_parser("auth"); auth.add_argument("provider")
    auth.add_argument("--required", action="store_true")
    auth.add_argument("--provider-entity", default="")
    entity = add_sub.add_parser("entity"); entity.add_argument("name")
    entity.add_argument("--blueprint", default="service"); entity.add_argument("--provider")
    entity.add_argument("--source", default="",
                        help="what to call the entity's Source stub (default: the "
                             "blueprint's own, e.g. Items for persistence)")
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


def _fails_validation(project_dir: str, *, release: bool, starting: bool = False,
                      profile: Optional[str] = None) -> bool:
    """Run the topology validation ahead of a build or a run, and report it.

    Only the topology half runs here, not the full `synqt check`: the contract, QML, and
    route lints shell out to qmllint and read every QML file in the project, which is a
    second or more on each hot reload, and `synqt dev` calls this on every rebuild. The
    rules that stop a broken or unsafe deployment are all in validate().
    """
    if not (Path(project_dir) / "synqt.yaml").exists():
        return False  # not a project yet; the command below reports that in its own words
    resolved = configmod.resolve(project_dir, profile=profile)
    for source in resolved.sources:
        print(f"synqt: {source}")
    ok, messages = checkmod.validate(resolved.config, release=release,
                                     project_dir=project_dir, starting=starting)
    for message in messages:
        if message.startswith(("error:", "warn:")):
            print(message, file=sys.stderr if message.startswith("error:") else sys.stdout)
    if not ok:
        print("synqt: refusing to continue with an invalid configuration "
              "(run 'synqt check' for the full report).", file=sys.stderr)
    return not ok


def _run_add(args: argparse.Namespace) -> int:
    if args.what == "auth":
        message = addauth.scaffold(args.project_dir, args.provider, required=args.required,
                                   provider_entity=args.provider_entity)
    elif args.what == "entity":
        message = addentity.scaffold(args.project_dir, args.name, args.blueprint,
                                     provider=args.provider, source=args.source or None)
    elif args.what == "provider":
        message = addprovider.scaffold(args.project_dir, args.name, args.family)
    elif args.what == "contract":
        message = addcontract.scaffold_contract(args.project_dir, args.name)
    else: # connect-point
        consumers = [c for c in args.consumers.split(",") if c]
        message = addcontract.scaffold_connect_point(
            args.project_dir, args.name, owner=args.owner, consumers=consumers,
            contract=args.contract, instance=args.instance)
    print(message)
    return 0


def _run_mesh(args: argparse.Namespace) -> int:
    config = _load_config(args.project_dir, args.profile)
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


def _run_docker(args: argparse.Namespace) -> int:
    if args.docker_command == "init":
        # The container topology is validated before it is written, not after it fails to
        # come up: a project whose synqt.yaml is already invalid produces a compose file
        # that is invalid in exactly the same way, four minutes into an image build.
        if _fails_validation(args.project_dir, release=False):
            return 1
        config = _load_config(args.project_dir)
        print(dockermod.init(args.project_dir, config, force=args.force,
                             subnet=args.subnet, client=args.client, port=args.port,
                             source=None if args.no_input else sys.stdin))
        return 0
    if args.docker_command == "up":
        command = dockermod.up_command(args.project_dir, detach=args.detach,
                                       build=not args.no_build)
    else:
        command = dockermod.down_command(args.project_dir, volumes=args.volumes)
    print(f"synqt: {' '.join(command)}")
    return dockermod.run(args.project_dir, command)


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if not getattr(args, "command", None):
        parser.print_help()
        return 2
    try:
        if args.command == "new":
            print(newproject.scaffold(args.parent_dir, args.name, auth=args.auth,
                                      blueprints=args.blueprints))
        elif args.command == "create":
            print(create.create(args.parent_dir, name=args.name))
        elif args.command == "providers":
            print(addentity.list_providers())
        elif args.command == "doctor":
            print(doctor.report(args.project_dir, profile=args.profile))
        elif args.command == "check":
            ok, messages = checkmod.check_project(args.project_dir, release=args.release,
                                                  types=args.types, profile=args.profile)
            print("\n".join(messages))
            return 0 if ok else 1
        elif args.command == "infer":
            config = _load_config(args.project_dir, args.profile)
            backend = typebackend.resolve(args.types, args.project_dir)
            edges = infermod.collect(args.project_dir, config, backend=backend)
            if args.json:
                print(json.dumps(infermod.to_document(edges, config), indent=2))
            else:
                print(infermod.report(edges, typed_by=typebackend.name_of(backend)))
                if edges and not args.write:
                    print("\nWrite these to shared/ with: synqt infer --write")
            if args.write:
                written = infermod.write(args.project_dir, edges, force=args.force)
                # With --json the report is machine-read, so what was written is said on
                # stderr rather than in the middle of the document.
                for path in written:
                    print(f"wrote {path}", file=sys.stderr if args.json else sys.stdout)
        elif args.command == "design":
            # No validation gate here, unlike `build` and `serve`. A topology the validator
            # refuses is exactly what somebody opens the editor to fix, and refusing to open
            # it would put the repair tool behind the damage. The page reads the same verdict
            # on arrival and paints it; the rules gate Apply, not the door.
            print(designmod.serve(args.project_dir, port=args.port,
                                  open_browser=not args.no_open, profile=args.profile))
        elif args.command == "clean":
            build_dir = Path(args.project_dir) / "build"
            if build_dir.exists():
                shutil.rmtree(build_dir)
            print("Removed build/ (kept the toolchain cache and the CA).")
        elif args.command in ("build", "dev"):
            release = args.release and not args.debug
            if args.command == "dev":
                # Development keeps mutual TLS with a throwaway dev CA. Issued before the
                # validation below rather than after, so the certificate rule sees the
                # certificates dev is about to create instead of reporting them missing
                # and then creating them in the next breath.
                mesh.init(args.project_dir, dev=True, force=True)
                mesh.cert_all(args.project_dir,
                              _service_entities(_load_config(args.project_dir, args.profile)),
                              dev=True)
            # Fail fast, before anything is compiled or started. Without this the whole
            # validation contract in docs/project-layout-and-config.md only ever ran when
            # someone remembered to type `synqt check`, which is not where a plaintext
            # release edge or a literal database password gets caught.
            if _fails_validation(args.project_dir, release=release, profile=args.profile):
                return 1
            # Checked before anything compiles. A --deploy that is going to be refused for
            # not having said anything about signing should be refused in the first second,
            # not after a full release build of every entity.
            if getattr(args, "deploy", False):
                try:
                    deploymod.check_signing_choice(buildmod.desktop_platform(),
                                                   args.sign, args.unsigned)
                except deploymod.DeployError as err:
                    print(f"error: {err}")
                    return 1
            elif getattr(args, "sign", None) or getattr(args, "unsigned", False):
                print("error: --sign and --unsigned only mean something with --deploy.")
                return 1
            try:
                print(buildmod.build(args.project_dir, release=release, client=args.client,
                                     entity=getattr(args, "entity", None),
                                     threads=getattr(args, "threads", None),
                                     verbose=args.verbose, profile=args.profile,
                                     deploy=getattr(args, "deploy", False),
                                     sign=getattr(args, "sign", None)))
            except deploymod.DeployError as err:
                # The compile succeeded and only the opt-in deploy failed, so say which, or the
                # reader spends their time looking for a build error that is not there.
                print(f"error: --deploy: {err}")
                return 1
            if args.command == "dev":
                print()
                print(runmod.dev(args.project_dir, port=args.port,
                                 open_browser=not args.no_open, client=args.client,
                                 watch=not args.no_watch, profile=args.profile))
        elif args.command == "serve":
            # `synqt serve` runs the built artifacts as a deployment, so it holds them to
            # the release rules even though it does not build anything.
            if _fails_validation(args.project_dir, release=True, starting=True,
                                 profile=args.profile):
                return 1
            print(runmod.serve(args.project_dir, profile=args.profile))
        elif args.command == "test":
            return runmod.test(args.project_dir)
        elif args.command == "mesh":
            return _run_mesh(args)
        elif args.command == "docker":
            return _run_docker(args)
        elif args.command == "add":
            return _run_add(args)
        else:
            parser.error("unknown command")
    except (newproject.NewProjectError, create.CreateError, addauth.AddAuthError,
            addentity.AddEntityError,
            addprovider.AddProviderError, addcontract.AddContractError, mesh.MeshError,
            designmod.DesignError, infermod.InferError, typebackend.TypeBackendError,
            dockermod.DockerError, appmodel.AppGenError, buildmod.BuildError,
            configmod.ConfigError, FileNotFoundError) as error:
        print(f"synqt {args.command}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
