# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The ``synqt`` command-line interface: `synqt new`, `synqt dev`, `synqt build`, the mesh
certificate tooling and the scaffolders.
"""

from __future__ import annotations

import argparse
import getpass
import json
import shutil
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from . import (addauth, addcontract, addentity, addprovider, appmodel,
               build as buildmod, check as checkmod, clientbuild,
               config as configmod, create, deploy as deploymod, design as designmod,
               docker as dockermod, doctor, examples as examplesmod,
               infer as infermod, mesh,
               monitorops, newproject, profiles,
               run as runmod, typebackend, version as versionmod)


def _load_config(project_dir: str, profile: Optional[str] = None) -> Dict[str, Any]:
    return configmod.load(project_dir, profile=profile)


def _service_entities(config: Dict[str, Any]) -> List[str]:
    return [e.get("name") for e in config.get("entities", [])
            if appmodel.is_service(e)]


class _PrintVersionAction(argparse.Action):
    """Print `version.version_lines()` as three lines for `--version`. argparse's
    ``action="version"`` would reflow them into one paragraph.
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
    # `synqt version` is the documented form. `--version` and `-V` also work, hidden from
    # the help.
    parser.add_argument("--version", "-V", action=_PrintVersionAction,
                        help=argparse.SUPPRESS)
    sub = parser.add_subparsers(dest="command", required=False)

    sub.add_parser("version", help="print the CLI version and the pinned toolchain")

    new = sub.add_parser("new", help="scaffold a new project")
    new.add_argument("name")
    # No --origin-model: a scaffolded project is same-origin, the only shape with a
    # first-party session cookie. `split_origin` needs a hand edit (see "Serving the client
    # from another origin" in docs/project-layout-and-config.md).
    new.add_argument("--auth", default=None, help="provider to prime auth for")
    # No starting-entity flag: run `synqt add entity <name> --type <type>` after creation;
    # `synqt create` asks for both. `--example` copies one of the shipped systems (`synqt
    # examples` lists them).
    new.add_argument("--example", default=None,
                     help="start from a shipped example (see 'synqt examples')")
    new.add_argument("--parent-dir", default=".")

    # The interactive twin of `new`. `create` refuses to run without a terminal.
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
                           ("providers", "list bundled providers per family"),
                           ("examples", "list the example systems 'synqt new' can copy")]:
        p = sub.add_parser(name, help=helptext)
        if name not in ("providers", "examples"):
            p.add_argument("--project-dir", default=".")
        if name in ("dev", "design", "build", "serve", "check", "infer", "doctor"):
            # Commands that read the topology take the profile layered over it
            # (docs/project-layout-and-config.md, "Configuration resolution order").
            # `clean`, `providers` and `test` read no configuration.
            p.add_argument("--profile", default=None, metavar="NAME",
                           help="layer synqt.<NAME>.yaml over synqt.yaml")
        if name == "check":
            # The release-only rules (TLS to the browser, mutual TLS off-machine, a wss
            # desktop edge URL) are opt-in here and automatic on `build --release` and
            # `serve`.
            p.add_argument("--release", action="store_true",
                           help="also apply the rules a release build and serve apply")
        if name == "infer":
            # Report by default; write only when asked.
            p.add_argument("--write", action="store_true",
                           help="write each contract into its owner's folder")
            p.add_argument("--force", action="store_true",
                           help="with --write, overwrite a contract that is already there")
            p.add_argument("--json", action="store_true",
                           help="print the result as a design document instead of a report")
        if name in ("infer", "check"):
            # Who types expressions: `auto` uses TypeScript when installed, `ts` requires
            # it, `heuristic` reads literals only. `check` takes it too, to compare
            # contracts with calls.
            p.add_argument("--types", default="auto", choices=list(typebackend.MODES),
                           help="who answers what type an expression has (default: auto)")
        if name in ("dev", "build"):
            # The build profile, default debug. Not on `serve`, which launches
            # build/<entity>/ whatever profile was built last.
            profile_flags = p.add_mutually_exclusive_group()
            profile_flags.add_argument(
                "--release", dest="profile_name", action="store_const", const="release",
                help="optimise for each artifact's own environment, and leave no symbols")
            profile_flags.add_argument(
                "--debug", dest="profile_name", action="store_const", const="debug",
                help="the default: symbols kept, nothing optimised away")
            profile_flags.add_argument(
                "--custom", dest="custom_type", metavar="TYPE",
                choices=list(profiles.CUSTOM_TYPES),
                help="name a CMake build type yourself (%s)" % ", ".join(
                    profiles.CUSTOM_TYPES))
            p.set_defaults(profile_name="debug", custom_type=None)
            p.add_argument("--strip", action="store_true",
                           help="leave no symbols in the binaries; implied by --release")
            # `none` builds the services and no client, so no Emscripten kit is needed (for
            # `synqt docker init --client host`).
            p.add_argument("--client", default="wasm",
                           choices=["wasm", "desktop", "all", "none"])
            p.add_argument("--verbose", action="store_true",
                           help="echo each build command and stream its output")
        if name == "build":
            p.add_argument("--entity", default=None,
                           help="build one entity instead of every one")
            # Off by default: signing and notarization are the project's choice
            # (docs/desktop.md). This runs the platform deploy step; it never signs by
            # itself.
            p.add_argument("--deploy", action="store_true",
                           help="also run the platform deploy step on a desktop client "
                                "(macdeployqt/windeployqt/portable layout); requires "
                                "--sign or --unsigned")
            # --deploy alone is refused. deploy.check_signing_choice says what an unsigned
            # build costs on this platform.
            p.add_argument("--sign", default=None, metavar="IDENTITY",
                           help="sign the deployed client with this identity (macOS: a "
                                "codesign identity; Windows: the certificate subject name)")
            p.add_argument("--unsigned", action="store_true",
                           help="deploy without signing, accepting what that means on this "
                                "platform")
            # Not on `dev`, which re-reads synqt.yaml on every hot reload and would drop an
            # argv override. For dev, set build.client_threads.
            p.add_argument("--threads", default=None, choices=list(clientbuild.MODES),
                           help="override build.client_threads for this build "
                                "(multi implies cross-origin isolation)")
        if name == "dev":
            p.add_argument("--desktop", action="store_true", help="run the client natively")
            # Replaces every sign-in with one page listing the scopes. `dev` only: the edge
            # it starts is built with SYNQT_DEV_TOOLS, the only build that contains the
            # picker.
            p.add_argument("--identity-picker", action="store_true",
                           help="replace every sign-in with a picker listing the "
                                "project's scopes (development only)")
            p.add_argument("--port", type=int, default=8080, help="the local dev port")
            p.add_argument("--no-open", action="store_true", help="do not open a browser")
            p.add_argument("--no-watch", action="store_true",
                           help="serve once without watching for changes")
        if name == "design":
            # Its own port, so the editor and `synqt dev` can run together.
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
        # `status` takes --project-dir like its siblings.
        mp.add_argument("--project-dir", default=".")
    for mp in (mi, mc, mr):
        # A profile may add an entity, so `mesh cert --all` resolves the same entity list as
        # the build. `status` only reports certificate files.
        mp.add_argument("--profile", default=None, metavar="NAME",
                        help="layer synqt.<NAME>.yaml over synqt.yaml")
    meshp.set_defaults(project_dir=".", profile=None)

    # `synqt monitor` manages console operator credentials. Only `operator add` exists; the
    # list lives in the monitor environment.
    monitorp = sub.add_parser("monitor", help="the monitoring console's operators")
    monitor_sub = monitorp.add_subparsers(dest="monitor_command", required=True)
    mop = monitor_sub.add_parser("operator", help="operator credentials")
    mop_sub = mop.add_subparsers(dest="operator_command", required=True)
    moa = mop_sub.add_parser("add", help="mint one operator credential")
    moa.add_argument("name")
    moa.add_argument("--password-stdin", action="store_true",
                     help="read the password from stdin instead of prompting")
    moa.add_argument("--project-dir", default=".")
    monitorp.set_defaults(project_dir=".")

    # `synqt docker`: generate the container setup and drive it. `init` writes; `up` and
    # `down` run `docker compose` with the profile and two pre-checks.
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
    # The flag decides whether to prompt (see create.py). Questions only cover external
    # secrets.
    di.add_argument("--no-input", action="store_true",
                    help="ask nothing; leave a placeholder for every secret from outside")
    du = docker_sub.add_parser("up", help="build the images and start every container")
    du.add_argument("--detach", "-d", action="store_true", help="start in the background")
    du.add_argument("--no-build", action="store_true",
                    help="start what is already built instead of rebuilding first")
    dd = docker_sub.add_parser("down", help="stop every container")
    dd.add_argument("--volumes", action="store_true",
                    help="also remove the mesh CA and any engine data (a clean slate)")
    dca = docker_sub.add_parser(
        "ca", help="copy out the development authority the browser link is signed by")
    for dp in (di, du, dd, dca):
        dp.add_argument("--project-dir", default=".")
    dockerp.set_defaults(project_dir=".")

    add = sub.add_parser("add", help="add a capability to the project")
    add_sub = add.add_subparsers(dest="what", required=True)
    auth = add_sub.add_parser("auth"); auth.add_argument("provider")
    auth.add_argument("--required", action="store_true")
    auth.add_argument("--provider-entity", default="")
    entity = add_sub.add_parser("entity"); entity.add_argument("name")
    # Defaulted: an entity with no type is a plain service.
    entity.add_argument("--type", dest="entity_type", default=appmodel.PLAIN_TYPE,
                        choices=sorted(addentity.TYPES),
                        help="what the entity is (default: %(default)s)")
    entity.add_argument("--provider")
    provider = add_sub.add_parser("provider"); provider.add_argument("name")
    provider.add_argument("--family", required=True)
    connect_point = add_sub.add_parser("connect-point")
    # The owner is the name; consumers reach it as the owner capitalized.
    connect_point.add_argument("owner")
    connect_point.add_argument("--consumers", default="", help="comma-separated entity names")
    for ap in (auth, entity, provider, connect_point):
        ap.add_argument("--project-dir", default=".")
    return parser


def resolved_profile(args: argparse.Namespace) -> Tuple[str, str]:
    """The profile these arguments ask for, as `(profile_name, custom_type)`. `--custom` wins
    and sets `custom_type`; otherwise `profile_name`, which argparse defaults to `debug`.
    """
    custom = getattr(args, "custom_type", None)
    if custom:
        return "custom", custom
    return getattr(args, "profile_name", "debug"), ""


def _fails_validation(project_dir: str, *, release: bool, starting: bool = False,
                      profile: Optional[str] = None) -> bool:
    """Run the topology validation ahead of a build or a run, and report it.

    Only validate() runs here, not the full `synqt check`: the lints run qmllint over every
    QML file, and `synqt dev` calls this on every rebuild.
    """
    if not (Path(project_dir) / "synqt.yaml").exists():
        return False  # not a project yet. The command below reports that in its own words
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
        message = addentity.scaffold(args.project_dir, args.name, args.entity_type,
                                     provider=args.provider)
    elif args.what == "provider":
        message = addprovider.scaffold(args.project_dir, args.name, args.family)
    else: # connect-point
        consumers = [c for c in args.consumers.split(",") if c]
        message = addcontract.scaffold_connect_point(
            args.project_dir, args.owner, consumers=consumers)
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


def _run_monitor(args: argparse.Namespace) -> int:
    if args.monitor_command != "operator" or args.operator_command != "add":
        raise monitorops.MonitorOpsError("unknown monitor command")
    if args.password_stdin:
        password = sys.stdin.readline().rstrip("\n")
    else:
        # getpass, so the password is not echoed and does not end up in a shell history.
        password = getpass.getpass(f"password for operator '{args.name}': ")
        if password != getpass.getpass("repeat: "):
            raise monitorops.MonitorOpsError("the two passwords do not match")
    print(monitorops.instructions(monitorops.mint(args.name, password)))
    return 0


def _run_docker(args: argparse.Namespace) -> int:
    if args.docker_command == "init":
        # Validate the container topology before writing it.
        if _fails_validation(args.project_dir, release=False):
            return 1
        config = _load_config(args.project_dir)
        print(dockermod.init(args.project_dir, config, force=args.force,
                             subnet=args.subnet, client=args.client, port=args.port,
                             source=None if args.no_input else sys.stdin))
        return 0
    if args.docker_command == "ca":
        print(dockermod.export_ca(args.project_dir))
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
        if args.command == "version":
            print("\n".join(versionmod.version_lines()))
        elif args.command == "new":
            if args.example:
                if args.auth:
                    # An example carries its own identity block, or none; it is not primed.
                    raise examplesmod.ExampleError(
                        "--auth and --example cannot be used together: an example already "
                        "says whether it signs people in. Copy it, then run 'synqt add "
                        "auth <provider>' if you want to change that.")
                print(examplesmod.scaffold(args.parent_dir, args.name, args.example))
            else:
                print(newproject.scaffold(args.parent_dir, args.name, auth=args.auth))
        elif args.command == "create":
            print(create.create(args.parent_dir, name=args.name))
        elif args.command == "providers":
            print(addentity.list_providers())
        elif args.command == "examples":
            print(examplesmod.listing())
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
                    print("\nWrite these into their owners' folders with: "
                          "synqt infer --write")
            if args.write:
                written = infermod.write(args.project_dir, edges, config,
                                         force=args.force)
                # With --json, what was written goes to stderr.
                for path in written:
                    print(f"wrote {path}", file=sys.stderr if args.json else sys.stdout)
        elif args.command == "design":
            # No validation gate: the editor is where a broken topology gets fixed. The page
            # shows the verdict; the rules gate Apply.
            print(designmod.serve(args.project_dir, port=args.port,
                                  open_browser=not args.no_open, profile=args.profile))
        elif args.command == "clean":
            # Both trees SynQt writes: the build output and the generated sources.
            removed = []
            for name in ("build", appmodel.GENERATED_DIR):
                target = Path(args.project_dir) / name
                if target.exists():
                    shutil.rmtree(target)
                    removed.append(f"{name}/")
            print(f"Removed {' and '.join(removed) or 'nothing'} "
                  "(kept the toolchain cache and the CA).")
        elif args.command in ("build", "dev"):
            profile_name, custom_type = resolved_profile(args)
            # Release rules bind a shipped artifact only; a debug build is held to the
            # localhost rules.
            release = profile_name == "release"
            if args.command == "dev":
                # Development keeps mutual TLS with a throwaway dev CA, issued before
                # validation so the certificate rule sees it.
                mesh.init(args.project_dir, dev=True, force=True)
                mesh.cert_all(args.project_dir,
                              _service_entities(_load_config(args.project_dir, args.profile)),
                              dev=True)
            # Fail fast, before anything is compiled or started.
            if _fails_validation(args.project_dir, release=release, profile=args.profile):
                return 1
            # Check the signing choice before compiling.
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
                print(buildmod.build(args.project_dir, profile_name=profile_name,
                                     custom_type=custom_type, strip=args.strip,
                                     dev_tools=(args.command == "dev"), client=args.client,
                                     entity=getattr(args, "entity", None),
                                     threads=getattr(args, "threads", None),
                                     verbose=args.verbose, profile=args.profile,
                                     deploy=getattr(args, "deploy", False),
                                     sign=getattr(args, "sign", None)))
            except deploymod.DeployError as err:
                # The compile succeeded and only the deploy failed; say so.
                print(f"error: --deploy: {err}")
                return 1
            if args.command == "dev":
                print()
                print(runmod.dev(args.project_dir, port=args.port,
                                 open_browser=not args.no_open, client=args.client,
                                 watch=not args.no_watch, profile=args.profile,
                                 profile_name=profile_name,
                                 identity_picker=args.identity_picker))
        elif args.command == "serve":
            # `synqt serve` runs the artifacts as a deployment, so it applies the release
            # rules.
            if _fails_validation(args.project_dir, release=True, starting=True,
                                 profile=args.profile):
                return 1
            print(runmod.serve(args.project_dir, profile=args.profile))
        elif args.command == "test":
            return runmod.test(args.project_dir)
        elif args.command == "mesh":
            return _run_mesh(args)
        elif args.command == "monitor":
            return _run_monitor(args)
        elif args.command == "docker":
            return _run_docker(args)
        elif args.command == "add":
            return _run_add(args)
        else:
            parser.error("unknown command")
    except (newproject.NewProjectError, create.CreateError, addauth.AddAuthError,
            addentity.AddEntityError, examplesmod.ExampleError,
            addprovider.AddProviderError, addcontract.AddContractError, mesh.MeshError,
            designmod.DesignError, infermod.InferError, typebackend.TypeBackendError,
            dockermod.DockerError, appmodel.AppGenError, buildmod.BuildError,
            monitorops.MonitorOpsError,
            configmod.ConfigError, FileNotFoundError) as error:
        print(f"synqt {args.command}: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
