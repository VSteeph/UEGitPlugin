# UEGitPlugin — Lock Manager

A lightweight Unreal Engine 5 fork of [Project Borealis's UEGitPlugin](https://github.com/ProjectBorealis/UEGitPlugin) focused solely on **Git LFS locking/unlocking** and **asset status tracking** in a team environment.

All Git operations (branches, commits, pushes, pulls, merges) must be done in an external Git client (Fork, GitKraken, GitHub Desktop, CLI). If you want a complete in-editor Git solution, head to the [original repository](https://github.com/ProjectBorealis/UEGitPlugin).

# Setup

## Prerequisites

* **Unreal Engine 5** (UE4 is not supported).
* **Git** and **Git LFS** installed and authenticated (HTTPS with [Git Credential Manager](https://github.com/GitCredentialManager/git-credential-manager) recommended).
* `*.uasset` and `*.umap` marked `lockable` in your repository root `.gitattributes`:

```gitattributes
*.uasset filter=lfs diff=lfs merge=lfs -text lockable
*.umap filter=lfs diff=lfs merge=lfs -text lockable
```

## Installation

1. Copy this plugin into your project's `Plugins/GitSourceControl` folder (or disable `Engine/Plugins/Developer/GitSourceControl` and install it in `Engine/Plugins/`).
2. Compile your project (Visual Studio / Rider / UBT) so the `GitSourceControl` plugin binaries are built.

## Configuration

1. Open Unreal Editor and click **Revision Control → Connect to Revision Control**.
2. Select **Git LFS 2** as the provider.
3. Verify the **Git Path** (auto-detected if left blank).
4. **Uses Git LFS** is enabled by default, and **LFS username** is pre-filled from your local Git `user.name`. Make sure it matches the lock owner name reported by your Git host (for example, your GitHub/GitLab username if it differs from your Git display name).
5. Click **Accept Settings**.

## Configuration Options

### Unreal Editor (`Config/DefaultEditorPerProjectUserSettings.ini`)

* **Disable automatic `git add` for new files** (recommended so your external Git client stays in control):

```ini
[/Script/UnrealEd.EditorLoadingSavingSettings]
bSCCAutoAddNewFiles=False
```

### Extra Remote Branches Checked (`RegisterStateBranches`)

By default, the plugin automatically checks the repository's remote base branch (`origin/HEAD`, or `develop` / `dev` / `main` / `master`) and your current upstream branch (`@{u}`).  
If you need extra branches checked (for example `origin/promoted`), call `ISourceControlProvider::RegisterStateBranches` from a custom `UUnrealEdEngine::Init` subclass.

# How to Use

## Lock Lifecycle

1. **Lock (`Check Out`):** Before editing an asset, check it out in Unreal. The plugin fetches the remote base and tracking branches, blocks if a teammate already holds the lock or if remote has a newer revision, and runs `git lfs lock`. Unlocked tracked files remain read-only on disk.
2. **Revert:** Discard local modifications in Unreal (`git reset` / `git checkout`).
   * Currently, **Revert** discards local edits and unlocks the file.
   * *(Planned)* Split into **Revert Changes** (discard edits, keep the lock) and **Revert and Unlock** (discard edits and release the lock).
3. **Manual Unlock:** Release a lock directly when discarding changes via **Revert** (or manually via `git lfs unlock` if you want to release a lock without reverting).
4. **Unlock Mode** *(planned)*: Automatic unlock policy configured in the plugin settings:
   * `OnMerge` *(default)*: automatically unlocks via Git hook when your commit lands on the remote base branch.
   * `OnPush`: automatically unlocks when you push.
   * `None`: manual unlock only.

## Other In-Editor Actions

* **Refresh:** Fetches targeted remote branches, refreshes `git lfs locks`, and updates Content Browser lock states.
* **History / Diff:** Views commit history and visual Blueprint diffs against previous revisions.

## Removed from Original Plugin

In-editor **Submit Content / Commit**, **Push**, **Pull / Sync**, **Changelists**, **auto-staging on save**, and **UE4 support** are intentionally removed.
