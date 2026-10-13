# Building and sharing the Perforce editor

How to build this Godot fork on Windows and get it to your team.
**On Linux?** Jump to [Building on Linux](#building-on-linux).

**Only one person has to build.** Teammates just get the finished `.exe` and
don't need any of the build tools.

---

## Part 1: Set up a build machine (once)

You need these four things. Everything below is free.

1. **Visual Studio 2022 Build Tools** (or Visual Studio 2022 Community)
   - Download from <https://visualstudio.microsoft.com/downloads/> (under
     "Tools for Visual Studio").
   - In the installer, tick **Desktop development with C++**. Keep the default
     components, which include the MSVC compiler and the Windows SDK.
2. **Python 3.8 or newer** from <https://www.python.org/downloads/>. Tick
   **Add Python to PATH** during install.
3. **SCons**, Godot's build tool. In a terminal, run:
   ```
   python -m pip install scons
   ```
4. **Git** from <https://git-scm.com/downloads>.

Plan for about **10 GB of free disk space**. Pick a drive with room, not a nearly
full C:.

## Part 2: Get the source (once)

```
git clone https://github.com/advladim/godot.git "B:\Godot\Godot Fork"
cd "B:\Godot\Godot Fork"
git checkout perforce
```

The `perforce` branch is Godot 4.7.2-stable with the Perforce integration added.

## Part 3: Build the editor

Run this from the source folder:

```
set BUILD_NAME=perforce
python -m SCons platform=windows target=editor production=yes d3d12=no accesskit=no
```

| Option | Why |
|---|---|
| `BUILD_NAME=perforce` | The version shows as `4.7.2.stable.perforce` in **Help > About**. That makes it easy to tell whether someone is on the team build. |
| `target=editor` | Builds the editor, not an export template. |
| `production=yes` | Optimized like the official releases. The build is slower and needs more RAM, but the editor runs faster. Leave it off while you're changing the code. |
| `d3d12=no accesskit=no` | Skips two optional features that need extra SDKs. The editor still renders with Vulkan or OpenGL, like normal. See [Optional extras](#optional-extras). |

**How long it takes:** the first build takes about 15–30 minutes with
`production=yes` (more on slower PCs). After that, only changed files are
rebuilt, so later builds are much faster. SCons uses all CPU cores by default;
add `-j4` to use fewer.

**The result** is in `bin\`:

```
bin\godot.windows.editor.x86_64.exe          <- the editor
bin\godot.windows.editor.x86_64.console.exe  <- same editor with a console window (shows logs)
```

### Check that it works

1. Run `bin\godot.windows.editor.x86_64.exe` and open a project that's in your
   Perforce workspace.
2. You should see a toast: **"Perforce: using workspace ..."**
3. Edit a scene and save it. In P4V it should now show as checked out in your
   default changelist.

## Part 4: Give it to your teammates

### What to send

Make a folder, for example `Godot-4.7.2-perforce`, containing:

```
Godot-4.7.2-perforce\
  godot.windows.editor.x86_64.exe
  godot.windows.editor.x86_64.console.exe
  godot-merge.bat        (optional, for p4 resolve, see PERFORCE.md)
  PERFORCE.md
```

Zip it and share it. Two good options:

- **Your Perforce depot (recommended):** submit the zip, or the unzipped folder,
  to a tools path such as `//depot/tools/godot/`. Everyone gets the same version
  with **Get Latest**, and updating the editor is just a new submit. Godot
  editors are around 150 MB, so check with your server admin first.
- **GitHub Releases:** on <https://github.com/advladim/godot/releases>, click
  **Draft a new release**, create a tag such as `4.7.2-perforce-1`, and attach the
  zip. Teammates download it from there.

Avoid sending the `.exe` over chat apps. They often block executables.

### What each teammate needs

1. **P4V with the command-line client.** The P4V installer includes `p4.exe`;
   make sure **Command-Line Client (P4)** is ticked during install. To check, open
   a terminal and run `p4 -V`.
2. **The editor folder** from above, unzipped anywhere, for example
   `C:\Tools\Godot-4.7.2-perforce\`.
3. **A one-time setting:**
   ```
   p4 set P4IGNORE=.p4ignore
   ```
4. **Export templates, only for exporting games:** in the editor, go to
   **Editor > Manage Export Templates > Download and Install**. The standard
   4.7.2 templates work, because the Perforce code only exists in the editor.

Nothing else is needed: no Python, no Visual Studio, and no Perforce setup in
Godot. The editor uses the same login and workspace as P4V.

### First launch on Windows

The `.exe` isn't code-signed, so Windows SmartScreen may show
**"Windows protected your PC"**. Click **More info > Run anyway**. This only
happens once per file.

### Everyone must use the same editor

Mixing this build with the official 4.7.2 editor in the same project is safe:
the scene format is identical. But anyone on the official editor gets the old
read-only problems back, so the whole team should switch.

## Part 5: Updating

**When you change the fork:**

```
cd "B:\Godot\Godot Fork"
git pull
set BUILD_NAME=perforce
python -m SCons platform=windows target=editor production=yes d3d12=no accesskit=no
```

Then share the new `bin\` files the same way as before, and bump the release
number (`4.7.2-perforce-2`, ...) so people know which version they're on.

**When Godot releases a new version (e.g. 4.7.3):**

```
git fetch upstream --tags
git checkout -b perforce-4.7.3 perforce
git rebase --onto 4.7.3-stable 4.7.2-stable
```

This replays the Perforce commits on top of the new release.

(One time only, add the upstream remote:
`git remote add upstream https://github.com/godotengine/godot.git`)

The Perforce changes are small and almost all live in `modules/perforce/`, so
conflicts are rare. Only the team's builder needs to do this. Everyone else
just gets the new zip. Upgrade the whole team at once, since newer Godot versions
can update project files in ways older editors don't understand.

## Building on Linux

The same source builds on Linux; Perforce support works the same way there.

### 1. Install the build tools (once)

Ubuntu / Debian / Mint:

```bash
sudo apt-get install -y build-essential scons pkg-config git libx11-dev libxcursor-dev libxinerama-dev libgl1-mesa-dev libglu1-mesa-dev libasound2-dev libpulse-dev libudev-dev libxi-dev libxrandr-dev libwayland-dev
```

Fedora:

```bash
sudo dnf install -y scons pkgconfig git gcc-c++ libstdc++-static libatomic-static libX11-devel libXcursor-devel libXrandr-devel libXinerama-devel libXi-devel wayland-devel mesa-libGL-devel mesa-libGLU-devel alsa-lib-devel pulseaudio-libs-devel libudev-devel
```

Arch / Manjaro:

```bash
sudo pacman -S --needed scons pkgconf gcc git libxcursor libxinerama libxi libxrandr wayland-utils mesa glu libglvnd alsa-lib pulseaudio
```

If your distribution's SCons is old, use `python3 -m pip install --user scons` instead.

### 2. Get the source and build

```bash
git clone -b perforce https://github.com/advladim/godot.git godot-perforce
cd godot-perforce
BUILD_NAME=perforce scons platform=linuxbsd target=editor production=yes accesskit=no
```

This takes 15–40 minutes the first time (add `-j4` to limit CPU use). The
editor ends up at `bin/godot.linuxbsd.editor.x86_64`. Give it a clear name and
run it:

```bash
cp bin/godot.linuxbsd.editor.x86_64 ~/godot_perforce_linux
~/godot_perforce_linux
```

### 3. Install the Perforce command line client

Godot calls `p4` to talk to Perforce. P4V for Linux doesn't always include it,
so download **Helix Core Command-Line Client (P4)** for Linux from
<https://www.perforce.com/downloads/helix-command-line-client-p4>, then:

```bash
chmod +x p4
sudo mv p4 /usr/local/bin/
p4 -V
p4 set P4IGNORE=.p4ignore
```

Set up the connection once (`p4 set P4PORT=...`, `p4 set P4USER=...`,
`p4 set P4CLIENT=...`) or log in through P4V; the editor uses the same
settings. On Linux, `p4 set` stores them in `~/.p4enviro`.

### Scene merge tool on Linux

The equivalent of `godot-merge.bat` is a small script:

```bash
#!/bin/sh
exec ~/godot_perforce_linux --headless --merge-scene "$1" "$2" "$3" "$4"
```

## Running the tests

```
python -m SCons platform=windows target=editor d3d12=no accesskit=no tests=yes
bin\godot.windows.editor.x86_64.exe --headless --test --test-case="[SceneMerger]*"
```

Don't hand out builds made with `tests=yes`. They're larger and are only for
development.

## Optional extras

- **Direct3D 12 renderer:** run `python misc\scripts\install_d3d12_sdk_windows.py`
  once, then build without `d3d12=no`.
- **Screen reader support:** run `python misc\scripts\install_accesskit.py` once,
  then build without `accesskit=no`.

## Troubleshooting

| Problem | Fix |
|---|---|
| `'python' is not recognized` | Reinstall Python with **Add Python to PATH** ticked, or use `py` instead of `python`. |
| `No module named SCons` | Run `python -m pip install scons`. |
| `Could not find Visual Studio` / `cl.exe not found` | Open **Visual Studio Installer** and add **Desktop development with C++**. |
| Build runs out of memory with `production=yes` | Add `-j4` (or lower) so fewer files compile at once. |
| Editor never says "Perforce: using workspace" | Check `p4 -V` works, and that the project folder is inside your workspace root (`p4 info` shows it as **Client root**). |
| "Could not check out ..." toast | Someone else has the file exclusively locked; the toast says who. Or the file isn't in the depot yet. |
