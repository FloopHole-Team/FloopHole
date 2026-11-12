# FloopHole

<p align="center">
  <img src="./src/floophole_logo.svg" alt="FloopHole" width="300" height="300"/>
</p>  

Open-source VST3 bridge for FL Studio's native instrument and effect plugins on
macOS and Windows. Each discovered, supported native plugin appears as its own
DAW entry: **Harmor**, **Sytrus**, **Maximus**, and so on. Instrument and effect
classes have the corresponding VST3 categories. Building FloopHole does not
require FL Studio, but using it requires a
local FL Studio installation and the corresponding Image-Line plugins. FL Studio
and its plugins are not downloaded or bundled by this project.

## Discovery and project migration

Install **one** FloopHole VST3 module, then perform a **full plugin rescan** in
your DAW. FloopHole inspects the installed native module files without loading
their engines or enumerating all their presets. A new factory discovery refreshes
the file list, including newly installed plugins; only the selected instrument or
effect loads its native libraries and presets when instantiated.

Some DAWs cache an unchanged VST3 module even after a normal rescan. After adding
FL plugins, use the DAW's full/forced rescan, not just its incremental scan.
In Ableton Live, hold Option on macOS or Alt on Windows while clicking Rescan.
FloopHole deliberately ships **no `moduleinfo.json`**: its factory, rather than a
static two-class manifest, is the source of the available classes.

## License and legal notices

FloopHole's original code is released under the [MIT License](./licence.md).
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
copyright notices. The build pins LunaSVG and its PlutoVG submodule, and packages
these notices with the binaries under `Contents/Resources/licenses`. The Windows
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
It downloads the Steinberg VST3 SDK component used by this project,
`pluginterfaces`, into `third_party/pluginterfaces`. The version is pinned to
SDK 3.8.1, commit `4f547e8e102b47de4a8b8aaf343c73b700786372`, rather than the
latest branch. The first run requires internet access to GitHub; subsequent runs
reuse the existing SDK without downloading or overwriting it.

The first CMake configure also fetches the pinned LunaSVG revision
`7b6970f40e72826749d4b050d11c2a4c3406f10c` and its pinned PlutoVG submodule into
the build directory. They render the supplied SVG consistently on both platforms;
system-font loading in that renderer is disabled. The logo data is embedded and
contains no external asset references.

The SDK is not a Git submodule of this project and is not included in a normal
GitLab clone or pull. Downloaded files are ignored by Git. Run the initializer
after every fresh clone; there is no need to run `git submodule update`.

An existing full SDK at `third_party`, with its `pluginterfaces` directory
populated, is also supported and left untouched. If an incomplete SDK directory
exists, the initializer stops and asks you to move it aside or remove it before
retrying. CMake also reports the initialization command when the SDK is missing.

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

Automatic discovery searches `/Applications`, `~/Applications`, and
`/Applications/Image-Line`, without a fixed list of FL Studio version numbers.
Valid versioned installations are sorted newest first; Launch Services is also
checked for a registered FL Studio application.

## Build on Windows

Requirements: Git for Windows, CMake 3.20+, and Visual Studio 2022 (or Build Tools)
with **Desktop development with C++**, the Windows SDK, and **C++ CMake tools for
Windows** (including Ninja).

Open an **x64 Native Tools Command Prompt for VS 2022**, change to the repository
root, and run:

```bat
cmake -P scripts/init.cmake
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows --target FloopHole --parallel
```

Use the x64 tools, not x86 or ARM64. These instructions use Ninja, a
single-configuration generator, so the output paths below do not acquire a
`Release` subdirectory.

Outputs:

- Standard bundle: `build-windows/VST3/FloopHole.vst3/Contents/x86_64-win/FloopHole.vst3`.
- Standalone binary: `build-windows/VST3/FloopHole_x64.vst3`.
- Standalone distribution with license notices: `build-windows/VST3/FloopHole_x64.zip`.

Copy the entire `build-windows/VST3/FloopHole.vst3` directory into
`C:\Program Files\Common Files\VST3` (administrator permission may be required),
then rescan plugins in your DAW. Do not install the standalone binary as well as
the bundle, since that can create duplicate plugin entries. FL Studio's Windows
installation is discovered through the registry and standard Image-Line install
directories. An MSVC build may also require the Microsoft Visual C++ 2015-2022
x64 Redistributable on the destination machine.

## Finding FL Studio

On Windows the bridge checks Image-Line's shared installation path, enumerates
versioned registry keys in both registry views, and searches Image-Line folders
under the configured Program Files locations and fixed drives. It does not depend
on hardcoded release years. Installations must retain FL Studio's native
`Plugins/Fruity/Generators`, `Plugins/Fruity/Effects`, and `Shared` layout.

If FL Studio is not found, the factory reports the failure and exposes no plugin
classes. Set the absolute `FLOOPHOLE_FL_STUDIO` environment variable before
launching your DAW: point it at the `.app` bundle on macOS, or the installation
folder containing `Plugins` and `Shared` on Windows. Restart the DAW and perform
a full rescan. There is deliberately no generic setup/plugin-picker class.

The saved choice takes priority over automatic discovery. A missing/moved saved
installation is ignored with a diagnostic and automatic discovery is retried.
An active installation is not hot-swapped: restart the host before switching FL
Studio versions. Advanced users can explicitly select an installation using the
absolute `FLOOPHOLE_FL_STUDIO` environment variable before launching the DAW. The
legacy `FLBR_FL_STUDIO` variable and previously saved installation choice remain
supported. An invalid explicit override is reported instead of silently using
another version. `FLOOPHOLE_USER_DATA` (or legacy `FLBR_USER_DATA`) can isolate data
and preferences during testing.

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

Alternatively, if `x86_64-w64-mingw32-g++` is on `PATH` when configuring a macOS
build, its default build also includes the `FloopHole_Windows` target:

```sh
cmake --build build --parallel
```

This builds the macOS bundle and Windows binary, embeds the latter at
`build/VST3/FloopHole.vst3/Contents/x86_64-win/FloopHole.vst3`, and creates the
standalone `build/VST3/FloopHole_x64.vst3`. Reconfigure the macOS build after
installing MinGW if it was not present at the previous configure step. Building
only `--target FloopHole` skips this optional Windows target.

## Verification

`BUILD_TESTING` is enabled by default. Build `test_bridge` and run the
`bridge_contracts` test through VS Code's CMake Tools test integration. The
macOS `test_vst3` target provides additional integration checks that require
FL Studio and an interactive desktop session:

```sh
cmake --build build --target test_vst3 --parallel
./build/test_vst3
```

Run the integration executable from the repository root so its default bundle
path resolves correctly.

On Windows, the editor-resource regression test does not require FL Studio:

```bat
cmake --build build-windows --target test_windows_editor --parallel
ctest --test-dir build-windows -R "^windows_editor_lifecycle$" --output-on-failure
```

It repeatedly starts/stops GDI+, paints overlapping toolbars, checks that closing
one editor does not invalidate another, verifies window-class cleanup, and checks
the resize grip's dark background. Text formats are created per paint rather than
using MinGW's cached `Generic*` wrappers, which become invalid after graphics
shutdown. Custom window classes belong to the plugin DLL and are unregistered
after their last editor closes, outside DLL unload callbacks.

## Parameter names and hints

FL Studio prefixes some labels with hint-icon markers: `^a` means MIDI-linkable,
`^b` means recordable/automatable, and `^c` requests a smiley. The bridge removes
these leading markers from status text, parameter names and displayed values on
both platforms. For example, `^b^aLFO Phase` becomes `LFO Phase`. Literal carets
elsewhere in text remain unchanged. Automation IDs, parameter order, values and
saved-state formats are unaffected.

## Plugin selection and compatibility

The runtime factory lists individual classes alphabetically (case-insensitive).
Instruments are marked `Instrument|Synth`; effects and visual/notebook utilities
are marked `Fx`. There is no toolbar or automatable plugin selector. Each
instance is permanently bound to its native module and rejects another class's
saved state.

Additional installed plugins now exposed:

- **Instruments:** DirectWave, FL Keys, Soundfont Player, Fruity Slicer 2, Slicex,
  Fruity DrumSynth Live, and FPC. DirectWave needs a sample or bank loaded;
  The **folder button** accepts its `.dwp` programs and `.dwb` banks. FL Keys uses the
  installed `Wavtables` directory and its native `.wti`, `.dat`, and `.wav`
  loading. Its factory `default.fst` is selected on first load; allow background
  wavetable loading to finish before playing. Default-preset matching is
  case-insensitive on both platforms, with user presets taking precedence.
  Slicers need a sample loaded in their native editor. Soundfont Player accepts
  `.sf2` banks through the **folder button** as well as its native editor.
- **FX/utilities:** Fruity Dance, Fruity HTML NoteBook, Fruity NoteBook,
  Fruity NoteBook 2, Razer Chroma, Wave Candy, and ZGameEditor Visualizer.
  Fruity Dance is a visual utility, not a sounding instrument, despite being
  installed in FL Studio's Generators directory. No-process utilities preserve
  input audio through the bridge's existing output-gain stage.
  Fruity Big Clock and Fruity Dance receive native playback time in beats,
  milliseconds and ticks, including seeks, stops and time-signature changes.
  Clock milliseconds use the DAW's sample timeline, not a reconstruction from the
  current tempo, so tempo changes do not reinterpret elapsed project time.

These additions were checked on macOS for native construction, editor embedding,
state restore, rendering, and unload; sounding instruments were also checked with
actual sample/bank content. Windows builds are cross-compiled, not runtime-tested.
Razer hardware output, advanced visualizer features, and external integrations
are not certified by these smoke tests.

The following requested plugins remain excluded rather than exposing broken
entries:

| Plugins | Current limitation |
| --- | --- |
| Wave Traveller, Fruity Granulizer | Standalone sample/voice integration remains unverified with the new host sample services. |
| FL Studio Mobile, Patcher | Standalone audio and FL-specific hosting/routing are not implemented or verified. |
| Newtone | Crashed during native editor initialization in macOS testing. |

## Editor regression checks

The bridge queries each native plugin's `FPD_WindowMinMax` limits to advertise
VST3 resizing only when supported. Host resizing changes the actual native editor,
respecting its minimum/maximum dimensions and sizing increments; fixed-size editors
remain fixed. Both instrument and effect versions provide their own draggable
bottom-right resize corner, independent of the DAW window's resize style. Fixed
editors hide the corner. The wrapper follows the native editor's width, including
short/narrow effects; its toolbar wraps into rows and the preset picker and helper
text expand to use available space.
On macOS, native pixel limits are converted to Cocoa points. DirectWave's initial
pixel-sized Cocoa view is corrected without scaling its fonts or controls, and
reopening retains a user-resized editor without shrinking it again. Full-client
MacVCL drawing viewports are aligned and resized with their forms, then invalidated,
so controls reflow and repaint rather than stretching or leaving black padding.

With FL Studio installed and an interactive macOS desktop session, run
`./build/test_vst3 --editor-coordinates` after building the `test_vst3` target.
It checks native window/client screen origins for BooBass, Sakura, Sawer and Harmor
at offset and moved nested parents, exercises BooBass knob input, and verifies cleanup
on editor detach. The full `test_vst3` run includes these checks as well.

The reopening checks recreate Cocoa parents, reuse and replace VST3 views, preserve
native state, and verify Harmor FX input after reopening. Run with Objective-C Zombies
enabled to detect premature view releases:

```sh
NSZombieEnabled=YES ./build/test_vst3 --editor-reopening
NSZombieEnabled=YES ./build/test_vst3 --vst-editor-reopening
./build/test_vst3 --sytrus-layout
./build/test_vst3 --flex-scroll
./build/test_vst3 --editor-resizing
./build/test_vst3 --transport-utilities
./build/test_vst3 --catalog
```

The resizing check exercises the actual VST3 views for DirectWave, Big Clock,
Wave Candy, FL Keys, Sytrus, Emphasis, Frequency Splitter, PanOMatic, Parametric EQ 2,
Spectroman, Low Lifter, Multiband Delay, Spreader, Tuner, Bass Boost, Center, Fast LP,
Mute 2, Phase Inverter, dB meter and LuxeVerb: native dimensions, capability queries before/after
attachment, bounds, FL Keys' height increments, same-size acknowledgements,
repeated reopening, DirectWave knob input and fresh-instance
state restoration, native-driven Sytrus keyboard resizing, LuxeVerb's two height
states, wrapped-toolbar bounds, native drawing-viewport bounds and actual corner
drags in non-resizable host windows. Each fixed/resizable effect has its own class.
An optional
output directory captures the resized owned windows. `bridge_contracts` covers
the sizing ABI, ignored messages, zero-return responses, fixed/one-axis limits,
pixel-to-point snap grids and invalid/extreme requests on both platforms, plus
deterministic toolbar wrapping, bounds and stretching, native class-ID fixtures,
path/display-name independence, collision rejection, and fresh filesystem rescans.

The transport check exercises actual native Big Clock/Dance time queries,
bar/step/tick conversion, visible clock and dancer frame progression, seeking,
stopping, meter changes and editor reopening. An optional output directory captures
the owned utility windows. The catalog check validates every actual VST3 factory
class, its CID and instrument/FX metadata, checks the absence of `moduleinfo.json`,
and verifies that enumeration does not load native FL libraries. Its scan-time
budget is two seconds on the interactive test machine.
Deterministic `bridge_contracts` additionally covers future installation names,
numeric version ordering, invalid roots and native time formats on both platforms.

The Sytrus check measures panel widths and spacing before interaction, verifies
parameter values, hidden/visible keyboard states, fresh-instance state restoration
and reattachment. An optional output PNG path captures the owned initial editor.
The full VST3 run also checks Sytrus layout in its dedicated native instance.

The FLEX check compares the original native wheel handler with the bridged path,
scrolls the preset list in both directions using wheel and precise trackpad events
(including momentum), and selects revealed presets to verify actual synth changes.
It also checks normal knob automation, native class restoration, reopening and
repeated fixed-class reopening. An optional output PNG path captures the owned
scrolled editor.

## Sample loading, image drops and analyzer displays

The native **3x Osc** custom-waveform button and **Fruity Scratcher** sample
browser use the system audio-file picker. The bridge implements the SDK sample
loader, format conversion and sample lifetime for both native ABIs. Supported
file formats depend on the operating system's audio decoders. SDK sample paths
must fit its 256-byte filename buffer; oversized paths are reported, not truncated.

3x Osc's host-owned waveform is rebuilt from its saved sample path when restoring
a project. Bridge state version 5 retains these sample paths and the instrument
volume or FX mix; existing version-1/2/3/4 states remain supported. Audio files are referenced,
not embedded, and must remain
available. Scratcher retains its sample reference in its own native state.

On macOS, scoped editor drop handlers route Scratcher audio files and Harmor
PNG/JPEG/BMP images to the native file loader, including drops over child views.
Other drag payloads keep their native handling, and hooks are removed on detach.
Drop subclasses report the original native view class, preserving MacVCL control
identification and mouse input in Harmor's FX and IMG panels.
Windows retains native drag handling and uses the same host sample services.

The bridge sends the native enabled/disabled notification on load, state/preset
changes and VST3 activation/deactivation. This enables the live Maximus, Fruity
Limiter and Transient Processor displays instead of their inactive animation or
PAUSE indication, including when audio is processed with the transport stopped.

The following interactive macOS checks require an existing audio/image fixture.
The picker checks open and automatically cancel the actual system dialog; the
drop checks exercise the hosted Cocoa destination. Screenshots capture only the
owned test window.

```sh
./build/test_vst3 --channel-state /absolute/path/test.wav
./build/test_vst3 --host-services "3x Osc" /absolute/path/test.wav /tmp/flbridge-osc.png picker
./build/test_vst3 --host-services "Fruity Scratcher" /absolute/path/test.wav /tmp/flbridge-scratcher.png picker
./build/test_vst3 --host-services "Fruity Scratcher" /absolute/path/test.wav /tmp/flbridge-drop.png drop
./build/test_vst3 --host-services Harmor /absolute/path/test.png /tmp/flbridge-harmor.png drop
./build/test_vst3 --host-services Harmor /absolute/path/test.png /tmp/flbridge-harmor-input.png mouse
./build/test_vst3 --host-services Maximus - /tmp/flbridge-maximus.png
```

Native picker/drop, sample playback and analyzer checks were run on macOS.
The Harmor `mouse` check drags actual FX and IMG controls around an image drop and
asserts native parameter changes.
Windows builds are cross-compiled; Windows runtime behavior still needs validation.

## Presets, FLEX packs and writable data

The preset menu scans factory and user `Plugin presets/Generators/<plugin>` or
`Plugin presets/Effects/<plugin>` directories. Besides `.fst`, it supports Toxic
Biohazard `.tbio`, Morphine `.mrp`, GMS `.gmsynth`, MiniSynth `.flms`, and
Soundfont Player `.sf2` files.
DirectWave `.dwp` programs and `.dwb` banks are also scanned and passed to its
native loader. DirectWave does not acknowledge the SDK load request; it reports
bank-loading failures in its native editor.
Native formats are passed to the plugin's file loader, not treated as FL Studio
state chunks. The native bank browsers also receive the correct preset roots.

the **folder button** can import a `.flexpack` in a FLEX instance. Packs and optional
same-name `.ini` metadata are copied into `Documents/Image-Line/FLEX/Packs`.
Existing packs are not overwritten. Switch to another plugin and back to FLEX
after importing to refresh its native library; select individual sounds inside
FLEX. Pack containers are not individual `.fst` presets.

On a fresh load, FLEX selects its first installed internal preset unless a
`Default.fst` was loaded. Restored project state takes precedence; reopening the
editor does not reset the sound. The bridge initializes SDK channel volume on
load and after preset/state changes so FLEX does not remain muted.

On macOS, DirectWave uses a C++ audio ABI with a MacVCL editor. The bridge
provides a retained native application context and tags its embedded view handle
separately from audio-ABI selection. Neither instrument's wavetable/bank format
is reimplemented by the bridge.

Native sampler regressions check finite, nonzero audio before and after state
restoration, editor reopening, and fresh-instance restoration:

```sh
./build/test_vst3 --catalog-plugin Generator "FL Keys" - 0.001
./build/test_vst3 --catalog-plugin Generator DirectWave /absolute/path/program.dwp 0.001
./build/test_vst3 --sample-instruments /absolute/path/program.dwp
```

The last check runs both instruments through the built VST3, including the
instrument-only selector and complete 2,048-slot automation exposure.

FLEX caches and downloaded content use writable user directories, separate from
the FL Studio installation. Factory packs remain in the installation's
`Data/Patches/Packs/FLEX` directory. No administrator access or changes to
Program Files/the application bundle are required.

The default FL Studio user root is `Documents/Image-Line/FL Studio`. For a relocated
user-data directory, set the **absolute** `FLBR_USER_DATA` environment variable
before starting the DAW. FLEX data lives in the sibling `FLEX` directory, matching
Image-Line's directory layout. This override is also useful for isolated tests.

FLEX's native Library/Store UI is retained, but authenticated purchases, FL Cloud
and host-managed online downloads are not implemented or certified by the bridge.
Install purchased/downloaded packs through FL Studio or your Image-Line account,
then use the local library/import workflow. See Image-Line's
[FLEX pack installation instructions](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/FLEX.htm).
Unsupported newer host path requests are logged rather than redirected into the
installation.

## Audio/UI contention checks

Rendering no longer replaces an entire buffer with silence just because a UI
operation owns the plugin lock. Native lock/unlock callbacks synchronize on both
platforms, and host automation notifications are sent after the bridge callback
releases its lock. This prevents deliberate buffer dropouts, but does not make
long preset loads, state restores or plugin switches real-time-safe.

The `bridge_contracts` test covers contended rendering, native preset dispatch,
path-manager lookup/enumeration, writable cache directories and pack import without
requiring FL Studio. With FL Studio and an interactive macOS desktop, also run:

```sh
./build/test_vst3 --realtime-knobs
./build/test_vst3 --plugin-data "Toxic Biohazard"
./build/test_vst3 --plugin-data Morphine
./build/test_vst3 --plugin-data GMS
./build/test_vst3 --plugin-data MiniSynth
./build/test_vst3 --plugin-data FLEX
```

The knob check renders at 64 frames/44.1 kHz while changing native controls and
idling the editor, checks for muted blocks and requires the 99th-percentile render
time to fit the buffer deadline. Plugin-data checks exercise native state
round trips and legacy preset rendering. FLEX checks native preset registration,
audible default playback before opening its editor, selecting another internal
preset and preserving restored state across editor reattachment.
