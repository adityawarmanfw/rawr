# Agent Guidelines

## General

- Use `uv` whenever Python or its libraries are needed.
- Keep temporary work in `./tmp`, not in the root or system `/tmp`.

## UI testing on device

- Drive the debug app with `scripts/ui`, not `uiautomator dump` + coordinates + screenshots.
  Debug builds publish every `testTag` as a resource-id; chain steps in one call:
  `scripts/ui seq "app" "tap capture_topbar_settings" "wait section:Image"`.
- Prefer `ui state` / `ui text <id>` over screenshots. `text:Foo` / `desc:Foo` selectors reach
  untagged controls and dialogs (popup windows get no resource-ids).
- `ui open <SettingsSection>` deep-links Settings; `ui audit` lists clickable nodes with no tag.
- The phone must be unlocked and awake; never enter credentials. End every run with `ui stop`.
- When adding a control, give it a `testTag` (see `CaptureTestTags`, `SettingsTestTags`).

## Git workflow

### Branches

- `origin/main` is the trunk. The remote squash-merges every PR, so `main` only
  ever gains one flattened commit per change.
- `dev` is a long-lived local archive branch. It must contain the complete
  development history — every granular commit, kept forever. It is never the
  source of a PR and is never used as the PR head.
- PR branches are disposable and always derived fresh from `origin/main`.

### PR branch rule

- A PR branch must contain exactly `origin/main` plus the commits for the work
  being proposed.
- It must never carry any of `dev`'s local commit history.
- If `origin/main` advances before the PR merges, the PR branch is rebased onto
  the new `origin/main`.

### Why the two histories coexist

- Because the remote is squash-only, `main` (squashed snapshots) and `dev`
  (granular commits) share no commits naturally.
- After each PR squash-merges, `origin/main` is folded into `dev` with
  `git merge -s ours` so `dev` records main's new commit while keeping its own
  detailed tree and history.

### The loop

1. Cut a clean PR branch from `origin/main`.
2. Put only the work commits on it (cherry-picked/rebased out of `dev` if that is
   where the work was authored).
3. Push and open the PR against `main`.
4. Squash merge on the remote.
5. Fetch and absorb the new `origin/main` into `dev`; keep `dev` as the permanent
   full-history archive.

### Commands

Open a PR:

```sh
git stash -u                       # keep uncommitted WIP out of the PR
git push -u origin <pr-branch>     # branch already based on origin/main
```

After the squash merge, sync and maintain `dev`:

```sh
git fetch origin
git switch main && git pull --ff-only
git switch dev && git merge -s ours origin/main
git switch -
```
