"""Prepare an isolated Unreal host, or explicitly build/open/run it later.

Python 3.10+, Git and Git LFS are required. Unreal 5.6 and 5.8 are supported targets.
No command runs automatically.

Suites: unit, integration and e2e ("all" runs each in its own editor session).
`run` serves an in-memory Git LFS lock server (lfs_lock_server.py) for the whole session.
The e2e suite drives the editor like a user and runs attended, so its dialogs reach the tests.

Create a workspace outside any existing Git repository:
  python Tests/framework.py prepare --engine ENGINE_ROOT --output-root SCRATCH_ROOT

Subsequent commands accept the workspace directory printed by prepare:
  python Tests/framework.py build --workspace WORKSPACE
  python Tests/framework.py open --workspace WORKSPACE
  python Tests/framework.py run --workspace WORKSPACE --suite unit

The engine installation is selected at preparation time. Logs and future
automation reports stay in the workspace. Workspaces are retained for diagnosis.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import uuid

from lfs_lock_server import LfsLockServer


REPOSITORY = Path(__file__).resolve().parents[1]
HOST_NAME = "GitSourceControlTestHost"
MARKER = "uegitplugin-workspace.json"
SUPPORTED_ENGINES = {(5, 6), (5, 8)}
PLUGIN_FILES = (
    "GitSourceControl.uplugin",
    "Source",
    "Resources",
    "LICENSE.txt",
    "git-lfs.exe",
    "git-lfs",
    "git-lfs-mac-amd64",
    "git-lfs-mac-arm64",
)


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def isolated_environment() -> dict[str, str]:
    # Inherited repository overrides must never redirect a fixture to the real checkout.
    environment = {key: value for key, value in os.environ.items() if not key.upper().startswith("GIT_")}
    environment.update(
        GIT_CONFIG_NOSYSTEM="1",
        GIT_CONFIG_GLOBAL=os.devnull,
        GIT_TERMINAL_PROMPT="0",
        GCM_INTERACTIVE="Never",
    )
    return environment


def git_repository_ancestor(path: Path) -> Path | None:
    return next((parent for parent in (path, *path.parents) if (parent / ".git").exists()), None)


def execute(command: list[str], directory: Path, timeout: int | None = 60) -> None:
    print(subprocess.list2cmdline(command), flush=True)
    options = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if sys.platform == "win32" else {"start_new_session": True}
    process = subprocess.Popen(command, cwd=directory, env=isolated_environment(), **options)
    try:
        return_code = process.wait(timeout=timeout)
    except (subprocess.TimeoutExpired, KeyboardInterrupt):
        # Compilers and editor workers must not outlive an interrupted invocation.
        if sys.platform == "win32":
            subprocess.run([str(Path(os.environ["SystemRoot"]) / "System32/taskkill.exe"),
                            "/PID", str(process.pid), "/T", "/F"], check=False, timeout=30)
        else:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.wait()
        raise
    if return_code:
        raise subprocess.CalledProcessError(return_code, command)


def prepare(engine: Path, output_root: Path) -> None:
    engine = engine.resolve()
    output_root = output_root.resolve()
    version = read_json(engine / "Engine/Build/Build.version")
    engine_version = (version["MajorVersion"], version["MinorVersion"])
    if engine_version not in SUPPORTED_ENGINES:
        raise ValueError("Select an Unreal Engine 5.6 or 5.8 installation.")
    if output_root == REPOSITORY or output_root.is_relative_to(REPOSITORY):
        raise ValueError("The workspace must be outside the plugin checkout.")
    if git_repository_ancestor(output_root):
        raise ValueError("The workspace must be outside any existing Git repository.")
    git = shutil.which("git")
    if not git:
        raise ValueError("Git must be installed and available on PATH.")

    workspace = output_root / f"ue-{engine_version[0]}.{engine_version[1]}-{uuid.uuid4().hex}"
    workspace.mkdir(parents=True, exist_ok=False)
    project = workspace / "HostProject"
    shutil.copytree(REPOSITORY / "Tests/HostProject", project, ignore=shutil.ignore_patterns(
        "Binaries", "Intermediate", "Saved", "DerivedDataCache", ".vs", ".git",
    ))
    plugin = project / "Plugins/GitSourceControl"
    plugin.mkdir(parents=True)
    for name in PLUGIN_FILES:
        source = REPOSITORY / name
        destination = plugin / name
        if source.is_dir():
            shutil.copytree(source, destination)
        else:
            shutil.copy2(source, destination)

    descriptor = project / f"{HOST_NAME}.uproject"
    project_settings = read_json(descriptor)
    project_settings["EngineAssociation"] = f"{engine_version[0]}.{engine_version[1]}"
    write_json(descriptor, project_settings)
    (project / ".gitignore").write_text(
        "Binaries/\nDerivedDataCache/\nIntermediate/\nSaved/\nPlugins/\n.vs/\n", encoding="utf-8",
    )
    # A local-only remote makes future destructive Git scenarios disposable.
    hooks = workspace / "EmptyHooks"
    hooks.mkdir()
    remote = workspace / "Remote.git"
    execute([git, "-c", f"core.hooksPath={hooks}", "init", "--bare", str(remote)], workspace)
    execute([git, "-C", str(remote), "symbolic-ref", "HEAD", "refs/heads/main"], workspace)
    execute([git, "-c", f"core.hooksPath={hooks}", "init", str(project)], workspace)
    for key, value in (
        ("core.hooksPath", str(hooks)),
        ("core.autocrlf", "false"),
        ("user.name", "Git Plugin Automation"),
        ("user.email", "automation@example.invalid"),
    ):
        execute([git, "-C", str(project), "config", "--local", key, value], workspace)
    execute([git, "-C", str(project), "symbolic-ref", "HEAD", "refs/heads/main"], workspace)
    execute([git, "-C", str(project), "add", "--all"], workspace)
    execute([git, "-C", str(project), "commit", "-m", "Initialize isolated test host"], workspace)
    execute([git, "-C", str(project), "remote", "add", "origin", str(remote)], workspace)
    execute([git, "-C", str(project), "push", "--set-upstream", "origin", "main"], workspace)
    settings = workspace / "Settings"
    settings.mkdir()
    (settings / "SourceControlSettings.ini").write_text(
        "[SourceControl.SourceControlSettings]\nUseGlobalSettings=False\nProvider=None\n", encoding="utf-8",
    )
    shutil.copy2(REPOSITORY / "Tests/suites.json", workspace / "suites.json")
    write_json(workspace / MARKER, {"schema": 1, "engine": str(engine), "source": str(REPOSITORY)})
    print(f"Workspace: {workspace}")


def load_workspace(path: Path) -> tuple[Path, Path, Path]:
    workspace = path.resolve()
    if workspace == REPOSITORY or workspace.is_relative_to(REPOSITORY):
        raise ValueError("Refusing to use the plugin checkout as a test workspace.")
    settings = read_json(workspace / MARKER)
    if settings.get("schema") != 1:
        raise ValueError("Unsupported workspace format.")
    project = workspace / "HostProject" / f"{HOST_NAME}.uproject"
    if project.resolve() != project or not project.is_file() or not (project.parent / ".git").is_dir():
        raise ValueError("The isolated host or its local repository is missing.")
    if git_repository_ancestor(project.parent) != project.parent:
        raise ValueError("The host must own its local Git repository.")
    return workspace, Path(settings["engine"]), project


def engine_tools(engine: Path) -> tuple[list[str], Path, Path]:
    if sys.platform == "win32":
        return ([str(engine / "Engine/Build/BatchFiles/Build.bat")],
                engine / "Engine/Binaries/Win64/UnrealEditor.exe",
                engine / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe")
    if sys.platform == "darwin":
        editor = engine / "Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
        return (["/bin/bash", str(engine / "Engine/Build/BatchFiles/Mac/Build.sh")], editor, editor)
    if sys.platform.startswith("linux"):
        editor = engine / "Engine/Binaries/Linux/UnrealEditor"
        return (["/bin/bash", str(engine / "Engine/Build/BatchFiles/Linux/Build.sh")], editor, editor)
    raise ValueError("Supported host platforms: Windows, macOS and Linux.")


def use_workspace(action: str, workspace_path: Path, suite: str, timeout: int, render: bool) -> None:
    workspace, engine, project = load_workspace(workspace_path)
    build, editor, command_editor = engine_tools(engine)
    if action == "build":
        platform = {"win32": "Win64", "darwin": "Mac"}.get(sys.platform, "Linux")
        execute(build + [f"{HOST_NAME}Editor", platform, "Development", f"-Project={project}",
                         "-WaitMutex", "-NoHotReloadFromIDE"], workspace, timeout)
        return

    # Keep the provider inactive until a future fixture explicitly selects it.
    common = [str(project), "-SCCProvider=None", "-NoSplash", "-NoSound",
              f"-UserDir={workspace / 'User'}",
              f"-SourceControlSettingsINI={workspace / 'Settings/SourceControlSettings.ini'}",
              f"-EditorSettingsINI={workspace / 'Settings/EditorSettings.ini'}"]
    if action == "open":
        execute([str(editor), *common, f"-abslog={workspace / 'Editor.log'}"], workspace, timeout=None)
        return

    suites = read_json(workspace / "suites.json")
    # "all" runs every suite in its own editor session, since E2E needs an attended editor and the others must not.
    selected = [name for name in suites if name != "all"] if suite == "all" else [suite]
    with LfsLockServer() as lock_server:
        execute([shutil.which("git") or "git", "-C", str(project.parent), "config", "--local", "lfs.url", lock_server.url],
                workspace)
        for name in selected:
            run_suite(workspace, command_editor, common, name, suites[name], timeout, render)


def run_suite(workspace: Path, command_editor: Path, common: list[str], name: str, automation_filter: str,
              timeout: int, render: bool) -> None:
    if not isinstance(automation_filter, str) or not all(
        part.isidentifier() for part in automation_filter.split(".")
    ) or automation_filter.split(".")[0] != "UEGitPlugin":
        raise ValueError("The automation filter must stay within the UEGitPlugin namespace.")
    report = workspace / "Reports" / uuid.uuid4().hex
    report.mkdir(parents=True)
    rendering = ["-RenderOffscreen"] if render else ["-NullRHI"]
    # E2E drives the editor like a user: dialogs must reach the tests' responder instead of auto-cancelling.
    attendance = [] if name == "e2e" else ["-Unattended"]
    execute([str(command_editor), *common, *rendering, *attendance, "-NoPause", "-StdOut",
             f"-abslog={report / 'Automation.log'}", f"-ReportExportPath={report}",
             f"-ExecCmds=Automation RunTests {automation_filter};Quit",
             "-TestExit=Automation Test Queue Empty"], workspace, timeout)
    result = read_json(report / "index.json")
    if result.get("failed", 0) or result.get("notRun", 0) or result.get("inProcess", 0):
        raise ValueError(f"Automation reported failed or incomplete tests in suite '{name}'; see {report}.")
    if result.get("succeeded", 0) + result.get("succeededWithWarnings", 0) == 0:
        raise ValueError(f"No tests executed in suite '{name}'; see {report}.")
    print(f"Report ({name}): {report}")


def positive_seconds(value: str) -> int:
    seconds = int(value)
    if seconds <= 0:
        raise argparse.ArgumentTypeError("The timeout must be greater than zero.")
    return seconds


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    actions = parser.add_subparsers(dest="action", required=True)
    preparation = actions.add_parser("prepare", help="Copy the host/plugin and create a local-only Git workspace.")
    preparation.add_argument("--engine", type=Path, required=True)
    preparation.add_argument("--output-root", type=Path, required=True)
    for action in ("build", "open", "run"):
        command = actions.add_parser(action, help=f"Explicitly {action} an already prepared workspace.")
        command.add_argument("--workspace", type=Path, required=True)
        if action != "open":
            command.add_argument("--timeout", type=positive_seconds, default=3600, help="Maximum duration in seconds.")
        if action == "run":
            command.add_argument("--suite", choices=tuple(read_json(REPOSITORY / "Tests/suites.json")), default="all")
            command.add_argument("--render", action="store_true", help="Enable offscreen rendering for future visual tests.")
    args = parser.parse_args()
    try:
        if args.action == "prepare":
            prepare(args.engine, args.output_root)
        else:
            use_workspace(args.action, args.workspace, getattr(args, "suite", "all"),
                          getattr(args, "timeout", 3600), getattr(args, "render", False))
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f"Framework error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
