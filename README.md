# GrailTrackrr plugin

D2RLoader plugin for Diablo II: Resurrected (offline) that sends your Holy Grail finds to [grailtrackrr.com](https://grailtrackrr.com), where anyone with the link can follow your grail live. It also shows a popup in game for each new grail item.

Created by PyroSplat. Not affiliated with Blizzard Entertainment.

## Install
One plugin plus two small mods with their own save folders: `GrailTrackrr` for your grail, `GrailTrackrrRace` for races and challenges. The plugin only tracks while one of them runs; with no mod or any other mod it stays off.

1. Install [D2RLoader](https://d2rloader.net/download.html) and check the game starts with it.
2. Download the zip from the [D2RLoader plugin hub](https://d2rloader.net/hub/plugins/grailtrackrr-live-grail-sharing) or [Releases](https://github.com/pyrosplat/grailtrackrr-plugin/releases/latest) and drag its `d2rloader` and `mods` folders into your game folder.
3. In D2RLoader, **General > Startup > Default mod**: pick `GrailTrackrr` (keep "Allow mod extensions" on). Start the game once.
4. Sign up at grailtrackrr.com, open **Profile**, copy your API key and paste it as `api_key` in `<game folder>\d2rloader\config\grailtrackrr.toml`.
5. Start a grail on the site and make a new level 1 character. The grail starts on its own.

For a race or challenge, pick `GrailTrackrrRace` instead and make a new level 1 character. Races never count toward your grail.
Full steps, including moving an existing grail, are in `instructions.txt`. In game, the console command `grailtrackrr` shows the status.

## Settings (`grailtrackrr.toml`)
| Key | Default | |
| --- | --- | --- |
| `api_key` | | From the Profile page. Keep it private |
| `item_popup` | `true` | In-game card for each new grail item |
| `badge_popup` | `true` | In-game card for each badge earned |
| `popup_seconds` | `6` | How long each card stays on screen |
| `show_status` | `true` | Connection line in the bottom-left corner |

Self-hosting: add `server_url = "http://localhost:8080"` (or your own https address).

## What it sends
Only to the `server_url` you set, signed with your API key:
- **New items** you pick up, gamble, buy or make in the Cube: item code, quality, item level, seed and stats, plus where you were (area, difficulty, character level, last monster killed).
- **Your character**: the `.d2s` save file of the loaded character when the game writes it (for gear, stats, skills and merc), and level-ups.
- **Presence**: that you are playing, your area and difficulty, about once a minute. **Deaths**, with the area.
- **Races and challenges** (only while you are in one): act boss kills and quests completed, and a check of the race character: level, monsters killed this game, grail items carried, shared stash items and gold, whether it is a TCP/IP game, and when its save file was made.
- **Save folder check**: the names and levels of the characters in your save folder and whether the shared stash is empty, used to confirm a clean grail start.

Nothing else from your PC. If the server cannot be reached, unsent events wait in `%LOCALAPPDATA%\GrailTrackrr\outbox-<grail|race>.jsonl` and go out on the next connection.

## Upgrading from 1.0
Drag the new folders in, pick the `GrailTrackrr` mod and copy your grail characters and shared stash into `Saved Games\Diablo II Resurrected\mods\GrailTrackrr`. Your settings stay. The site only accepts 1.1.0 and newer, run under one of the two mods.

## Problems or questions
Open an [issue](https://github.com/pyrosplat/grailtrackrr-plugin/issues). Include the D2RLoader log lines that start with `GrailTrackrr`.

## Build it yourself
Double-click `build.cmd` (it installs anything missing with winget and fetches the [D2RLoader PluginSDK](https://github.com/D2RLoader/PluginSDK)). The release layout lands in `dist\`. `.\build.ps1 -GameDir "<game folder>"` also copies it into the game.

Test accounts (set by a site admin) can spawn items to test with: `grailtrackrr spawn <item name | set name | random>` in the D2RLoader console.
`grailtrackrr popup` shows a test popup.

Releases are built by GitHub Actions from a version tag (`v0.7.1`, matching `PluginVersion` in `src/grailtrackrr.cpp`).

## License
The source is public so you can see exactly what the plugin does. You may read it, build it for your own use and send improvements as pull requests. You may not publish or share modified builds, or anything named GrailTrackrr. See [LICENSE.md](LICENSE.md). Only download releases from this page.
