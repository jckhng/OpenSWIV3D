# OpenSWIV3D 0.1.0-alpha.5

Prepared on 14 November 2026 as a public Windows x64 testing alpha.
Project-authored source and documentation use MIT. The decoder retains
its LGPL 2.1-or-later license.

## Release files

- `OpenSWIV3D-0.1.0-alpha.5-windows-x64.zip`
- `OpenSWIV3D-0.1.0-alpha.5-source.zip`
- `SHA256SUMS.txt`
- `release-index.json`

Users supply an original SWIV 3D installation. CD music requires the user's
CUE and referenced BIN image. See README for launch commands and controls.

## Implemented scope

Original opening movies, animated pilot/menu presentation, setup and keyboard
remapping lead into native mission simulation. Combat includes enemies,
pickups, upgrades, special weapons, vehicle changes and bosses. Campaign
dispatch covers seventeen missions, results, checkpoint profiles and ending
presentation. Windows supports keyboard, mouse and XInput adapters.

The portable player builds independently of the reverse-engineering tools.
Linux supports the core and headless replay. `--final-only` retains compact
replay captures while executing every update and render.

## Validation

- Independent source builds pass eleven synthetic CTests on each of Windows
  and Linux, plus original-resource movie and retained-session checks.
- The extracted Windows player passes save/reload across processes and
  Continue from a declared compatible later-mission checkpoint.
- An unchanged normal-inventory recording reaches first-mission death.
  Menu navigation then restarts the mission in the same process, with working
  movement/fire. Reloading the death profile in a further process also passes.
- Fixed original comparisons cover selected startup, menus, gameplay,
  vehicles, bosses and ending routes. Controlled audio comparisons pass
  within their recorded boundaries.
- The owner reports first-mission play through death with no fault found
  and original-like music/effects balance.

## Open checks

- Full uninterrupted normal-inventory play through all seventeen missions
  remains unverified. Later Continue coverage uses a supplied checkpoint.
- Desktop input delivery, window-close saving and a separate clean-Windows
  installation need further checks. Short automation taps at pilot selection
  produced no visible transition during the latest desktop attempt.
- Physical controllers retain injected-input coverage. Broader controls,
  vehicle/beam rendering and presentation branches need further coverage.
- Cold autonomous movie handoff audio alignment and physical IRQ/VGA/CD
  timing retain measured differences or coverage limits.

Record the version, Windows version, original data edition, mission, craft,
controller and reproduction steps when reporting a problem. See TESTING.md
for the interactive smoke procedure.
