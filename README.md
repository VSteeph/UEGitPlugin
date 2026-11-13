# UEGitPlugin - Lock Manager

A lightweight Unreal Engine 5 fork of [Project Borealis's UEGitPlugin](https://github.com/ProjectBorealis/UEGitPlugin) focused solely on **Git LFS locking/unlocking** and **asset status tracking** in a team environment.

All Git operations (branches, commits, pushes, pulls, merges) must be done in an external Git client (Fork, GitKraken, GitHub Desktop, CLI). If you want a complete in-editor Git solution, head to the [original repository](https://github.com/ProjectBorealis/UEGitPlugin).

# Setup

## Prerequisites

* **Unreal Engine 5.6 or 5.8**.
* **Git 2.28 or newer** (automatic unlock relies on the `reference-transaction` hook) and **Git LFS** installed and authenticated (HTTPS with [Git Credential Manager](https://github.com/GitCredentialManager/git-credential-manager) recommended).
* `*.uasset` and `*.umap` marked `lockable` in your repository root `.gitattributes`:

```gitattributes
*.uasset filter=lfs diff=lfs merge=lfs -text lockable
*.umap filter=lfs diff=lfs merge=lfs -text lockable
```

If Git LFS is missing or the assets are not `lockable`, the plugin still connects but warns at connection ("Please activate and/or configure Git LFS") and asset locking stays disabled until it is fixed.

## Installation

1. Copy this plugin into your project's `Plugins/GitSourceControl` folder (or disable `Engine/Plugins/Developer/GitSourceControl` and install it in `Engine/Plugins/`).
2. Compile your project (Visual Studio / Rider / UBT) so the `GitSourceControl` plugin binaries are built.

## Configuration

1. Open Unreal Editor and click **Revision Control → Connect to Revision Control**.
2. Select **Git LFS 2** as the provider.
3. Verify the **Git Path** (auto-detected if left blank). Git LFS must be installed and available to Git.
4. **LFS Username** is pre-filled with the lock owner name reported by your Git host once the plugin has seen one of your locks, otherwise with your local Git `user.name`.
   Make sure it matches the lock owner name reported by your Git host (for example, your GitHub/GitLab username if it differs from your Git display name).
   The pre-filled value is only saved once you confirm the field.
5. **Unlock Mode** is a team choice (see [Unlock Mode](#unlock-mode)).
   The first team member to connect picks it in this window; it is then saved in `Config/DefaultEditor.ini`, which they commit.
   Everyone else sees the team's choice here and is never asked again.
6. Click **Accept Settings**.

## Configuration Options

### Unlock Mode (`Config/DefaultEditor.ini`, versioned)

```ini
[GitSourceControl]
UnlockMode=OnMerge
```

Accepted values: `OnMerge`, `OnPush`, `None`.
The plugin reads this file directly, so a per-user setting can never override the team's choice.
When the key is missing, `OnMerge` applies until a team member picks a mode.
After changing the value (or pulling a change to it), reconnect to Revision Control to update the Git hook.

### Outdated Assets

The plugin checks your assets against the repository's remote base branch (`origin/HEAD`, or `develop` / `dev` / `main` / `master`) and your current upstream branch (`@{u}`).

# How to Use

## Lock Lifecycle

1. **Lock (`Check Out`):** Before editing an asset, check it out in Unreal. The plugin fetches the remote base and tracking branches, blocks if a teammate already holds the lock or if remote has a newer revision, and runs `git lfs lock`. Unlocked tracked files remain read-only on disk.
2. **Revert:** Discard local modifications, with or without releasing the lock (see [Revert](#revert)).
3. **Manual Unlock:** Use **Revert and Unlock**, or `git lfs unlock` from a terminal.
4. <a id="unlock-mode"></a>**Unlock Mode:** Automatic unlock policy shared by the team in `Config/DefaultEditor.ini` and enforced by a managed Git `reference-transaction` hook.
   The hook is installed in each clone when Unreal connects to Revision Control; it then also works when Unreal is closed.
   * `OnMerge` *(default)*: automatically unlocks files changed on the remote base branch when their local working copy is clean and identical.
   * `OnPush`: automatically unlocks files as soon as they reach any remote branch (seen by a push or a fetch) when their local working copy is clean and identical.
   * `None`: manual unlock only.
   Files with uncommitted local modifications are never unlocked automatically.

## Asset Icons

The Content Browser shows one status icon per asset, at the bottom right of its thumbnail.
Hover the asset to see who holds the lock and since when.

| Icon | Meaning |
| --- | --- |
| *(none)* | Free: nobody holds a lock. |
| ![Locked by you](Resources/Readme/LockSelf.png) | Locked by you: you can edit and save the asset. |
| ![Locked by another user](Resources/Readme/LockOther.png) | Locked by another user: you cannot check it out. |
| ![Outdated](Resources/Readme/Outdated.png) | Outdated: a newer version exists on your remote branch or on the base branch. Update before locking it. |
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

In-editor **Submit Content / Commit**, **Push**, **Pull / Sync**, **Changelists**, **staging** (`git add` / `git rm` on save, new asset, delete or rename), **conflict resolution**, **status branches**, the **`git` console command**, the **bundled Git LFS binaries** and **UE4 support** are intentionally removed.
New, deleted and renamed assets show up in your Git client like any other change.
