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
4. **Uses Git LFS** is enabled by default.
   **LFS username** is pre-filled with the lock owner name reported by your Git host once the plugin has seen one of your locks, otherwise with your local Git `user.name`.
   Make sure it matches the lock owner name reported by your Git host (for example, your GitHub/GitLab username if it differs from your Git display name).
   The pre-filled value is only saved once you confirm the field.
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
2. **Revert:** Discard local modifications, with or without releasing the lock (see [Revert](#revert)).
3. **Manual Unlock:** Use **Revert and Unlock**, or `git lfs unlock` from a terminal.
4. **Unlock Mode** *(planned)*: Automatic unlock policy configured in the plugin settings:
   * `OnMerge` *(default)*: automatically unlocks via Git hook when your commit lands on the remote base branch.
   * `OnPush`: automatically unlocks when you push.
   * `None`: manual unlock only.

## Asset Icons

The Content Browser shows one status icon per asset, at the bottom right of its thumbnail.
Hover the asset to see who holds the lock and since when.

| Icon | Meaning |
| --- | --- |
| *(none)* | Free: nobody holds a lock. |
| ![Locked by you](Resources/Readme/LockSelf.png) | Locked by you: you can edit and save the asset. |
| ![Locked by another user](Resources/Readme/LockOther.png) | Locked by another user: you cannot check it out. |
| ![Outdated](Resources/Readme/Outdated.png) | Outdated: a newer version exists on your remote branch or on another branch (base branch, extra checked branches). Update before locking it. |
| ![Locked by you, outdated](Resources/Readme/LockSelfOutdated.png) | Locked by you and outdated. |
| ![Locked by another user, outdated](Resources/Readme/LockOtherOutdated.png) | Locked by another user and outdated. |

The outdated arrow stacks on the lock icon as a badge.
When the newer version is on another branch, the tooltip names that branch.

## Revert

Revert discards local modifications (`git reset` / `git checkout`).
Each action exists in two flavors: keep the lock, or release it.

| Action | Where | Scope | Lock |
| --- | --- | --- | --- |
| **Revert Changes** | Right-click an asset in the Content Browser → **Revision Control** | Selected assets only | Kept |
| **Revert and Unlock** | Right-click an asset in the Content Browser → **Revision Control** | Selected assets only | Released |
| **Revert All Assets** | **Revision Control** menu (status bar, bottom right of the editor) → **Git** section | Every modified asset (`.uasset`, `.umap`) of the project | Kept |
| **Revert and Unlock All Assets** | **Revision Control** menu (status bar, bottom right of the editor) → **Git** section | Every modified or locked asset (`.uasset`, `.umap`) of the project | Released |

The global actions never touch config files, code, plugins sources or the `.uproject`.
Both global actions ask for confirmation and are disabled when there is nothing to revert.
**Revert and Unlock** on an unmodified asset simply releases its lock.

## Other In-Editor Actions

* **Refresh:** Fetches targeted remote branches, refreshes `git lfs locks`, and updates Content Browser lock states.
* **History / Diff:** Views commit history and visual Blueprint diffs against previous revisions.

## Removed from Original Plugin

In-editor **Submit Content / Commit**, **Push**, **Pull / Sync**, **Changelists**, **auto-staging on save**, and **UE4 support** are intentionally removed.
