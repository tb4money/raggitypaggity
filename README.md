# CXPS5

CarX Street mod loader for a jailbroken PS5, with a phone-friendly web UI. Load car, rim and audio
bundles from your phone or PC, flip them on and off, and swap them into your own dumped copy of the
game. Everything is reversible.

**Status: 0.1.0. The loader logic is tested on a computer against a fake game folder (38 checks,
`make test`). It has not been run on a real PS5 or a real CarX Street dump.** Treat your first run as a
test: see "First run" below.

## What the research says

- CarX Street's own PC Workshop system works by **replacing Addressables `.bundle` files by name** when
  the game loads resources. Only `.bundle` files are accepted, the originals are not modified, and
  when two mods replace the same bundle the one with higher priority wins.
- Only bundles from supported Addressables groups are replaced. Car and body kit bundles are the main
  case. Mods break after game updates until the author re-saves the bundles.
- Modded bundles on PC can stop the game logging in, so use this offline only.

The PS5 build has no mod system, so CXPS5 does the same thing on disk: index the game's `.bundle`
files, then for each enabled mod bundle with a matching name, back up the original once and swap the
mod's file in. No code is injected into the game, so it does not depend on the game build.

## Install

1. Build `cxps5.elf` (below) or use a prebuilt one.
2. Send it with your usual payload sender / Payload Manager. A notification says it is ready.
3. Open `http://<PS5 IP>:8090` on any device on your network.
4. Optional: copy `config.ini.example` to `/data/cxps5/config.ini` and edit it.

## Using it

1. **Add**: choose `.bundle` files, a folder, or a `.zip` (Workshop items live in
   `Steam\steamapps\workshop\content\1114150\<item id>` on a PC). Or paste a direct link to a
   `.bundle`/`.zip`.
2. **Garage**: switch the mod on. Each file shows whether its Unity version matches the game's file.
3. Close CarX Street, tap **Apply to game**, start the game.
4. **System → Restore originals** puts every original file back, byte for byte.

Steam Workshop pages are not direct links: the Workshop needs Steam, so download on a PC and upload.

## First run

1. Open **System**. The game should be detected, the bundle count should be more than zero, and
   "Writable" should say yes. If not, set `game_root` in `config.ini` and check `/data/cxps5/cxps5.log`.
2. Test with one small bundle first. Apply, launch, check, then Restore.
3. Bundles made for PC may show pink or invisible parts on PS5 because shaders are platform specific.
   Meshes and audio often work. A bundle with a different Unity version will probably not load.

## Crashes and incompatibility

**System → Checks** lists problems it can see: a mod built with a different Unity version than the game's
file, bundles with no matching game file, a read-only game folder, or a missing backup. CXPS5 also
watches the game: if it closes within 90 seconds of starting with modded files in place, that is logged
and shown as a likely crash or bad bundle.

**System → Download report** saves `cxps5-report.txt` with the game version, every swapped file, the
Unity version of each mod and game file, the findings and the recent log. Attach it to bug reports.
(CXPS5 can only see that the game closed, not why. It has no access to the console's own crash dumps.)

## Limits

- Bundle names must match the game's. A mod that adds a brand new car with new bundle names will be
  listed as "no matching game file". It needs the game's catalog changed too, which is not done here.
- If the game folder is mounted read-only on your setup, Apply fails with a clear error.
- No shaders/ReShade, no code mods (IL2CPP).

## Build

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk   # needs the curl/openssl/zlib ports
make                                          # cxps5.elf
make test                                     # host build + API tests (no console needed)
```

Layout: `src/cxps5.c` (HTTP server, mod store, bundle index, apply/restore, zip import, downloads),
`ui/index.html` (embedded web UI), `third_party/cacert.pem` (Mozilla CA bundle, MPL-2.0, for https).

The structure (payload + embedded web server + `/data` config) follows the Hitman PS5 project you
attached. No code was copied from it, because it is AGPL-3.0.

Use only with a copy of the game you own, offline. Not affiliated with CarX Technologies or Sony.
