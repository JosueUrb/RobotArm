![SolidCraft Gears: involute gear generator plugin](og-image.png)

# SolidCraft Gears

An involute spur gear generator for [SolidCraft](https://github.com/tangency-labs/solidcraft). You give it a module, a tooth count and a face width; it draws the tooth profile in a sketch and extrudes it, using ordinary SolidCraft commands. What lands in your design is a normal timeline (a cylinder, a sketch, an extrude, a circular pattern and a bore), so you can open any of those features afterwards and change them. It is not a mesh.

I wrote the involute math myself from the textbook definitions, and it runs sandboxed as a WebAssembly plugin (`craft:plugin@0.1.0`, capability `commands@0.1`). It needs no files, no network and no clock.

## Install

You need a SolidCraft build that has the plugin host (the `commands@0.1` capability).

1. Download `solidcraft-gears-<version>.zip` from the [latest release](../../releases/latest).
2. Unzip it into SolidCraft's plugins folder, so you end up with a `solidcraft-gears` folder in it holding `plugin.json` and `plugin.wasm`:

| OS | Plugins folder |
|---|---|
| Windows | `%LOCALAPPDATA%\SolidCraft\plugins\` |
| macOS | `~/Library/Application Support/SolidCraft/plugins/` |
| Linux | `$XDG_DATA_HOME/solidcraft/plugins/` (usually `~/.local/share/solidcraft/plugins/`) |

3. Start SolidCraft. Preferences > Plugins lists it; listing never runs any code. Each run needs your approval of the exact package contents.

The release also has a `.sha256` file if you want to check the download.

## Use

The plugin has one entry, `make-gear`. All sizes are millimetres.

| Parameter | Default | Meaning |
|---|---|---|
| `module` | 2 | Module m. Pitch diameter is m x z |
| `teeth` | 20 | Tooth count z, 8 to 80 |
| `pressure_angle` | 20 | Pressure angle in degrees, 10 to 30 |
| `face_width` | 10 | Gear thickness |
| `bore_diameter` | 0 | Centre hole; 0 means none |
| `profile_shift` | 0 | Profile shift coefficient x, -1 to 1 |

The gear sits on the XY plane, centred on the origin, extruded up along +Z. The reply gives the pitch, base, outer and root diameters.

From the command line, with a `solidcraft-cli` that has the plugin host:

```
solidcraft-cli run inspect.json --results        # [{"command":"plugin.inspect","params":{"path":"<plugins>/solidcraft-gears"}}]
solidcraft-cli run make.json --results --save gear.sc
```

where `make.json` is

```json
[{"command": "plugin.run", "params": {
  "path": "<plugins>/solidcraft-gears",
  "approval": "<the approval hash inspect gave you>",
  "entry": "make-gear",
  "params": {"module": 2, "teeth": 20, "face_width": 10, "bore_diameter": 6}}}]
```

## What it builds

- Standard full-depth teeth: addendum 1 m, dedendum 1.25 m, so with no shift the outer diameter is m (z + 2) and the root diameter m (z - 2.5).
- Profile shift x moves the cutter: outer diameter m (z + 2 + 2x), root diameter m (z - 2.5 + 2x), and the tooth thickness on the pitch circle becomes m (pi/2 + 2 x tan a).
- Each flank is an involute of the base circle (radius m z cos a / 2), drawn as a spline through 10 points computed exactly from the involute equations. Below the base circle (small tooth counts) the flank continues radially.
- The root fillet is a circle of 0.3 m tangent to both the root circle and the flank. It is an approximation of the true trochoidal root a hob would cut, and it shrinks on its own if the gap is too narrow.

Not modelled: undercut for very small tooth counts (hence the minimum of 8), backlash, helical or bevel teeth, and tip relief. The build extrudes one tooth wedge onto a hub and repeats it with a circular pattern, which gets slow as the count grows, so I cap the tooth count at 80 to stay inside the host's 30 second limit.

## Tests

`cargo test` checks the math (diameters, tooth thickness at the pitch circle, that the profile closes up). `tests/cli_test.py` runs the built plugin through `solidcraft-cli`, saves each design, and measures the result: one closed solid, the tooth count from the number of tip faces, the outer diameter from the largest cylinder, the bore, the face width, and the pitch diameter by cutting a thin ring around d = m z out of the gear and checking that the teeth fill the share of it that the tooth thickness says they should (half, with no shift).

```
./build.sh
SOLIDCRAFT_CLI=/path/to/solidcraft-cli python3 tests/cli_test.py
```

## Build

You need Rust with the `wasm32-unknown-unknown` target and [`wasm-tools`](https://github.com/bytecodealliance/wasm-tools). `./build.sh` builds the core module, wraps it as a component and leaves the package in `dist/solidcraft-gears/`. The WIT in `wit/` is a copy of SolidCraft's `craft:plugin@0.1.0`.

## Releases

Pushing a tag like `v0.1.0` runs `.github/workflows/release.yml`, which builds the plugin and attaches `solidcraft-gears-0.1.0.zip` and `solidcraft-gears-0.1.0.zip.sha256` to a GitHub release. The version in `Cargo.toml` and `plugin.json` has to match the tag.

## License

MIT, see [LICENSE](LICENSE).
