# FloopHole

<p align="center">
  <img src="./src/floophole_logo.svg" alt="FloopHole" width="300" height="300"/>
</p>

Open-source VST3 bridge for FL Studio's native instrument and effect plugins on
macOS and Windows. Each discovered, supported native plugin appears as its own
entry in your VST3-compatible DAW, such as **Harmor**, **Sytrus**, or **Maximus**.

Building FloopHole does not require FL Studio. **Using it requires a local,
licensed FL Studio installation and the appropriate licenses for the plugins
you use.** FL Studio and its plugins are not downloaded or bundled by this project.

To save the current plugin sound or effect settings, click the diskette next to
**Load preset**. **Save preset as...** opens the active plugin's user preset folder.
Choose a filename to save an `.fst` and select it immediately in the preset
dropdown. Presets saved in the default folder are found again on restart.

Click the toolbar logo to open **About FloopHole**, with a larger logo, the
compiled version and a link to the [GitHub repository](https://github.com/FloopHole-Team/FloopHole).
The first release heading in [changelog.md](./changelog.md) supplies the version
for the About window, VST3 metadata and macOS bundle when building.

Cut, copy and paste use **Ctrl+X/C/V** on Windows and **Command+X/C/V** on macOS
inside Fruity NoteBook, Fruity NoteBook 2 and Edison. Click the native writing
surface or waveform first; shortcuts outside the editor remain with the host.

## License and legal notices

FloopHole's original code is released under the [MIT License](./LICENSE).
It is an independent, unofficial project, **not affiliated with, endorsed by,
sponsored by, or supported by Image-Line or FL Studio**.

FL Studio, Image-Line, their plugin names and their logos remain the property of
their respective owners. VST is a trademark of Steinberg Media Technologies GmbH.
Names are used to identify compatible software; this project's license grants
no rights to third-party trademarks, software, sample libraries or preset content.
You must obtain and use FL Studio and its plugins under their applicable licenses.
FloopHole includes no Image-Line plugin binaries, samples or preset packs.

Steinberg's VST3 interfaces, [LunaSVG](https://github.com/sammycage/lunasvg) and
[PlutoVG](https://github.com/sammycage/plutovg) retain their own MIT licenses and
copyright notices. These notices are packaged with the binaries under
`Contents/Resources/licenses`. The Windows
standalone ZIP includes its accompanying license directory. FloopHole is provided
without warranty; see the full license.

PlutoVG's FreeType-derived components retain the FreeType Project License.
This software is based in part on the work of the FreeType Team. Portions are
copyright (c) 2000-2003, 2005-2014 The FreeType Project
(https://www.freetype.org); all rights reserved. The unmodified `FTL.TXT` and
[third-party notices](./THIRD_PARTY_NOTICES.txt) accompany binary distributions.

## Initialize a fresh checkout

Install Git and CMake 3.20 or newer, then run this from the repository root:

```sh
cmake -P scripts/init.cmake
```

The same command works in macOS Terminal, Windows PowerShell, and Command Prompt.
It downloads the required Steinberg VST3 interfaces into
`third_party/pluginterfaces`. Run it after each fresh clone; subsequent runs reuse
the existing SDK. The first CMake configure also downloads LunaSVG and PlutoVG.
Internet access to GitHub is required for these initial downloads.

Steinberg's downloaded interfaces include their own MIT license in
`third_party/pluginterfaces/LICENSE.txt`. See also the
[official SDK license and usage guidelines](https://github.com/steinbergmedia/vst3sdk#license--usage-guidelines),
including the VST trademark requirements.

## Build on macOS

Requirements: macOS, Xcode Command Line Tools (or Xcode), CMake 3.20+, and Git.
Install the command line tools with `xcode-select --install`. If using Homebrew,
install CMake with `brew install cmake`.

From the repository root, after initializing the SDK:

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build --target FloopHole --parallel
```

The output is `build/VST3/FloopHole.vst3`, a universal Apple Silicon + Intel
bundle targeting macOS 12.0 or newer. It is ad-hoc signed automatically for
local use; this is not Developer ID signing or notarization. To build just one
architecture, replace `arm64;x86_64` with `arm64` or `x86_64` when configuring a
new build directory. The host and the installed FL Studio plugins must support
the architecture you run.

Install the bundle in your per-user VST3 folder and rescan plugins in your DAW:

```sh
mkdir -p "$HOME/Library/Audio/Plug-Ins/VST3"
ditto build/VST3/FloopHole.vst3 "$HOME/Library/Audio/Plug-Ins/VST3/FloopHole.vst3"
```

Quit your DAW before replacing an installed bundle, then restart it. An already
loaded plugin continues running the old code even if the bundle on disk changes.

### macOS native editor checks

The native editor container must follow the non-overlapping toolbar/footer views:
MacVCL's fallback view search otherwise discards the form it already found.
This ordering preserves native control lookup and hover help across instruments
and effects.

With a local FL Studio installation, run the loaded-VST regressions:

```sh
cmake --build build --target FloopHole test_vst3 --parallel
build/test_vst3 --native-hover-help
build/test_vst3 --edison-vst-poll
build/test_vst3 --edison-clipboard
build/test_vst3 --notebook-keyboard
build/test_vst3 --notebook-appkit-keyboard
build/test_vst3 --notebook-caret
```

These check visible hover-help text and native Edison hover/leave state using
the VST's real idle timer, plus Notebook typing/editing through host key
callbacks and AppKit responders, saved-state restoration, and duplicate-delivery
protection when a host invokes both keyboard APIs. Clipboard tests use an isolated
pasteboard, checking actual Notebook text and Edison cut/paste audio state.
The Notebook tests also check opening and reopening the About window.
They do not replace checking
the editor in the DAW.

On Windows, the `test_windows_editor` target checks the About dialog, VCL
shortcut forwarding and real Edit/RichEdit clipboard round-trips without
duplicate pastes. It restores the previous clipboard contents after testing.

Notebook typing/editing supports Cocoa responder events (used by Live) as well
as VST key callbacks, only while a notebook writing surface has focus.
Native activation notifications restore the blinking insertion caret after
selection, refocusing and reopening. The caret regression checks thin-bar pixel
changes in the native canvas across multiple blink cycles; it selects Metal
only within the test process and does not change FL Studio's saved preferences.
Fruity HTML Notebook is an
[HTML viewer](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/Fruity%20HTML%20NoteBook.htm),
not a text editor: load an externally edited HTML document with its file button.

## Build on Windows

Requirements: Git for Windows, CMake 3.20+, and Visual Studio 2022 (or Build Tools)
with **Desktop development with C++** and the Windows SDK.

Open PowerShell or Command Prompt, change to the repository root, and run:

```bat
cmake -P scripts/init.cmake
cmake -S . -B build-windows -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=OFF
cmake --build build-windows --config Release --target FloopHole --parallel
```

For subsequent Windows builds, only run:

```bat
cmake --build build-windows --config Release --target FloopHole --parallel
```

Windows cannot build this project's macOS target with the supplied toolchains.
Use a Mac with Xcode Command Line Tools, or a macOS CI runner, and follow
the macOS setup above. After that initial setup, repeat Mac builds with:

```sh
cmake --build build --target FloopHole --parallel
```

Outputs:

- Standard bundle: `build-windows/VST3/FloopHole.vst3/Contents/x86_64-win/FloopHole.vst3`.
- Standalone binary: `build-windows/VST3/FloopHole_x64.vst3`.
- Standalone distribution with license notices: `build-windows/VST3/FloopHole_x64.zip`.

Copy the entire `build-windows/VST3/FloopHole.vst3` directory into
`C:\Program Files\Common Files\VST3` (administrator permission may be required),
then rescan plugins in your DAW. Do not install the standalone binary as well as
the bundle, since that can create duplicate plugin entries. An MSVC build may
also require the Microsoft Visual C++ 2015-2022 x64 Redistributable on the
destination machine.

## Plugin setup

Install **one** FloopHole VST3 module, then perform a **full plugin rescan** in
your DAW. Repeat the full rescan after installing additional FL Studio plugins.
In Ableton Live, hold Option on macOS or Alt on Windows while clicking Rescan.

FloopHole normally finds FL Studio automatically. If it is not found, or to choose
a specific installation, set the absolute `FLOOPHOLE_FL_STUDIO` environment
variable before launching your DAW. Point it at the `.app` bundle on macOS or the
installation folder containing `Plugins` and `Shared` on Windows. Restart the DAW
and perform a full rescan after changing this setting.

## Cross-compile Windows on macOS

Install the MinGW-w64 compiler in addition to the macOS build prerequisites:

```sh
brew install mingw-w64
```

Initialize the SDK as above, then use a separate build directory:

```sh
cmake -S . -B build-win64 -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win64 --target FloopHole --parallel
```

The Windows outputs are `build-win64/VST3/FloopHole.vst3` (bundle directory)
and `build-win64/VST3/FloopHole_x64.vst3` (standalone binary). These require a
Windows x64 host and Windows FL Studio plugins; cross-compiling does not make
Windows plugins loadable on macOS.
