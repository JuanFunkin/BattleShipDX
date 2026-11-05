# BattleShipDX on Android

The upstream port already ships an Android build (see `android_port_status_2026-05-01.md` and
`android_armeabi_v7a_2026-06-20.md`). BattleShipDX builds it with its own additions.

## Building the APKs

Actions tab → **Android (APK)** → *Run workflow* (or push a `v*` tag). It builds two APKs and uploads them as artifacts:

- `BattleShipDX-android-arm64-v8a.apk` — 64-bit ARM (almost every phone from the last ~8 years).
- `BattleShipDX-android-armeabi-v7a.apk` — 32-bit ARM (older / cheap phones, TV boxes).

Without signing secrets the APK is debug-signed (fine for sideloading). To get a stable certificate add the
`SSB64_ANDROID_KEYSTORE_BASE64`, `SSB64_ANDROID_KEYSTORE_PASSWORD`, `SSB64_ANDROID_KEY_ALIAS` and
`SSB64_ANDROID_KEY_PASSWORD` repository secrets. `release.yml` also builds both ABIs for releases.

## First run

1. Install the APK (allow "install unknown apps").
2. Pick your own `baserom.us.z64` when asked; the game builds `BattleShip.o2r` on the device.
3. Open the menu with the hamburger button on the touch overlay.

## Touch controls (configurable)

Tap the **gear** next to the menu button to edit the controls:

- **Drag** any control to move it; **tap** it to change its size or hide it.
- Global **opacity** and **size**, vibration on touch.
- **Stick:** floating (the base appears where you touch, on the left side) or fixed; the base can follow your finger;
  adjustable dead zone and sensitivity.
- New **Jump** button (mapped to C-up, the N64 jump button).
- *Reset layout* restores the defaults. The layout is saved on the device.

## Online with PCs and other phones

Phones can't use Radmin, but any VPN that gives every device an IP works:

- **Tailscale**: install it on every device, sign in to the same tailnet, and use the host's `100.x.y.z` address.
- **ZeroTier**: join the same network ID on every device and use the host's ZeroTier IP.

Then it is the usual flow: the host opens *Network → LAN Online → Host game*; the others type the host IP and the
same UDP port and press *Join game*. The soft keyboard opens when you tap a text field. The app has the `INTERNET`
permission for this.

## Mods on Android

- Hi-res texture packs and music packs: the in-app importer copies a `.zip` into the app's `mods` folder.
  A pack exported as `.o2r` is a zip file: rename it to `.zip` before importing.
- Audio Editor: the **Import music** button opens the system file picker (.mp3, .ogg, .wav, or a .zip / .o2r music pack, several at once). Press **Rescan mods** afterwards.
- Cheats, the Audio Editor and the Network menu work like on PC. The new-release notice and the script mods are
  desktop only.

## Known limits (not verified on a device)

- The BattleShipDX additions were written on top of the upstream Android port and have **not been run on a phone**.
  Expect to fix small things after the first build.
- Cross-play between PC and Android relies on the simulation being identical on both. The game code is built with
  `-ffp-contract=off` to avoid ARM/x86 float differences, but this has not been tested. If a match reports a desync,
  please report which devices were involved. 32-bit ARM is the most likely to differ.
- 32-bit phones have little RAM: keep hi-res packs small.

## Comfort and speed (update 20)

- Stick: bigger ring (72 dp), more sensitive by default (130%, up to 200%) and a smaller dead zone (6%), so full deflection
  needs less thumb travel. Old stick settings are reset once; adjust in the edit panel (gear button).
- 32-bit ARM: built with NEON and, on first launch, uses 480p (window aspect) rendering. If it is still slow, keep
  MSAA at 1, Enhanced Framerate off, and no hi-res packs. Change the resolution in Settings (needs a restart).
