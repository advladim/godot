# Godot with Perforce

This fork of Godot 4.7.2 works with Perforce workspaces out of the box. Files no
longer need to be made writable by hand: the editor checks them out for you.

## What it does

When the project is inside a Perforce workspace, the editor:

| You do this in Godot                  | The editor runs              |
|---------------------------------------|------------------------------|
| Save a scene, resource, script, ...   | `p4 edit` (if read-only)     |
| Create a file (new scene, import, ...)| `p4 add`                     |
| Delete a file                         | `p4 delete` (or revert an add) |
| Rename / move a file                  | `p4 move` (history is kept)  |
| Move a folder                         | `p4 move folder/...`         |
| Reimport assets                       | one batched `p4 edit` for all `.import` files |

Files exclusively locked by a teammate (`+l`) can't be checked out; the editor
shows who holds the lock and the save fails instead of silently overwriting.

Files that are already writable but not checked out (for example in a
workspace with P4V's **allwrite** option, or a file made writable by hand) are
checked out in the background right after you save them, so they always end
up in your changelist.

`.import` and `.uid` files are never allowed to fail: if one is read-only, the
editor makes it writable immediately and checks it out in the background.

Everything goes into your **default changelist**.

## Right-click menu

Right-click files or folders in the **FileSystem** dock (or a scene tab) for:

- **Perforce: Check Out** — open the files for edit (folders: everything inside).
- **Perforce: Mark for Add** — add new files (folders: every new file inside,
  skipping what `.p4ignore` excludes).
- **Perforce: Revert** — discard your changes (asks first).

Right-clicking nodes in the **Scene** tree offers **Check Out Scene** and
**Revert Scene** for the open scene (and any instanced scenes you selected).

If Perforce is busy with something else, the action runs as soon as it's done.

## Automatic checkout

On by default. To turn it off, open the settings button (the wrench) in the
Perforce panel and untick **Check out and add files automatically when
saving**. It's saved per project.

When it's off, saving a file that isn't checked out stops with a message
telling you to check it out first (right-click > **Perforce: Check Out**), and
new files aren't marked for add. Deletes and renames still go through Perforce,
and `.import`/`.uid` files are still made writable so imports never fail.

## The Perforce panel

Open **Perforce** in the bottom panel, next to Output and Debugger. The tab
shows how many files you have checked out, e.g. **Perforce (3)**, and turns
yellow or red when something is wrong (logged out, server unreachable).

- **Status line:** whether you're connected, and to which workspace, user and
  server. It also shows **"N newer files on server"** when teammates have
  submitted.
- **Get Latest:** syncs the project. If a teammate changed a scene you're also
  editing, the two versions are merged automatically. When you both changed
  the *same* property, a dialog shows yours and theirs side by side, and you
  pick which to keep.
- **Checked out files:** everything you have open in this project.
  Double-click one to find it in the FileSystem dock. Select files and click
  **Revert** to discard your changes.
- **Submit:** type a description and press **Submit**. It saves all scenes
  first and submits only this project's files.
- **Activity:** a log of everything the editor did in Perforce (checkouts,
  adds, moves, merges, errors). If you ever wonder whether it's working, look
  here.
- **Settings button (the wrench):** opens **Perforce Connection** to change the
  server, user or workspace, or to log in.

History, shelving and anything more advanced stay in P4V.

Nothing changes in exported games: the Perforce code only exists in the editor,
so the standard 4.7.2 export templates work.

## Connecting

The editor uses the same settings as P4V and the `p4` command line (`P4PORT`,
`P4USER`, `P4CLIENT` from `p4 set` / P4CONFIG). If the current workspace
doesn't contain the project, it looks through your workspaces on this machine
and picks the one whose root contains it.

If you're logged in through P4V you're logged in here too. When the login has
expired, a **Perforce Connection** dialog asks for your password (it's sent to
`p4 login` and never stored). You can open it any time from
**Project > Tools > Perforce Connection...** to use a different server, user or
workspace for this project.

Requirements: the Perforce command line client (`p4`, installed with P4V) must
be on `PATH`, or set **Editor Settings > Version Control > Perforce > P4 Path**.

## Project setup (once per depot)

### `.p4ignore`

Put this next to `project.godot` and submit it. The `.godot/` folder is a local
cache and must never be in the depot.

```
.godot/
*.tmp
```

Each team member runs once:

```
p4 set P4IGNORE=.p4ignore
```

### Typemap

Text files Godot writes (scenes, resources, scripts, settings) can be merged, so
several people can work on them at once. Binary files can't be merged, so they
should be locked while someone edits them. Ask your Perforce admin to add this
to the typemap (`p4 typemap`):

```
TypeMap:
	text //....tscn
	text //....tres
	text //....gd
	text //....gdshader
	text //....godot
	text //....import
	text //....uid
	binary+l //....png
	binary+l //....jpg
	binary+l //....wav
	binary+l //....ogg
	binary+l //....mp3
	binary+l //....glb
	binary+l //....fbx
	binary+l //....blend
	binary+l //....scn
	binary+l //....res
```

## Merging scenes

`.tscn` and `.tres` files are text, but a line-based merge often breaks them.
This editor includes a merge tool that understands their structure: it matches
nodes, resources and connections by identity, so changes by different people
to different nodes or properties merge cleanly, even when both sides added
resources with the same id. Only the same property changed differently on both
sides is a conflict.

### Use it from P4V

**Edit > Preferences > Merge > Add...**

- Extension: `.tscn` (repeat for `.tres`)
- Application: path to this `godot.windows.editor.x86_64.exe`
- Arguments: `--headless --merge-scene %b %1 %2 %r`

### Use it from `p4 resolve`

Create `godot-merge.bat` next to the editor:

```bat
@echo off
"%~dp0godot.windows.editor.x86_64.exe" --headless --merge-scene %1 %2 %3 %4
```

and `p4 set P4MERGE=C:\path\to\godot-merge.bat`. Then choose `m` (merge) in
`p4 resolve`, and `ae` to accept the result.

The tool exits with 0 when everything merged, 1 when there were conflicts (it
keeps your version for those and prints them), and 2 on errors.

## Building

See [BUILDING.md](BUILDING.md) for building the editor and sharing it with
your team.

The Perforce integration is in `modules/perforce/`. The only engine changes are
the small `FileSystemWriteHook` in `core/io/` and the calls to it from
`FileAccess` and the platform `DirAccess` implementations, so rebasing onto a
new Godot release is cheap.

Tests: build with `tests=yes` and run
`godot.windows.editor.x86_64.exe --test --test-case="[SceneMerger]*"`.
