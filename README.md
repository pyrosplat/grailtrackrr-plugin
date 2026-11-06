# GrailTrackrr plugin

D2RLoader plugin for Diablo II: Resurrected (offline) that sends your Holy Grail finds to [grailtrackrr.com](https://grailtrackrr.com), where anyone with the link can follow your grail live. It also shows a popup in game for each new grail item.

Created by PyroSplat. Not affiliated with Blizzard Entertainment.

## Install
1. Install [D2RLoader](https://d2rloader.net/download.html) and check the game starts with it.
2. Get GrailTrackrr from the [D2RLoader plugin hub](https://d2rloader.net/hub/plugins/grailtrackrr-live-grail-sharing) (or [Releases](https://github.com/pyrosplat/grailtrackrr-plugin/releases/latest) here) and copy `d2rl-grailtrackrr.dll` into `<game folder>\d2rloader\plugins`. Each GitHub release lists the file's SHA-256 so you can check your download.
3. Start the game once. D2RLoader creates `<game folder>\d2rloader\config\grailtrackrr.toml`.
4. Sign up at grailtrackrr.com, open **My grail**, copy your API key and paste it as `api_key` in that file.
5. Start a grail on the site, then load a new level 1 character with an empty shared stash. The grail starts on its own.

In game, the console command `grailtrackrr` shows the connection status.

## Settings (`grailtrackrr.toml`)
| Key | Default | |
| --- | --- | --- |
| `server_url` | `https://grailtrackrr.com` | Use `http://localhost:8080` for a server on your own PC |
| `api_key` | | From the My grail page. Keep it private |
| `save_folder` | | Empty: `Saved Games\Diablo II Resurrected` (or the active mod's folder) |
| `popup_seconds` | `6` | How long the in-game popup stays |
| `popup_position` | `top` | `top` or `center` |
| `show_status` | `true` | Small connection line in the bottom-left corner |
| `debug` | `false` | Logs new items and UI messages to the D2RLoader log |
| `item_text` | `false` | Leave off (asks the game for tooltips, which trips an assertion on exit) |

## What it sends
Only to the `server_url` you set, signed with your API key:
- **New items** you pick up, gamble, buy or make in the Cube: item code, quality, item level, seed and stats, plus where you were (area, difficulty, character level, last monster killed).
- **Your character**: the `.d2s` save file of the loaded character when the game writes it (for gear, stats, skills and merc), and level-ups.
- **Presence**: that you are playing, your area and difficulty, about once a minute. **Deaths**, with the area.
- **Save folder check**: the names and levels of the characters in your save folder and whether the shared stash is empty, used to confirm a clean grail start.

Nothing else from your PC. If the server cannot be reached, unsent events wait in `%LOCALAPPDATA%\GrailTrackrr\outbox.jsonl` and go out on the next connection.

## Upgrading from "Live Grail"
Delete the old `d2rl-live-grail.dll` from the plugins folder. Until `grailtrackrr.toml` has a key, the plugin reads your old `live-grail.toml`.

## Problems or questions
Open an [issue](https://github.com/pyrosplat/grailtrackrr-plugin/issues). Include the D2RLoader log lines that start with `GrailTrackrr`.

## Build it yourself
Double-click `build.cmd` (it installs anything missing with winget and fetches the [D2RLoader PluginSDK](https://github.com/D2RLoader/PluginSDK)). The DLL lands in `dist\`. `.\build.ps1 -GameDir "<game folder>"` also installs it.

Releases are built by GitHub Actions from a version tag (`v0.7.1`, matching `PluginVersion` in `src/grailtrackrr.cpp`).

## License
The source is public so you can see exactly what the plugin does. You may read it, build it for your own use and send improvements as pull requests. You may not publish or share modified builds, or anything named GrailTrackrr. See [LICENSE.md](LICENSE.md). Only download releases from this page.
