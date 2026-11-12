// GrailTrackrr: D2RLoader plugin that reports Holy Grail finds to grailtrackrr.com (or your own GrailTrackrr server).
//
// How it works
//  - When a character finishes loading, it sends a "baseline": every item the
//    character and shared stash hold, plus a scan of the save folder. The
//    server uses this to start a grail (clean level 1 start) and to spot items
//    that appeared without being found.
//  - While playing, it snapshots carried items about once a second. An item
//    with a seed it has not seen before is a new find. Only Unique, Set and
//    rune items are reported; the server decides what counts for the grail.
//    Runewords keep their base item's seed, so a known item whose sockets
//    fill up is checked again and reported once it becomes a runeword.
//  - Each find carries context: character level, area, difficulty, the last
//    monster kill and which panel was open (gamble, vendor, Cube).
//  - The server replies with any new grail entries, shown as an in-game popup.
//
// Threads: inventory reads run on the UI thread, monster deaths arrive on the
// game server thread, HTTP replies on a worker thread and drawing on the
// render thread. Shared state is guarded by g.lock.

#include <D2RLPlugin/api.h>

#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <optional>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

static constexpr const char* PluginVersion = "1.0.11";

static constexpr D2RL::PluginInfo GrailTrackrrInfo {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "grailtrackrr",
	.name        = "GrailTrackrr",
	.version     = PluginVersion,
	.author      = "PyroSplat",
	.description = "Sends Holy Grail finds to GrailTrackrr and shows them in game.",
	.flags       = D2RL::PluginFlags::Client,
};

// ---------------------------------------------------------------------------
// State

struct Config {
	std::string serverUrl = "https://grailtrackrr.com";
	std::string apiKey;
	std::string saveFolder;
	float       popupSeconds = 6.0F;
	bool        popupCenter  = false;
	bool        showStatus   = true;
	bool        debug        = false;
	// Asks the game to build item tooltips. Off by default: building them outside
	// the game's own hover leaks stat lists, which trips an assertion on exit.
	bool        itemText     = false;
	// Where the item check runs: the game thread sees the real items (with seeds), the UI
	// thread only the display copies. "ui" is the old behaviour, kept as a fallback.
	bool        scanOnGame   = true;
	// Reads each item's own stat rolls (exact rolls on the site, runeword detection).
	// A switch so it can be turned off while chasing the leaked-stats assertion on exit.
	bool        itemStats    = true;
	bool        itemPopups   = true;   // in-game card for a new grail item
	bool        badgePopups  = true;   // in-game card for a badge earned
	// Not in the default config: turns the item check off entirely, for troubleshooting.
	bool        scanItems    = true;
};

struct Popup {
	std::string category;
	std::string name;
	std::string detail;
	uint64_t    notBeforeMs = 0; // test popups wait until the console is closed
};

struct LastKill {
	uint32_t classId  = 0;
	int32_t  levelId  = -1;
	uint64_t atMs     = 0;
};

struct State {
	const D2RL::PluginContext*    ctx       = nullptr;
	const D2RL::InventoryService* inventory = nullptr;
	const D2RL::ItemService*      items     = nullptr;
	const D2RL::ThreadService*    threads   = nullptr;
	const D2RL::HttpService*      http      = nullptr;
	const D2RL::PanelService*     panels    = nullptr;   // optional: popups and status line

	Config config;

	std::mutex lock;
	// Session (one loaded game).
	bool                         inGame          = false;
	bool                         baselinePending = false;
	uint64_t                     readyAtMs       = 0;
	uint64_t                     sessionGen      = 0;
	// One id per character load, sent with every event. If the guess of which
	// character is loaded turns out wrong, the server moves that session's finds.
	long long                    session         = 0;
	std::unordered_map<std::wstring, fs::file_time_type> saveTimes;   // .d2s write times at load
	uint64_t                     leftAtMs        = 0;
	int32_t                      fieldAreaId     = -1;   // last area outside town, where a death happened
	// Items already seen this game, by ItemKey (not the bare seed: see ItemKey).
	std::unordered_set<uint64_t> knownSeeds;
	// Runes held, by code, counting stack quantities (the Runes stash tab stacks them, and a
	// rune's seed changes on every load). A count going up during play is a rune find.
	std::unordered_map<std::string, int> runeTotals;
	std::unordered_map<uint64_t, int>    runewordTries; // scans spent looking for a new runeword (see the item scan)
	std::unordered_set<uint64_t> runewordSeeds;
	// Socketed item count last checked for runeword status, by seed.
	std::unordered_map<uint64_t, uint32_t> socketChecked;
	std::string                  charName;
	bool                         hardcore     = false;
	int32_t                      clvl         = 0;
	int32_t                      areaId       = -1;
	int32_t                      difficulty   = -1;
	LastKill                     lastKill;
	// Panel state from UI messages: "gamble", "vendor", "cube" or "".
	std::string                  openPanel;
	// Character view: the loaded character's save file and what was last sent.
	fs::path                     charSavePath;
	fs::file_time_type           charSaveTime {};
	std::string                  equippedSignature;
	bool                         characterDirty  = false;
	uint64_t                     nextCharCheckMs = 0;
	uint64_t                     lastCharSentMs  = 0;
	// Inventory and belt view: what was last sent, and items whose stats were sent this game.
	std::string                  itemsSignature;
	bool                         itemsDirty      = false;
	uint64_t                     lastItemsSentMs = 0;
	std::unordered_set<uint64_t> statsSent;
	// Presence: tells the site this character is playing right now.
	bool                         presenceDirty   = false;
	uint64_t                     nextHeartbeatMs = 0;
	// Outgoing events (JSON objects) and the request in flight.
	std::vector<std::string>     outbox;
	bool                         sending      = false;
	size_t                       inFlight     = 0;    // events at the front of outbox being sent
	uint64_t                     nextSendMs   = 0;
	uint32_t                     failures     = 0;
	// Display.
	std::deque<Popup>            popups;

	// Races (from the server's RACE reply line): what to show, when to check, the start time.
	std::string                  raceState    = "none";   // none | check | ready | waiting | running | done
	bool                         raceNeedCheck = false;
	uint64_t                     raceStartMs  = 0;        // local NowMs() of the start (or of this racer's start)
	bool                         raceStartKnown = false;
	std::string                  raceTitle;
	bool                         raceCheckPending = false;
	std::string                  raceCheckPhase = "ready";
	uint64_t                     lastRaceCheckMs = 0;
	bool                         raceGoShown = false;
	// This game: monsters killed (a race character must start with none) and whether another
	// player killed something (a TCP/IP game).
	uint32_t                     killsThisGame = 0;
	uint32_t                     localPlayerId = 0xFFFFFFFFU;
	bool                         multiplayer   = false;
	// Shared stash: items seen in the last scan.
	uint32_t                     sharedItems   = 0;
	int32_t                      charClass     = -1;

	// Test accounts: "grailtrackrr spawn ..." (see the Test mode section).
	struct SpawnSpec {
		std::string code;
		std::string quality;
		int32_t     row = -1;
		bool        eth = false;
		std::string name;
	};
	bool                   testMode = false;  // the server says this is a test account
	std::string            spawnQuery;        // waiting to be looked up by the server
	bool                   spawnLookup = false;
	std::vector<SpawnSpec> spawnQueue;        // looked up, waiting to be created in game
	// Spawned items not yet picked up: their finds are reported as "test". code|quality|row -> expiry.
	std::unordered_map<std::string, uint64_t> spawnedPending;
	std::string                  statusLine   = "GrailTrackrr: connecting";
	std::string                  lastError;
	bool                         pinging      = false;
	uint64_t                     nextPingMs   = 0;

	std::atomic<bool> scanQueued { false };
	// When the pending scan was queued. Leaving a game discards queued game-thread work, so a
	// scan queued just before that never runs; the worker re-queues one that is overdue.
	std::atomic<uint64_t> scanQueuedAtMs { 0 };
	uint64_t          nextScanMs = 0;
};

static State g;

static auto NowMs() noexcept -> uint64_t {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

static auto WallMs() noexcept -> uint64_t {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// Lines for the D2RLoader console. Log() only reaches the log file, so messages the player
// should see are queued here and written from the UI thread (see FlushConsole).
static std::mutex               consoleLock;
static std::vector<std::string> consoleQueue;

static void Say(const char* fmt, ...) noexcept {
	char    buffer[512] {};
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	try {
		std::lock_guard guard(consoleLock);
		if (consoleQueue.size() < 50) {
			consoleQueue.emplace_back(buffer);
		}
	} catch (...) {
	}
	if (g.ctx != nullptr) {
		g.ctx->LogInfo(buffer);
	}
}

static void FlushConsole(const D2RL::PluginContext* context) noexcept {
	std::vector<std::string> lines;
	try {
		std::lock_guard guard(consoleLock);
		lines.swap(consoleQueue);
	} catch (...) {
		return;
	}
	for (const auto& line : lines) {
		context->WriteConsoleMessage(line.c_str());
	}
}

static void Log(const char* fmt, ...) noexcept {
	char    buffer[1024] {};
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	if (g.ctx != nullptr) {
		g.ctx->LogInfo(buffer);
	}
}

// ---------------------------------------------------------------------------
// Config: a tiny reader for the flat [grailtrackrr] table (or [live-grail] from older versions).

static auto Trim(std::string s) -> std::string {
	const auto first = s.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) {
		return {};
	}
	const auto last = s.find_last_not_of(" \t\r\n");
	return s.substr(first, last - first + 1);
}

static auto Unquote(std::string s) -> std::string {
	s = Trim(s);
	if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
		return s.substr(1, s.size() - 2);
	}
	// Strip a trailing comment from bare values.
	const auto hash = s.find('#');
	return Trim(hash == std::string::npos ? s : s.substr(0, hash));
}

// true/false, also on/off, yes/no and 1/0.
static auto IsOn(std::string value) -> bool {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value == "true" || value == "on" || value == "yes" || value == "1";
}

static void ParseConfig(const std::string& text) {
	bool        inTable = false;
	size_t      start   = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) {
			end = text.size();
		}
		std::string line = Trim(text.substr(start, end - start));
		start            = end + 1;
		if (line.empty() || line[0] == '#') {
			continue;
		}
		if (line[0] == '[') {
			inTable = line == "[grailtrackrr]" || line == "[live-grail]";
			continue;
		}
		if (!inTable) {
			continue;
		}
		const auto eq = line.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		const std::string key   = Trim(line.substr(0, eq));
		const std::string value = Unquote(line.substr(eq + 1));
		if (key == "server_url") {
			g.config.serverUrl = value.empty() ? "https://grailtrackrr.com" : value;
			while (!g.config.serverUrl.empty() && g.config.serverUrl.back() == '/') {
				g.config.serverUrl.pop_back();
			}
		} else if (key == "api_key") {
			g.config.apiKey = value;
		} else if (key == "save_folder") {
			g.config.saveFolder = value;
		} else if (key == "popup_seconds") {
			// 0 turns the popup off.
			const float seconds = static_cast<float>(std::atof(value.c_str()));
			g.config.popupSeconds = seconds <= 0.0F ? 0.0F : std::clamp(seconds, 1.0F, 30.0F);
		} else if (key == "popup_position") {
			g.config.popupCenter = value == "center";
		} else if (key == "show_status") {
			g.config.showStatus = IsOn(value);
		} else if (key == "debug") {
			g.config.debug = IsOn(value);
		} else if (key == "scan_thread") {
			g.config.scanOnGame = value != "ui";
		} else if (key == "item_text") {
			g.config.itemText = IsOn(value);
		} else if (key == "item_popup") {
			g.config.itemPopups = IsOn(value);
		} else if (key == "badge_popup") {
			g.config.badgePopups = IsOn(value);
		} else if (key == "item_stats") {
			g.config.itemStats = IsOn(value);
		} else if (key == "scan_items") {
			g.config.scanItems = IsOn(value);
		}
	}
}

static void LoadConfig() {
	std::string text(64 * 1024, '\0');
	uint32_t    required = 0;
	if (!g.ctx->ReadConfig(text.data(), static_cast<uint32_t>(text.size()), &required)) {
		Log("GrailTrackrr: could not read config file.");
		return;
	}
	text.resize(std::strlen(text.c_str()));
	ParseConfig(text);

	// Before the rename the plugin was "Live Grail" with live-grail.toml. If the new file has
	// no key yet, take the settings from the old file next to it so nothing needs re-entering.
	if (g.config.apiKey.empty() && g.ctx->pluginConfigPath != nullptr) {
		const fs::path old = fs::path(g.ctx->pluginConfigPath).parent_path() / L"live-grail.toml";
		std::ifstream  in(old, std::ios::binary);
		if (in) {
			const std::string oldText((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			ParseConfig(oldText);
			if (!g.config.apiKey.empty()) {
				Log("GrailTrackrr: using server_url and api_key from the old live-grail.toml. Copy them into grailtrackrr.toml when convenient.");
			}
		}
	}
}

// ---------------------------------------------------------------------------
// JSON writing helpers (the plugin only ever writes JSON).

static void JsonString(std::string& out, const std::string& s) {
	out += '"';
	for (const unsigned char c : s) {
		switch (c) {
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (c < 0x20) {
					char esc[8];
					std::snprintf(esc, sizeof(esc), "\\u%04x", c);
					out += esc;
				} else {
					out += static_cast<char>(c);
				}
		}
	}
	out += '"';
}

static auto JsonKey(std::string& out, const char* key) -> std::string& {
	if (out.back() != '{') {
		out += ',';
	}
	out += '"';
	out += key;
	out += "\":";
	return out;
}

static void JsonInt(std::string& out, const char* key, long long v) {
	JsonKey(out, key) += std::to_string(v);
}

static void JsonBool(std::string& out, const char* key, bool v) {
	JsonKey(out, key) += v ? "true" : "false";
}

static void JsonStr(std::string& out, const char* key, const std::string& v) {
	JsonString(JsonKey(out, key), v);
}

// ---------------------------------------------------------------------------
// Save folder: character files, used for the clean-start check and to find
// which character is loaded (the SDK does not expose the character name).

struct SaveChar {
	std::string     name;
	int             level    = 0;
	int             cls      = -1;
	bool            hardcore = false;
	fs::file_time_type modified {};
	fs::path        path;
};

static auto FromUtf8(const std::string& s) -> fs::path {
	return fs::path(std::u8string(s.begin(), s.end()));
}

static auto ToUtf8(const fs::path& p) -> std::string {
	const std::u8string s = p.u8string();
	return std::string(s.begin(), s.end());
}

static auto SaveFolder() -> fs::path {
	if (!g.config.saveFolder.empty()) {
		return FromUtf8(g.config.saveFolder);
	}
	PWSTR raw = nullptr;
	fs::path base;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &raw)) && raw != nullptr) {
		base = fs::path(raw) / L"Diablo II Resurrected";
	}
	CoTaskMemFree(raw);
	// Mods that use their own save folder keep it under mods/<mod>.
	if (g.ctx->activeMod != nullptr && g.ctx->activeMod[0] != '\0') {
		const fs::path modSaves = base / L"mods" / FromUtf8(g.ctx->activeMod);
		std::error_code ec;
		if (fs::is_directory(modSaves, ec)) {
			return modSaves;
		}
	}
	return base;
}

// D2S header layout depends on the save version (uint32 at offset 4).
// Reign of the Warlock saves (version 105+) moved status, class and level
// 16 bytes earlier: status 0x14 (bit 2 = hardcore), class 0x18, level 0x1B.
// Older D2R saves use status 0x24, class 0x28, level 0x2B.
struct HeaderOffsets {
	size_t status;
	size_t cls;
	size_t level;
};

static auto OffsetsFor(const unsigned char* data) -> HeaderOffsets {
	const uint32_t version = static_cast<uint32_t>(data[4]) | (static_cast<uint32_t>(data[5]) << 8) | (static_cast<uint32_t>(data[6]) << 16)
		| (static_cast<uint32_t>(data[7]) << 24);
	if (version >= 105) {
		return { 0x14, 0x18, 0x1B };
	}
	return { 0x24, 0x28, 0x2B };
}

// The character level is also stored in the "gf" stats block (stat 12), which
// keeps its format across versions; used when the header byte looks wrong.
static auto LevelFromStats(const std::vector<unsigned char>& data) -> int {
	static constexpr int Bits[16] { 10, 10, 10, 10, 10, 8, 21, 21, 21, 21, 21, 21, 7, 32, 25, 25 };
	for (size_t i = 0x14F; i + 1 < data.size(); ++i) {
		if (data[i] != 'g' || data[i + 1] != 'f') {
			continue;
		}
		size_t     bit  = (i + 2) * 8;
		const auto read = [&](int n) -> uint64_t {
			uint64_t v = 0;
			for (int k = 0; k < n; ++k, ++bit) {
				if (bit / 8 >= data.size()) {
					return 0x1FF;
				}
				if (data[bit / 8] & (1U << (bit % 8))) {
					v |= 1ULL << k;
				}
			}
			return v;
		};
		for (int guard = 0; guard < 32; ++guard) {
			const uint64_t id = read(9);
			if (id >= 16) {
				return 0;
			}
			const uint64_t value = read(Bits[id]);
			if (id == 12) {
				return static_cast<int>(value);
			}
		}
		return 0;
	}
	return 0;
}
// When a save file was created (ms since 1970), or 0. A race character must be new.
static auto CreatedMs(const fs::path& path) -> long long {
	WIN32_FILE_ATTRIBUTE_DATA data {};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
		return 0;
	}
	const uint64_t ticks = (static_cast<uint64_t>(data.ftCreationTime.dwHighDateTime) << 32) | data.ftCreationTime.dwLowDateTime;
	if (ticks < 116444736000000000ULL) {
		return 0;
	}
	return static_cast<long long>((ticks - 116444736000000000ULL) / 10'000ULL);
}

// Gold in the shared stash file: every page starts 0xAA55AA55, with its gold at +12 and
// its size at +16. Items are counted by the item scan instead.
static auto SharedStashGold(bool hardcore) -> long long {
	const fs::path file = SaveFolder() / (hardcore ? L"SharedStashHardCoreV2.d2i" : L"SharedStashSoftCoreV2.d2i");
	std::ifstream in(file, std::ios::binary);
	if (!in) {
		return 0;
	}
	std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	const auto u32 = [&](size_t at) -> uint32_t {
		return at + 4 <= data.size() ? static_cast<uint32_t>(data[at]) | (static_cast<uint32_t>(data[at + 1]) << 8) | (static_cast<uint32_t>(data[at + 2]) << 16)
			| (static_cast<uint32_t>(data[at + 3]) << 24) : 0;
	};
	long long gold = 0;
	size_t    at   = 0;
	for (int guard = 0; guard < 64 && at + 64 <= data.size(); ++guard) {
		if (u32(at) != 0xAA55AA55U) {
			break;
		}
		gold += u32(at + 12);
		const uint32_t size = u32(at + 16);
		if (size < 64) {
			break;
		}
		at += size;
	}
	return gold;
}

// The file name is the character name.
static auto ScanSaves() -> std::vector<SaveChar> {
	std::vector<SaveChar> out;
	std::error_code       ec;
	const fs::path        folder = SaveFolder();
	for (const auto& entry : fs::directory_iterator(folder, ec)) {
		if (!entry.is_regular_file(ec) || entry.path().extension() != L".d2s") {
			continue;
		}
		std::ifstream file(entry.path(), std::ios::binary);
		unsigned char header[0x30] {};
		if (!file.read(reinterpret_cast<char*>(header), sizeof(header))) {
			continue;
		}
		SaveChar c;
		c.name     = ToUtf8(entry.path().stem());
		const HeaderOffsets at = OffsetsFor(header);
		c.hardcore = (header[at.status] & 0x04) != 0;
		c.level    = header[at.level];
		c.cls      = header[at.cls] <= 6 ? header[at.cls] : -1;
		if (c.level < 1 || c.level > 99) {
			file.clear();
			file.seekg(0);
			std::vector<unsigned char> all((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			c.level = LevelFromStats(all);
		}
		c.modified = entry.last_write_time(ec);
		c.path     = entry.path();
		out.push_back(std::move(c));
	}
	return out;
}

// ---------------------------------------------------------------------------
// Item helpers

static auto ContainerName(D2RL::Items::ItemContainer c) -> const char* {
	using C = D2RL::Items::ItemContainer;
	switch (c) {
		case C::Equipment:     return "equipment";
		case C::Cursor:        return "cursor";
		case C::Belt:          return "belt";
		case C::Inventory:     return "inventory";
		case C::Cube:          return "cube";
		case C::Trade:         return "trade";
		case C::PersonalStash: return "stash";
		case C::SharedStash:   return "sharedstash";
		case C::CustomPage:    return "custom";
		case C::Ground:        return "ground";
		default:               return "unknown";
	}
}

static auto QualityName(D2RL::Items::Quality q) -> const char* {
	using Q = D2RL::Items::Quality;
	switch (q) {
		case Q::Inferior: return "inferior";
		case Q::Normal:   return "normal";
		case Q::Superior: return "superior";
		case Q::Magic:    return "magic";
		case Q::Set:      return "set";
		case Q::Rare:     return "rare";
		case Q::Unique:   return "unique";
		case Q::Crafted:  return "crafted";
		default:          return "unknown";
	}
}

static auto CodeString(uint32_t code) -> std::string {
	std::string s;
	for (int i = 0; i < 4; ++i) {
		const char c = static_cast<char>((code >> (8 * i)) & 0xFF);
		if (c != '\0' && c != ' ') {
			s += c;
		}
	}
	return s;
}

// One item's identity within a game. Not the bare itemSeed: if the loader reports no seed
// (0) or the same seed for several items, a new grail item would look already known and
// its find would never be sent. Two items only share a key if seeds, base and quality match.
// Without any seed, the runtime id (stable for the whole game) keeps items apart.
static auto ItemKey(const D2RL::Items::ItemInfo& item) -> uint64_t {
	uint64_t h = 1469598103934665603ULL;
	const auto mix = [&h](uint64_t v) {
		h ^= v;
		h *= 1099511628211ULL;
	};
	const bool seeded = item.itemSeed != 0 || item.generationSeed != 0;
	mix(item.itemSeed);
	mix(item.generationSeed);
	mix(item.code);
	mix(static_cast<uint32_t>(item.quality));
	mix(static_cast<uint32_t>(item.qualityRecordId));
	if (!seeded) {
		mix(0xFFFFFFFFULL + item.runtimeId);
	}
	return h;
}

// Identifies a spawned item until its find is reported (see the Test mode section).
static auto SpawnSignature(const std::string& code, const std::string& quality, int32_t row) -> std::string {
	return code + "|" + quality + "|" + std::to_string(quality == "unique" || quality == "set" ? row : -1);
}

// The seed the server stores for an item: itemSeed, else generationSeed, else none (0).
static auto ReportSeed(const D2RL::Items::ItemInfo& item) -> uint32_t {
	return item.itemSeed != 0 ? item.itemSeed : item.generationSeed;
}

static auto IsRuneCode(const std::string& code) -> bool {
	return code.size() == 3 && code[0] == 'r' && std::isdigit(static_cast<unsigned char>(code[1])) && std::isdigit(static_cast<unsigned char>(code[2]));
}

// Grail candidates: anything Unique or Set, and runes.
static auto IsCandidate(const D2RL::Items::ItemInfo& item) -> bool {
	return item.quality == D2RL::Items::Quality::Unique || item.quality == D2RL::Items::Quality::Set || IsRuneCode(CodeString(item.code));
}

// Runes held by code, counting each stack's quantity.
static auto RuneTotals(const std::vector<D2RL::Items::ItemInfo>& items) -> std::unordered_map<std::string, int> {
	std::unordered_map<std::string, int> totals;
	for (const auto& item : items) {
		if (const auto code = CodeString(item.code); IsRuneCode(code)) {
			totals[code] += item.quantity > 0 ? item.quantity : 1;
		}
	}
	return totals;
}

// Runes.txt row of a finished runeword, or -1. UI thread only. Only items with
// every socket filled can be runewords, so other items are skipped cheaply.
static auto RunewordRow(const D2RL::Items::ItemInfo& item) -> int32_t {
	using Q = D2RL::Items::Quality;
	if ((item.quality != Q::Normal && item.quality != Q::Superior) || item.socketCount == 0 || item.socketedItemCount != item.socketCount) {
		return -1;
	}
	if (!g.config.itemStats || !D2RL::HasItemServiceField(g.items, D2RL::ItemServiceDetailsFieldEnd) || g.items->getItemDetails == nullptr) {
		return -1;
	}
	D2RL::Items::ItemDetails     details { .structSize = D2RL::Items::ItemDetailsSize };
	D2RL::Items::ItemStatValue   stats[128] {};
	if (g.items->getItemDetails(g.ctx, item.handle, &details, stats, 128) != D2RL::Items::Result::Success) {
		return -1;
	}
	return (details.flags & D2RL::Items::ItemDetailRuneword) != 0 ? details.runewordRecordId : -1;
}

// First tooltip line, with D2 color codes removed. Optional: the server only
// uses it to catch catalog mismatches.
static auto ItemName(D2RL::ItemHandle handle) -> std::string {
	if (!g.config.itemText || !D2RL::HasItemServiceField(g.items, D2RL::ItemServiceTooltipFieldEnd) || g.items->getItemTooltip == nullptr) {
		return {};
	}
	char     buffer[2048] {};
	uint32_t required = 0;
	if (g.items->getItemTooltip(g.ctx, handle, buffer, sizeof(buffer), &required) != D2RL::Items::Result::Success) {
		return {};
	}
	// The name is the first line of a multi-line tooltip in D2R; take it either way.
	std::string text(buffer);
	const auto  nl = text.find('\n');
	std::string line = nl == std::string::npos ? text : text.substr(0, nl);
	std::string clean;
	for (size_t i = 0; i < line.size(); ++i) {
		// Color codes are "\xC3\xBF" + 'c' + one character (UTF-8 for y-diaeresis).
		if (static_cast<unsigned char>(line[i]) == 0xC3 && i + 3 < line.size() && static_cast<unsigned char>(line[i + 1]) == 0xBF && line[i + 2] == 'c') {
			i += 3;
			continue;
		}
		clean += line[i];
	}
	return Trim(clean);
}

// The item's own magic stats as [statId, layer, value] so the site can show exact rolls.
// UI thread only. Optional: older loaders without getItemDetails simply send none.
static void ItemStatsJson(std::string& out, const D2RL::Items::ItemInfo& item) {
	if (!g.config.itemStats || !D2RL::HasItemServiceField(g.items, D2RL::ItemServiceDetailsFieldEnd) || g.items->getItemDetails == nullptr) {
		return;
	}
	D2RL::Items::ItemDetails   details { .structSize = D2RL::Items::ItemDetailsSize };
	D2RL::Items::ItemStatValue stats[128] {};
	if (g.items->getItemDetails(g.ctx, item.handle, &details, stats, 128) != D2RL::Items::Result::Success) {
		return;
	}
	const uint32_t count = details.statCount < 128 ? details.statCount : 128;
	JsonKey(out, "stats") += '[';
	for (uint32_t i = 0; i < count; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += '[' + std::to_string(stats[i].statId) + ',' + std::to_string(stats[i].layer) + ',' + std::to_string(stats[i].value) + ']';
	}
	out += ']';
}

static void ItemJson(std::string& out, const D2RL::Items::ItemInfo& item, bool withName, int32_t runeword = -1) {
	out += '{';
	if (ReportSeed(item) != 0) {
		JsonInt(out, "seed", ReportSeed(item));
	}
	JsonStr(out, "code", CodeString(item.code));
	JsonStr(out, "quality", QualityName(item.quality));
	JsonInt(out, "row", item.qualityRecordId);
	JsonBool(out, "eth", (item.stateFlags & D2RL::Items::ItemStateEthereal) != 0);
	JsonInt(out, "ilvl", item.itemLevel);
	JsonStr(out, "container", ContainerName(item.container));
	if (runeword >= 0) {
		JsonInt(out, "runeword", runeword);
	}
	if (withName) {
		const std::string name = ItemName(item.handle);
		if (!name.empty()) {
			JsonStr(out, "name", name);
		}
		ItemStatsJson(out, item);
	}
	out += '}';
}

// ---------------------------------------------------------------------------
// Sending

// Events wait here until the server accepts them. The queue is also saved to
// disk (see SaveOutbox), so finds made while offline are sent next time.
static constexpr size_t OutboxLimit = 5000;

static auto IsType(const std::string& json, const char* type) -> bool {
	return json.find(std::string("\"type\":\"") + type + "\"") != std::string::npos;
}

// Caller holds g.lock.
static void QueueEventLocked(std::string json) {
	if (g.session != 0 && json.size() > 2 && json.front() == '{' && json.find("\"session\":") == std::string::npos) {
		json.insert(1, "\"session\":" + std::to_string(g.session) + ",");
	}
	// Only the newest character snapshot and presence matter; drop older unsent ones.
	for (const char* type : { "character", "presence", "items" }) {
		if (IsType(json, type)) {
			const auto from = g.outbox.begin() + static_cast<std::ptrdiff_t>(std::min(g.inFlight, g.outbox.size()));
			g.outbox.erase(std::remove_if(from, g.outbox.end(), [type](const std::string& e) { return IsType(e, type); }), g.outbox.end());
		}
	}
	if (g.outbox.size() < OutboxLimit) {
		g.outbox.push_back(std::move(json));
	}
}

static void QueueEvent(std::string json) {
	std::lock_guard guard(g.lock);
	QueueEventLocked(std::move(json));
}

static void HandleReply(const std::string& body) {
	size_t start = 0;
	while (start < body.size()) {
		size_t end = body.find('\n', start);
		if (end == std::string::npos) {
			end = body.size();
		}
		const std::string line = body.substr(start, end - start);
		start                  = end + 1;

		std::vector<std::string> parts;
		size_t                   p = 0;
		while (true) {
			const size_t tab = line.find('\t', p);
			parts.push_back(line.substr(p, tab == std::string::npos ? std::string::npos : tab - p));
			if (tab == std::string::npos) {
				break;
			}
			p = tab + 1;
		}
		std::lock_guard guard(g.lock);
		if (parts[0] == "STATUS" && parts.size() >= 3) {
			if (parts[1] == "active") {
				g.statusLine = "GrailTrackrr connected: " + parts[2];
			} else if (parts[1] == "pending") {
				g.statusLine = "GrailTrackrr not started: " + parts[2];
			} else {
				g.statusLine = "GrailTrackrr: " + parts[2];
			}
		} else if (parts[0] == "RACE" && parts.size() >= 4) {
			// RACE <state> <1 = check now> <ms to the start> <title> <message>
			const std::string before = g.raceState;
			g.raceState     = parts[1];
			g.raceNeedCheck = parts[2] == "1";
			const long long toStart = std::atoll(parts[3].c_str());
			g.raceStartMs    = static_cast<uint64_t>(static_cast<long long>(NowMs()) + toStart);
			g.raceStartKnown = g.raceState != "none";
			g.raceTitle      = parts.size() >= 5 ? parts[4] : "";
			if (g.raceState != before && (g.raceState == "check" || g.raceState == "ready")) {
				g.raceCheckPhase = "ready";
			}
			if (g.raceState != before) {
				Log("GrailTrackrr: race %s: %s", g.raceState.c_str(), parts.size() >= 6 ? parts[5].c_str() : "");
				if (g.raceState != "ready" && g.raceState != "running") {
					g.raceGoShown = false;
				}
			}
		} else if (parts[0] == "TEST") {
			g.testMode = parts.size() >= 2 && parts[1] == "1";
		} else if (parts[0] == "POPUP" && parts.size() >= 4) {
			// item_popup / badge_popup turn the in-game cards off; the console still gets them.
			// Race cards always show.
			const bool race = parts[1] == "ready" || parts[1] == "notready" || parts[1] == "go" || parts[1] == "finish";
			if (!race && !(parts[1] == "badge" ? g.config.badgePopups : g.config.itemPopups)) {
				Say(parts[1] == "badge" ? "GrailTrackrr: badge earned: %s (%s)" : "GrailTrackrr: %s - %s", parts[2].c_str(), parts[3].c_str());
				continue;
			}
			g.popups.push_back(Popup { parts[1], parts[2], parts[3] });
			if (g.popups.size() > 4) {
				g.popups.pop_front();
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Connection status: plain-language reasons, shown in game and in the log.

static auto ResultName(D2RL::Http::Result r) -> const char* {
	using R = D2RL::Http::Result;
	switch (r) {
		case R::Success:          return "success";
		case R::InvalidArgument:  return "invalid request";
		case R::Unsupported:      return "unsupported";
		case R::Unavailable:      return "HTTP service unavailable";
		case R::Timeout:          return "timed out";
		case R::NetworkError:     return "network error";
		case R::TlsError:         return "HTTPS certificate error";
		case R::ResponseTooLarge: return "response too large";
		default:                  return "error";
	}
}

// ---------------------------------------------------------------------------
// HTTP transport. HTTPS goes through D2RLoader's HTTP service. Plain http://
// (localhost or a server on your network) goes through Windows WinHTTP,
// because some D2RLoader builds reject plain HTTP outright. If the loader's
// service turns out to be unusable, everything falls back to WinHTTP.

struct HttpResult {
	bool        reached = false; // got an HTTP response of any status
	uint32_t    status  = 0;
	std::string body;
	std::string error;           // why the server could not be reached
};
using HttpDone = void (*)(const HttpResult& result, void* user);

static std::atomic<bool> forceWinHttp { false };

static auto UseWinHttp() -> bool {
	return forceWinHttp || g.config.serverUrl.rfind("http://", 0) == 0;
}

static auto Widen(const std::string& s) -> std::wstring {
	if (s.empty()) {
		return {};
	}
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring out(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
	return out;
}

static auto WinHttpError(DWORD code) -> std::string {
	switch (code) {
		case ERROR_WINHTTP_CANNOT_CONNECT:      return "connection refused, is the server running?";
		case ERROR_WINHTTP_TIMEOUT:             return "timed out";
		case ERROR_WINHTTP_NAME_NOT_RESOLVED:   return "server name not found";
		case ERROR_WINHTTP_SECURE_FAILURE:      return "HTTPS certificate error";
		case ERROR_WINHTTP_CONNECTION_ERROR:    return "connection dropped";
		case ERROR_WINHTTP_INVALID_URL:
		case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return "server_url is not a valid URL";
		default:                                return "network error " + std::to_string(code);
	}
}

// Blocking request; only ever called from the worker thread.
static auto WinHttpRequest(const wchar_t* method, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body)
	-> HttpResult {
	HttpResult result;
	const std::wstring wurl = Widen(url);
	URL_COMPONENTS     parts {};
	parts.dwStructSize      = sizeof(parts);
	parts.dwHostNameLength  = static_cast<DWORD>(-1);
	parts.dwUrlPathLength   = static_cast<DWORD>(-1);
	parts.dwExtraInfoLength = static_cast<DWORD>(-1);
	if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
		result.error = "server_url is not a valid URL";
		return result;
	}
	const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
	std::wstring       path(parts.lpszUrlPath, parts.dwUrlPathLength);
	path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
	const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

	HINTERNET session = WinHttpOpen(L"GrailTrackrr", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (session == nullptr) {
		// Older Windows without automatic proxy support.
		session = WinHttpOpen(L"GrailTrackrr", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	}
	HINTERNET connect = session != nullptr ? WinHttpConnect(session, host.c_str(), parts.nPort, 0) : nullptr;
	HINTERNET request = connect != nullptr ? WinHttpOpenRequest(connect, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
	if (request == nullptr) {
		result.error = WinHttpError(GetLastError());
	} else {
		WinHttpSetTimeouts(request, 5'000, 5'000, 15'000, 15'000);
		std::wstring headerText;
		for (const auto& [name, value] : headers) {
			headerText += Widen(name) + L": " + Widen(value) + L"\r\n";
		}
		const BOOL sent = WinHttpSendRequest(request, headerText.c_str(), static_cast<DWORD>(-1L), body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
			static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
		if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
			result.error = WinHttpError(GetLastError());
		} else {
			DWORD status = 0;
			DWORD size   = sizeof(status);
			WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
			result.reached = true;
			result.status  = status;
			DWORD available = 0;
			while (WinHttpQueryDataAvailable(request, &available) && available > 0 && result.body.size() < 1'000'000) {
				std::string chunk(available, '\0');
				DWORD       read = 0;
				if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
					break;
				}
				result.body.append(chunk.data(), read);
			}
		}
	}
	if (request != nullptr) WinHttpCloseHandle(request);
	if (connect != nullptr) WinHttpCloseHandle(connect);
	if (session != nullptr) WinHttpCloseHandle(session);
	return result;
}

struct SdkCall {
	HttpDone done;
	void*    user;
};

static void __cdecl OnSdkResponse(const D2RL::PluginContext*, const D2RL::Http::Response* response, void* userData) noexcept {
	auto* call = static_cast<SdkCall*>(userData);
	try {
		HttpResult result;
		if (!D2RL::Http::HasResponseField(response, D2RL::Http::ResponseRequiredSize)) {
			result.error = "no response";
		} else if (response->result != D2RL::Http::Result::Success) {
			result.error = ResultName(response->result);
		} else {
			result.reached = true;
			result.status  = response->statusCode;
			result.body.assign(static_cast<const char*>(response->body), response->bodySize);
		}
		call->done(result, call->user);
	} catch (...) {
	}
	delete call;
}

// Sends a request and calls done exactly once: right away for WinHTTP, later for the loader's service.
static void HttpSend(bool post, const std::string& path, const std::string& body, HttpDone done, void* user) {
	const std::string url  = g.config.serverUrl + path;
	const std::string auth = "Bearer " + g.config.apiKey;
	if (!UseWinHttp() && g.http != nullptr) {
		const D2RL::Http::Header headers[] {
			{ .name = "Content-Type", .value = "application/json" },
			{ .name = "Accept", .value = "text/plain" },
			{ .name = "Authorization", .value = auth.c_str() },
		};
		const D2RL::Http::Request request {
			.structSize  = D2RL::Http::RequestSize,
			.method      = post ? D2RL::Http::Method::Post : D2RL::Http::Method::Get,
			.timeoutMs   = 15'000,
			.url         = url.c_str(),
			.headerCount = post ? 3U : 2U,
			.bodySize    = static_cast<uint32_t>(body.size()),
			.headers     = post ? headers : headers + 1,
			.body        = body.empty() ? nullptr : body.data(),
		};
		auto*                     call   = new SdkCall { done, user };
		D2RL::Http::RequestHandle handle = D2RL::Http::InvalidRequestHandle;
		const D2RL::Http::Result  sent   = g.http->send(g.ctx, &request, OnSdkResponse, call, &handle);
		if (sent == D2RL::Http::Result::Success) {
			return;
		}
		delete call;
		// The loader's HTTP service refused the request itself; use WinHTTP from now on.
		forceWinHttp = true;
		Log("GrailTrackrr: D2RLoader HTTP service refused the request (%s); using Windows HTTP instead.", ResultName(sent));
	}
	std::vector<std::pair<std::string, std::string>> headers { { "Accept", "text/plain" }, { "Authorization", auth } };
	if (post) {
		headers.insert(headers.begin(), { "Content-Type", "application/json" });
	}
	done(WinHttpRequest(post ? L"POST" : L"GET", url, headers, body), user);
}

static void SetStatus(bool ok, const std::string& line) {
	std::lock_guard guard(g.lock);
	g.statusLine = line;
	const std::string error = ok ? "" : line;
	if (error != g.lastError) {
		g.lastError = error;
		if (!error.empty() && g.ctx != nullptr) {
			g.ctx->LogWarn(error.c_str());
		}
	}
}

static void ReportFailure(const HttpResult& r) {
	const std::string& url = g.config.serverUrl;
	size_t waiting = 0;
	{
		std::lock_guard guard(g.lock);
		waiting = static_cast<size_t>(std::count_if(g.outbox.begin(), g.outbox.end(), [](const std::string& e) { return IsType(e, "find"); }));
	}
	if (!r.reached) {
		SetStatus(false, "GrailTrackrr: cannot reach " + url + " (" + r.error + ")" +
			(waiting > 0 ? ", " + std::to_string(waiting) + " find(s) saved to send later" : std::string()));
	} else if (r.status == 401) {
		SetStatus(false, "GrailTrackrr: API key rejected, copy it again from the My grail page");
	} else if (r.status == 404) {
		SetStatus(false, "GrailTrackrr: " + url + " is not a GrailTrackrr server (404)");
	} else {
		SetStatus(false, "GrailTrackrr: server error " + std::to_string(r.status));
	}
}

static void OnSendFinished(const HttpResult& r, void* user) {
	auto*      batch = static_cast<std::vector<std::string>*>(user);
	const bool ok    = r.reached && r.status >= 200 && r.status < 300;
	try {
		if (ok) {
			{
				std::lock_guard guard(g.lock);
				g.failures = 0;
				g.lastError.clear();
				// The sent events are still at the front of the queue; drop them now they arrived.
				g.outbox.erase(g.outbox.begin(), g.outbox.begin() + static_cast<std::ptrdiff_t>(std::min(g.inFlight, g.outbox.size())));
				g.inFlight = 0;
			}
			HandleReply(r.body);
		} else {
			{
				std::lock_guard guard(g.lock);
				// The batch never left the queue; retry with backoff.
				g.inFlight = 0;
				g.failures++;
				g.nextSendMs = NowMs() + std::min<uint64_t>(60'000, 2'000ULL << std::min<uint32_t>(g.failures, 5));
			}
			ReportFailure(r);
		}
	} catch (...) {
	}
	{
		std::lock_guard guard(g.lock);
		g.sending = false;
	}
	delete batch;
}

static void TrySend() {
	if (g.config.serverUrl.empty() || g.config.apiKey.empty()) {
		return;
	}
	auto* batch = new std::vector<std::string>();
	{
		std::lock_guard guard(g.lock);
		if (g.sending || g.outbox.empty() || NowMs() < g.nextSendMs) {
			delete batch;
			return;
		}
		const size_t n = std::min<size_t>(g.outbox.size(), 100);
		batch->assign(g.outbox.begin(), g.outbox.begin() + static_cast<std::ptrdiff_t>(n));
		// Stays in the queue (and on disk) until the server confirms it.
		g.inFlight = n;
		g.sending  = true;
	}
	// sentAt lets the server correct for a PC clock that is off.
	std::string body = "{\"plugin\":{\"version\":\"";
	body += PluginVersion;
	body += "\",\"sentAt\":" + std::to_string(WallMs()) + "},\"events\":[";
	for (size_t i = 0; i < batch->size(); ++i) {
		if (i != 0) {
			body += ',';
		}
		body += (*batch)[i];
	}
	body += "]}";
	HttpSend(true, "/api/plugin/events", body, OnSendFinished, batch);
}

// Checks the server and API key without waiting for game events, so the
// status line is right from the main menu on.
static void OnPingFinished(const HttpResult& r, void*) {
	const bool ok = r.reached && r.status >= 200 && r.status < 300;
	try {
		if (ok) {
			bool first = false;
			{
				std::lock_guard guard(g.lock);
				first = g.lastError.empty() && g.statusLine == "GrailTrackrr: connecting";
				g.lastError.clear();
			}
			HandleReply(r.body);
			std::string line;
			{
				std::lock_guard guard(g.lock);
				line = g.statusLine;
			}
			if (first) {
				Log("%s (server %s)", line.c_str(), g.config.serverUrl.c_str());
			}
		} else {
			ReportFailure(r);
		}
	} catch (...) {
	}
	std::lock_guard guard(g.lock);
	g.pinging    = false;
	g.nextPingMs = NowMs() + (ok ? 120'000 : 20'000);
}

static void TryPing() {
	if (g.config.serverUrl.empty() || g.config.apiKey.empty()) {
		return;
	}
	{
		std::lock_guard guard(g.lock);
		if (g.pinging || g.inGame || NowMs() < g.nextPingMs) {
			return;
		}
		g.pinging = true;
	}
	HttpSend(false, "/api/plugin/status", {}, OnPingFinished, nullptr);
}

// ---------------------------------------------------------------------------
// Inventory scanning (UI thread)

struct ScanResult {
	std::vector<D2RL::Items::ItemInfo> items;
};

static auto __cdecl CollectItem(const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item, void* userData) noexcept -> D2RL::Inventory::IterationAction {
	try {
		static_cast<ScanResult*>(userData)->items.push_back(*item);
	} catch (...) {
	}
	return D2RL::Inventory::IterationAction::Continue;
}

// For races: what the site checks about a race character (with the items the event carries).
static void RaceFields(std::string& ev, const char* phase) {
	uint32_t  kills = 0;
	bool      mp = false, hc = false;
	int32_t   cls = -1;
	fs::path  save;
	{
		std::lock_guard guard(g.lock);
		kills = g.killsThisGame;
		mp    = g.multiplayer;
		hc    = g.hardcore;
		cls   = g.charClass;
		save  = g.charSavePath;
	}
	JsonStr(ev, "phase", phase);
	JsonInt(ev, "kills", kills);
	JsonBool(ev, "multiplayer", mp);
	JsonInt(ev, "classId", cls);
	JsonInt(ev, "sharedGold", SharedStashGold(hc));
	JsonInt(ev, "saveCreated", save.empty() ? 0 : CreatedMs(save));
}

// A race check of the loaded character: level, kills, grail items carried and the shared stash.
static void SendRaceCheck(const std::vector<D2RL::Items::ItemInfo>& items, const std::string& phase) {
	std::string name;
	int32_t     clvl = 0;
	bool        hardcore = false;
	{
		std::lock_guard guard(g.lock);
		name     = g.charName;
		clvl     = g.clvl;
		hardcore = g.hardcore;
	}
	if (name.empty()) {
		return;
	}
	std::string ev = "{";
	JsonStr(ev, "type", "racecheck");
	JsonStr(ev, "charName", name);
	JsonInt(ev, "clvl", clvl);
	JsonBool(ev, "hardcore", hardcore);
	JsonInt(ev, "ts", static_cast<long long>(WallMs()));
	RaceFields(ev, phase.c_str());
	JsonKey(ev, "items") += '[';
	bool first = true;
	for (const auto& item : items) {
		if (!IsCandidate(item) && RunewordRow(item) < 0 && item.container != D2RL::Items::ItemContainer::SharedStash) {
			continue;
		}
		if (!first) {
			ev += ',';
		}
		first = false;
		ItemJson(ev, item, false, RunewordRow(item));
	}
	ev += "]}";
	QueueEvent(std::move(ev));
	Log("GrailTrackrr: race check sent for %s (%s).", name.c_str(), phase.c_str());
}

static void SendBaseline(const std::vector<D2RL::Items::ItemInfo>& items, const std::unordered_map<uint64_t, int32_t>& runewords) {
	const auto saves = ScanSaves();
	std::string name;
	bool        hardcore = false;
	int         level    = 0;
	{
		// The game does not say which character is loaded. Best guess: the save written
		// most recently, preferring saves at the live level when the game reported it.
		// WatchSaves corrects the guess as soon as the game writes the real save.
		int liveLevel = 0;
		{
			std::lock_guard guard(g.lock);
			liveLevel = g.clvl;
		}
		const SaveChar* newest = nullptr;
		for (const auto& c : saves) {
			const bool levelOk = liveLevel <= 0 || c.level == liveLevel;
			const bool bestOk  = newest == nullptr || liveLevel <= 0 || newest->level == liveLevel;
			if (newest == nullptr || (levelOk && !bestOk) || (levelOk == bestOk && c.modified > newest->modified)) {
				newest = &c;
			}
		}
		{
			std::lock_guard guard(g.lock);
			g.session = static_cast<long long>((WallMs() << 8) ^ (NowMs() & 0xFF)) & ((1LL << 52) - 1);
			g.saveTimes.clear();
			for (const auto& c : saves) {
				g.saveTimes[c.path.wstring()] = c.modified;
			}
		}
		if (newest != nullptr) {
			std::lock_guard guard(g.lock);
			g.charSavePath = newest->path;
			g.charSaveTime = {};
			g.characterDirty = true;
			g.presenceDirty  = true;
		}
		if (newest != nullptr) {
			name     = newest->name;
			hardcore = newest->hardcore;
			level    = newest->level;
			std::lock_guard guard(g.lock);
			g.charClass = newest->cls;
		}
	}
	{
		std::lock_guard guard(g.lock);
		g.charName = name;
		g.hardcore = hardcore;
		if (g.clvl <= 0) {
			g.clvl = level;
		}
		level = g.clvl;
	}

	std::string ev = "{";
	JsonStr(ev, "type", "baseline");
	JsonStr(ev, "charName", name);
	JsonInt(ev, "clvl", level);
	JsonBool(ev, "hardcore", hardcore);
	JsonInt(ev, "ts", static_cast<long long>(WallMs()));
	RaceFields(ev, "ready");
	JsonKey(ev, "items") += '[';
	bool first = true;
	for (const auto& item : items) {
		const auto    rw  = runewords.find(ItemKey(item));
		const int32_t row = rw != runewords.end() ? rw->second : -1;
		// The clean-start check needs every shared stash item; otherwise only grail candidates.
		if (!IsCandidate(item) && row < 0 && item.container != D2RL::Items::ItemContainer::SharedStash) {
			continue;
		}
		if (!first) {
			ev += ',';
		}
		first = false;
		ItemJson(ev, item, false, row);
	}
	ev += ']';
	JsonKey(ev, "saveScan") += "{\"characters\":[";
	for (size_t i = 0; i < saves.size(); ++i) {
		if (i != 0) {
			ev += ',';
		}
		ev += '{';
		JsonStr(ev, "name", saves[i].name);
		JsonInt(ev, "level", saves[i].level);
		JsonBool(ev, "hardcore", saves[i].hardcore);
		ev += '}';
	}
	ev += "]}}";
	QueueEvent(std::move(ev));
	Log("GrailTrackrr: baseline sent for %s (level %d, %zu items, %zu save files).", name.c_str(), level, items.size(), saves.size());
}

static void SendFind(const D2RL::Items::ItemInfo& item, int32_t runeword) {
	std::string charName, panel;
	int32_t     clvl, areaId, difficulty;
	LastKill    kill;
	{
		std::lock_guard guard(g.lock);
		charName   = g.charName;
		panel      = g.openPanel;
		clvl       = g.clvl;
		areaId     = g.areaId;
		difficulty = g.difficulty;
		kill       = g.lastKill;
	}
	// Where the item came from: Cube output lands in the Cube; anything that
	// appears while a vendor or gamble window is open was bought.
	std::string source = "drop";
	{
		// Spawned with the test command (and still waiting to be picked up).
		std::lock_guard guard(g.lock);
		const auto      it = g.spawnedPending.find(SpawnSignature(CodeString(item.code), QualityName(item.quality), item.qualityRecordId));
		if (it != g.spawnedPending.end()) {
			if (NowMs() < it->second) {
				source = "test";
			}
			g.spawnedPending.erase(it);
		}
	}
	if (source == "test") {
	} else if (runeword >= 0) {
		source = "runeword";
	} else if (item.container == D2RL::Items::ItemContainer::Cube) {
		source = "cube";
	} else if (panel == "gamble") {
		source = "gamble";
	} else if (panel == "vendor") {
		source = "vendor";
	} else if (panel == "cube") {
		source = "cube";
	}

	std::string ev = "{";
	JsonStr(ev, "type", "find");
	JsonStr(ev, "charName", charName);
	JsonInt(ev, "ts", static_cast<long long>(WallMs()));
	JsonStr(ev, "source", source);
	JsonKey(ev, "item");
	ItemJson(ev, item, true, runeword);
	JsonKey(ev, "ctx") += '{';
	JsonInt(ev, "clvl", clvl);
	JsonInt(ev, "areaId", areaId);
	JsonInt(ev, "difficulty", difficulty);
	if (kill.atMs != 0) {
		JsonKey(ev, "lastKill") += '{';
		JsonInt(ev, "classId", kill.classId);
		JsonInt(ev, "levelId", kill.levelId);
		JsonInt(ev, "msAgo", static_cast<long long>(NowMs() - kill.atMs));
		ev += '}';
	}
	ev += "}}";
	QueueEvent(std::move(ev));
	if (g.config.debug) {
		Log("GrailTrackrr: new item %s seed=%u quality=%s row=%d runeword=%d ilvl=%u source=%s", CodeString(item.code).c_str(), ReportSeed(item),
			QualityName(item.quality), item.qualityRecordId, runeword, item.itemLevel, source.c_str());
	}
}

// ---------------------------------------------------------------------------
// Character view and presence

static auto Hex(const unsigned char* data, size_t size) -> std::string {
	static constexpr char Digits[] = "0123456789abcdef";
	std::string out;
	out.reserve(size * 2);
	for (size_t i = 0; i < size; ++i) {
		out += Digits[data[i] >> 4];
		out += Digits[data[i] & 0x0F];
	}
	return out;
}

// Full tooltip text, D2 color codes kept so the site can color each line.
static auto ItemTooltip(D2RL::ItemHandle handle) -> std::string {
	if (!g.config.itemText || !D2RL::HasItemServiceField(g.items, D2RL::ItemServiceTooltipFieldEnd) || g.items->getItemTooltip == nullptr) {
		return {};
	}
	std::string buffer(8192, '\0');
	uint32_t    required = 0;
	if (g.items->getItemTooltip(g.ctx, handle, buffer.data(), static_cast<uint32_t>(buffer.size()), &required) != D2RL::Items::Result::Success) {
		return {};
	}
	buffer.resize(std::strlen(buffer.c_str()));
	return buffer;
}

// The save stores class (0x28), level (0x2B), then a "gf" stats block and an
// "if" block with the 30 class skill levels. The server decodes the stats, so
// the raw bytes from "gf" through the skill levels are sent as hex.
struct SaveSnapshot {
	int         classId = -1;
	int         level   = 0;
	std::string statsHex;
	std::string fileBase64; // the whole save, decoded on the server (gear, charms, merc)
};

static auto Base64(const std::vector<unsigned char>& data) -> std::string {
	static constexpr char Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((data.size() + 2) / 3 * 4);
	for (size_t i = 0; i < data.size(); i += 3) {
		const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (i + 1 < data.size() ? static_cast<uint32_t>(data[i + 1]) << 8 : 0)
			| (i + 2 < data.size() ? data[i + 2] : 0);
		out += Table[(n >> 18) & 63];
		out += Table[(n >> 12) & 63];
		out += i + 1 < data.size() ? Table[(n >> 6) & 63] : '=';
		out += i + 2 < data.size() ? Table[n & 63] : '=';
	}
	return out;
}

static auto ReadSave(const fs::path& path) -> SaveSnapshot {
	SaveSnapshot snap;
	std::ifstream file(path, std::ios::binary);
	std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	if (data.size() < 0x30) {
		return snap;
	}
	if (data.size() <= 1'000'000) {
		snap.fileBase64 = Base64(data);
	}
	const HeaderOffsets at = OffsetsFor(data.data());
	snap.classId = data[at.cls];
	snap.level   = data[at.level];
	if (snap.level < 1 || snap.level > 99) {
		snap.level = LevelFromStats(data);
	}
	for (size_t i = 0x14F; i + 1 < data.size(); ++i) {
		if (data[i] != 'g' || data[i + 1] != 'f') {
			continue;
		}
		for (size_t j = i + 2; j + 1 < data.size() && j < i + 1024; ++j) {
			if (data[j] == 'i' && data[j + 1] == 'f' && j + 32 <= data.size()) {
				snap.statsHex = Hex(&data[i], j + 32 - i);
				return snap;
			}
		}
		break;
	}
	return snap;
}

static auto EquippedSignature(const std::vector<D2RL::Items::ItemInfo>& items) -> std::string {
	std::string sig;
	for (const auto& item : items) {
		if (item.container == D2RL::Items::ItemContainer::Equipment) {
			sig += std::to_string(ItemKey(item)) + ':' + std::to_string(item.bodyLocation) + ':' + std::to_string(item.socketedItemCount) + ';';
		}
	}
	return sig;
}

// Inventory, belt and Horadric Cube: what the character carries (charms in the inventory count toward stats).
static auto IsCarried(const D2RL::Items::ItemInfo& item) -> bool {
	return item.container == D2RL::Items::ItemContainer::Inventory || item.container == D2RL::Items::ItemContainer::Belt
	    || item.container == D2RL::Items::ItemContainer::Cube;
}

static auto CarriedContainer(const D2RL::Items::ItemInfo& item) -> const char* {
	switch (item.container) {
	case D2RL::Items::ItemContainer::Belt: return "belt";
	case D2RL::Items::ItemContainer::Cube: return "cube";
	default:                               return "inventory";
	}
}

static auto CarriedSignature(const std::vector<D2RL::Items::ItemInfo>& items) -> std::string {
	std::string sig;
	for (const auto& item : items) {
		if (IsCarried(item)) {
			sig += std::to_string(ItemKey(item)) + ':' + std::to_string(static_cast<uint32_t>(item.container)) + ':' + std::to_string(item.x) + ',' + std::to_string(item.y)
			     + ':' + std::to_string(item.socketedItemCount) + ':' + std::to_string(item.quantity) + ';';
		}
	}
	return sig;
}

// UI thread: where every carried item sits (inventory, belt, Cube). Grail items (and runewords) also carry their
// exact stats, once per game for each item; the server keeps them between snapshots.
static void SendItems(const std::vector<D2RL::Items::ItemInfo>& items) {
	std::string charName;
	int32_t     clvl;
	{
		std::lock_guard guard(g.lock);
		charName = g.charName;
		clvl     = g.clvl;
	}
	if (charName.empty()) {
		return;
	}
	std::string ev = "{";
	JsonStr(ev, "type", "items");
	JsonStr(ev, "charName", charName);
	JsonInt(ev, "clvl", clvl);
	JsonInt(ev, "ts", static_cast<long long>(WallMs()));
	JsonBool(ev, "cube", true); // this snapshot includes the Cube (older plugins left it out)
	JsonKey(ev, "items") += '[';
	bool                  first = true;
	std::vector<uint64_t> withStats;
	for (const auto& item : items) {
		if (!IsCarried(item)) {
			continue;
		}
		const int32_t rw = RunewordRow(item);
		if (!first) {
			ev += ',';
		}
		first = false;
		ev += '{';
		if (ReportSeed(item) != 0) {
			JsonInt(ev, "seed", ReportSeed(item));
		}
		JsonStr(ev, "code", CodeString(item.code));
		JsonStr(ev, "quality", QualityName(item.quality));
		JsonInt(ev, "row", item.qualityRecordId);
		if (rw >= 0) {
			JsonInt(ev, "rw", rw);
		}
		JsonBool(ev, "eth", (item.stateFlags & D2RL::Items::ItemStateEthereal) != 0);
		JsonStr(ev, "c", CarriedContainer(item));
		JsonInt(ev, "x", item.x);
		JsonInt(ev, "y", item.y);
		JsonInt(ev, "sk", static_cast<int32_t>(item.socketCount));
		JsonInt(ev, "qty", item.quantity);
		bool sent;
		{
			std::lock_guard guard(g.lock);
			sent = g.statsSent.contains(ItemKey(item));
		}
		if (!sent && (IsCandidate(item) || rw >= 0)) {
			ItemStatsJson(ev, item);
			withStats.push_back(ItemKey(item));
		}
		ev += '}';
	}
	ev += "]}";
	QueueEvent(std::move(ev));
	std::lock_guard guard(g.lock);
	for (const auto seed : withStats) {
		g.statsSent.insert(seed);
	}
	g.lastItemsSentMs = NowMs();
}

// UI thread: equipped items come from the live game, stats and skills from the save.
static void SendCharacter(const std::vector<D2RL::Items::ItemInfo>& items) {
	fs::path    path;
	std::string charName;
	int32_t     clvl;
	{
		std::lock_guard guard(g.lock);
		path     = g.charSavePath;
		charName = g.charName;
		clvl     = g.clvl;
	}
	if (charName.empty()) {
		return;
	}
	SaveSnapshot    save;
	std::error_code ec;
	long long       savedAt = 0;
	if (!path.empty()) {
		save = ReadSave(path);
		const auto modified = fs::last_write_time(path, ec);
		if (!ec) {
			const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(modified);
			savedAt        = std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
		}
	}

	std::string ev = "{";
	JsonStr(ev, "type", "character");
	JsonStr(ev, "charName", charName);
	JsonInt(ev, "clvl", clvl > 0 ? clvl : save.level);
	JsonInt(ev, "classId", save.classId);
	JsonStr(ev, "save", save.statsHex);
	JsonStr(ev, "saveB64", save.fileBase64);
	JsonInt(ev, "savedAt", savedAt);
	JsonKey(ev, "equipped") += '[';
	bool first = true;
	for (const auto& item : items) {
		if (item.container != D2RL::Items::ItemContainer::Equipment) {
			continue;
		}
		if (!first) {
			ev += ',';
		}
		first = false;
		ev += '{';
		JsonInt(ev, "loc", item.bodyLocation);
		JsonStr(ev, "code", CodeString(item.code));
		JsonStr(ev, "quality", QualityName(item.quality));
		JsonInt(ev, "row", item.qualityRecordId);
		JsonInt(ev, "runeword", RunewordRow(item));
		JsonBool(ev, "eth", (item.stateFlags & D2RL::Items::ItemStateEthereal) != 0);
		JsonInt(ev, "sockets", item.socketCount);
		JsonStr(ev, "tooltip", ItemTooltip(item.handle));
		ev += '}';
	}
	ev += "]}";
	QueueEvent(std::move(ev));
	std::lock_guard guard(g.lock);
	g.lastCharSentMs = NowMs();
}

static void QueuePresence(bool playing) {
	std::lock_guard guard(g.lock);
	if (g.charName.empty()) {
		return;
	}
	std::string ev = "{";
	JsonStr(ev, "type", "presence");
	JsonStr(ev, "charName", g.charName);
	JsonBool(ev, "playing", playing);
	JsonInt(ev, "clvl", g.clvl);
	JsonInt(ev, "areaId", g.areaId);
	JsonInt(ev, "difficulty", g.difficulty);
	// Racing: the shared stash and the game type, watched for the whole event.
	if (g.raceState == "running") {
		JsonKey(ev, "race") += '{';
		JsonInt(ev, "sharedItems", g.sharedItems);
		JsonInt(ev, "sharedGold", SharedStashGold(g.hardcore));
		JsonBool(ev, "multiplayer", g.multiplayer);
		ev += '}';
	}
	ev += '}';
	QueueEventLocked(std::move(ev));
	g.presenceDirty   = false;
	g.nextHeartbeatMs = NowMs() + 60'000;
}

// Only the loaded character's save is written while in game, so a save that changes
// and is not the guessed one means the guess was wrong: switch and tell the server.
static void WatchSaves() {
	std::wstring current;
	std::unordered_map<std::wstring, fs::file_time_type> known;
	{
		std::lock_guard guard(g.lock);
		// The game writes the save when leaving, so keep watching briefly after that.
		if (g.session == 0 || (!g.inGame && NowMs() > g.leftAtMs + 30'000)) {
			return;
		}
		current = g.charSavePath.wstring();
		known   = g.saveTimes;
	}
	std::error_code ec;
	fs::path        changed;
	for (const auto& entry : fs::directory_iterator(SaveFolder(), ec)) {
		if (!entry.is_regular_file(ec) || entry.path().extension() != L".d2s") {
			continue;
		}
		const auto modified = entry.last_write_time(ec);
		const auto it       = known.find(entry.path().wstring());
		if (it != known.end() && it->second == modified) {
			continue;
		}
		{
			std::lock_guard guard(g.lock);
			g.saveTimes[entry.path().wstring()] = modified;
		}
		// A save that did not exist at load is a new character being created elsewhere; ignore.
		if (it != known.end() && entry.path().wstring() != current) {
			changed = entry.path();
		}
	}
	if (changed.empty()) {
		return;
	}
	const std::string to = ToUtf8(changed.stem());
	std::lock_guard guard(g.lock);
	const std::string from = g.charName;
	if (from == to) {
		return;
	}
	Log("GrailTrackrr: the loaded character is %s, not %s; moving this session to %s.", to.c_str(), from.c_str(), to.c_str());
	g.charName       = to;
	g.charSavePath   = changed;
	g.charSaveTime   = {};
	g.characterDirty = true;
	g.presenceDirty  = true;
	std::string ev = "{";
	JsonStr(ev, "type", "identify");
	JsonStr(ev, "from", from);
	JsonStr(ev, "charName", to);
	JsonInt(ev, "clvl", g.clvl);
	JsonInt(ev, "ts", static_cast<long long>(WallMs()));
	ev += '}';
	QueueEventLocked(std::move(ev));
}

static void StartQueuedSpawns();

static void __cdecl FlushConsoleOnUi(const D2RL::PluginContext* context, void*) noexcept {
	FlushConsole(context);
}

// Runs on the game thread by default (scan_thread in the config), else the UI thread.
static void __cdecl ScanOnUiThread(const D2RL::PluginContext* context, void*) noexcept {
	g.scanQueued = false;
	if (!g.config.scanItems) {
		return;
	}
	try {
		bool baseline = false;
		{
			std::lock_guard guard(g.lock);
			if (!g.inGame) {
				return;
			}
			// Give the game a moment after the player is ready so every page is loaded.
			if (g.baselinePending) {
				if (NowMs() < g.readyAtMs + 2'000) {
					return;
				}
				baseline          = true;
				g.baselinePending = false;
			}
		}

		D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
		if (g.inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success) {
			return;
		}
		StartQueuedSpawns();
		const D2RL::Inventory::ItemFilter filter {
			.structSize    = sizeof(D2RL::Inventory::ItemFilter),
			.containerMask = D2RL::Items::AllItemContainers,
		};
		ScanResult scan;
		if (g.inventory->forEachInventoryItem(context, player, &filter, CollectItem, &scan) != D2RL::Inventory::Result::Success) {
			return;
		}
		// Races: the shared stash must stay empty, and the site may ask for a check now.
		std::string raceCheck;
		{
			std::lock_guard guard(g.lock);
			g.sharedItems = static_cast<uint32_t>(std::count_if(scan.items.begin(), scan.items.end(), [](const auto& i) { return i.container == D2RL::Items::ItemContainer::SharedStash; }));
			if (g.raceCheckPending && !baseline) {
				g.raceCheckPending = false;
				g.lastRaceCheckMs  = NowMs();
				raceCheck          = g.raceCheckPhase;
			}
		}
		if (!raceCheck.empty()) {
			SendRaceCheck(scan.items, raceCheck);
		}

		if (baseline) {
			std::unordered_map<uint64_t, int32_t> runewords;
			for (const auto& item : scan.items) {
				const int32_t row = RunewordRow(item);
				if (row >= 0) {
					runewords[ItemKey(item)] = row;
				}
			}
			{
				std::lock_guard guard(g.lock);
				for (const auto& item : scan.items) {
					g.knownSeeds.insert(ItemKey(item));
					g.socketChecked[ItemKey(item)] = item.socketedItemCount;
				}
				g.runeTotals = RuneTotals(scan.items);
				for (const auto& [seed, row] : runewords) {
					g.runewordSeeds.insert(seed);
				}
			}
			// Diagnostics: items the loader gave no seed or a shared seed (see ItemKey).
			{
				std::unordered_set<uint32_t> seeds;
				size_t                       zero = 0, shared = 0;
				for (const auto& item : scan.items) {
					if (item.itemSeed == 0) {
						zero++;
					} else if (!seeds.insert(item.itemSeed).second) {
						shared++;
					}
				}
				Log("GrailTrackrr: load scan found %zu items (%zu without a seed, %zu sharing a seed).", scan.items.size(), zero, shared);
			}
			SendBaseline(scan.items, runewords);
			{
				std::lock_guard guard(g.lock);
				g.equippedSignature = EquippedSignature(scan.items);
			}
			SendCharacter(scan.items);
			{
				std::lock_guard guard(g.lock);
				g.itemsSignature = CarriedSignature(scan.items);
				g.itemsDirty     = false;
			}
			SendItems(scan.items);
			QueuePresence(true);
			return;
		}

		// Pass 1 (locked): new seeds, and known items whose filled sockets changed. Runes are
		// found by their count going up instead (stacks, and seeds that are not stable).
		std::vector<D2RL::Items::ItemInfo> fresh;
		std::vector<D2RL::Items::ItemInfo> recheck;
		{
			std::lock_guard guard(g.lock);
			const auto totals = RuneTotals(scan.items);
			if (totals != g.runeTotals) {
				std::string changes;
				for (const auto& [code, count] : totals) {
					const auto before = g.runeTotals.contains(code) ? g.runeTotals.at(code) : 0;
					if (count > before) {
						changes += " " + code + " " + std::to_string(before) + "->" + std::to_string(count);
						// Report one rune item of that code (its seed is not used for runes).
						const auto it = std::find_if(scan.items.begin(), scan.items.end(), [&](const auto& i) { return CodeString(i.code) == code; });
						if (it != scan.items.end()) {
							fresh.push_back(*it);
						}
					}
				}
				if (!changes.empty()) {
					Log("GrailTrackrr: runes picked up:%s", changes.c_str());
				}
				g.runeTotals = totals;
			}
			for (const auto& item : scan.items) {
				const bool isNew   = g.knownSeeds.insert(ItemKey(item)).second;
				auto&      checked = g.socketChecked[ItemKey(item)];
				auto&      tries   = g.runewordTries[ItemKey(item)];
				if (isNew || checked != item.socketedItemCount) {
					tries = 0;
				}
				// The game can mark an item as a runeword a moment after the last rune goes in,
				// so a full white item is checked again on the next few scans, not just once.
				const bool full    = item.socketCount > 0 && item.socketedItemCount == item.socketCount;
				// After the first few scans keep looking now and then: a runeword the game marks
				// late was missed before (and only found later from the save).
				const bool sockets = full && (tries < 10 || tries % 30 == 0);
				if (full && tries < 100'000) {
					tries++;
				}
				checked            = item.socketedItemCount;
				if (isNew && IsCandidate(item) && !IsRuneCode(CodeString(item.code))) {
					fresh.push_back(item);
				} else if (sockets && !g.runewordSeeds.contains(ItemKey(item))) {
					recheck.push_back(item);
				}
			}
		}
		// Pass 2 (unlocked, calls the game): which rechecked items are now runewords.
		std::vector<std::pair<D2RL::Items::ItemInfo, int32_t>> made;
		for (const auto& item : recheck) {
			const int32_t row = RunewordRow(item);
			if (row >= 0) {
				made.emplace_back(item, row);
			}
		}
		if (!made.empty()) {
			std::lock_guard guard(g.lock);
			for (const auto& [item, row] : made) {
				g.runewordSeeds.insert(ItemKey(item));
			}
		}
		for (const auto& item : fresh) {
			SendFind(item, -1);
		}
		for (const auto& [item, row] : made) {
			SendFind(item, row);
		}

		// Character view: resend when gear changes or the game writes the save
		// (stats and skills only change on disk at save time). At most every 5 s.
		const uint64_t now     = NowMs();
		bool           sendChar = false;
		bool           sendInv  = false;
		bool           presence = false;
		{
			std::lock_guard guard(g.lock);
			const std::string sig = EquippedSignature(scan.items);
			if (sig != g.equippedSignature) {
				g.equippedSignature = sig;
				g.characterDirty    = true;
			}
			if (now >= g.nextCharCheckMs && !g.charSavePath.empty()) {
				g.nextCharCheckMs = now + 10'000;
				std::error_code ec;
				const auto      modified = fs::last_write_time(g.charSavePath, ec);
				if (!ec && modified != g.charSaveTime) {
					g.charSaveTime   = modified;
					g.characterDirty = true;
				}
			}
			if (g.characterDirty && now >= g.lastCharSentMs + 5'000) {
				g.characterDirty = false;
				sendChar         = true;
				// The site shows the inventory from the save when it is newer than the
				// last live snapshot, so send a fresh one alongside.
				g.itemsDirty = true;
			}
			// Inventory, belt and Cube: resend when anything moves, at most every 5 s.
			const std::string carried = CarriedSignature(scan.items);
			if (carried != g.itemsSignature) {
				g.itemsSignature = carried;
				g.itemsDirty     = true;
			}
			if (g.itemsDirty && now >= g.lastItemsSentMs + 5'000) {
				g.itemsDirty = false;
				sendInv      = true;
			}
			presence = g.presenceDirty || now >= g.nextHeartbeatMs;
		}
		if (sendChar) {
			SendCharacter(scan.items);
		}
		if (sendInv) {
			SendItems(scan.items);
		}
		if (presence) {
			QueuePresence(true);
		}
	} catch (...) {
	}
}

// ---------------------------------------------------------------------------
// Game events

// Town level ids for the five acts.
static auto IsTown(int32_t levelId) -> bool {
	return levelId == 1 || levelId == 40 || levelId == 75 || levelId == 103 || levelId == 109;
}

static void __cdecl OnGameplayEvent(const D2RL::PluginContext*, const D2RL::Lifecycle::GameplayEvent* event, void*) noexcept {
	if (!D2RL::Lifecycle::HasGameplayEventField(event, D2RL::Lifecycle::GameplayEventRequiredSize)) {
		return;
	}
	using K = D2RL::Lifecycle::GameplayEventKind;
	try {
		std::lock_guard guard(g.lock);
		switch (event->kind) {
			case K::GameJoined:
				g.sessionGen = event->sessionGeneration;
				// Work queued for the last game was discarded with it.
				g.scanQueued = false;
				break;
			case K::LocalPlayerReady:
				g.inGame          = true;
				g.baselinePending = true;
				g.killsThisGame   = 0;
				g.multiplayer     = false;
				g.localPlayerId   = event->playerId;
				g.raceCheckPending = g.raceNeedCheck;
				g.raceCheckPhase  = g.raceState == "running" ? "start" : "ready";
				g.readyAtMs       = NowMs();
				g.knownSeeds.clear();
				g.runeTotals.clear();
				g.runewordSeeds.clear();
				g.socketChecked.clear();
				g.runewordTries.clear();
				g.statsSent.clear();
				g.itemsSignature.clear();
				g.clvl       = 0;
				g.areaId     = -1;
				g.fieldAreaId = -1;
				g.difficulty = -1;
				g.lastKill   = {};
				g.openPanel.clear();
				break;
			case K::GameLeft:
				g.inGame     = false;
				g.leftAtMs   = NowMs();
				g.scanQueued = false;
				if (!g.charName.empty()) {
					std::string ev = "{";
					JsonStr(ev, "type", "presence");
					JsonStr(ev, "charName", g.charName);
					JsonBool(ev, "playing", false);
					ev += '}';
					QueueEventLocked(std::move(ev));
				}
				break;
			case K::LevelChanged:
				g.areaId        = event->currentValue;
				g.presenceDirty = true;
				if (!IsTown(event->currentValue)) {
					g.fieldAreaId = event->currentValue;
				}
				break;
			case K::PlayerResurrected:
				// Softcore: coming back to life after dying counts one death. It happens in
				// town, so report the last area outside town as where it happened.
				if (!g.charName.empty()) {
					std::string ev = "{";
					JsonStr(ev, "type", "death");
					JsonStr(ev, "charName", g.charName);
					JsonInt(ev, "clvl", g.clvl);
					JsonInt(ev, "areaId", g.fieldAreaId >= 0 ? g.fieldAreaId : g.areaId);
					JsonInt(ev, "difficulty", g.difficulty);
					JsonInt(ev, "ts", static_cast<long long>(WallMs()));
					ev += '}';
					QueueEventLocked(std::move(ev));
				}
				break;
			case K::PlayerLevelChanged:
				if (event->currentValue > 0) {
					g.clvl          = event->currentValue;
					g.presenceDirty = true;
					std::string ev = "{";
					JsonStr(ev, "type", "level");
					JsonStr(ev, "charName", g.charName);
					JsonInt(ev, "clvl", g.clvl);
					ev += '}';
					QueueEventLocked(std::move(ev));
				}
				break;
			case K::QuestCompleted:
				if (event->difficulty <= 2 && g.difficulty != static_cast<int32_t>(event->difficulty)) {
					g.difficulty    = static_cast<int32_t>(event->difficulty);
					g.presenceDirty = true;
				}
				// Quests completed, for races (and to learn the quest numbers).
				if (!g.charName.empty()) {
					std::string ev = "{";
					JsonStr(ev, "type", "quest");
					JsonStr(ev, "charName", g.charName);
					JsonInt(ev, "questId", static_cast<long long>(event->questRecordId));
					JsonInt(ev, "difficulty", static_cast<long long>(event->difficulty));
					JsonInt(ev, "clvl", g.clvl);
					JsonInt(ev, "ts", static_cast<long long>(WallMs()));
					ev += '}';
					QueueEventLocked(std::move(ev));
				}
				break;
			default:
				break;
		}
	} catch (...) {
	}
}

// Server thread: remember the latest kill for find context.
static void __cdecl OnMonsterDeath(const D2RL::PluginContext*, const D2RL::Lifecycle::MonsterDeathEvent* event, void*) noexcept {
	if (!D2RL::Lifecycle::HasMonsterDeathEventField(event, D2RL::Lifecycle::MonsterDeathEventRequiredSize)) {
		return;
	}
	std::lock_guard guard(g.lock);
	g.lastKill = LastKill { event->monster.classId, event->levelId, NowMs() };
	g.killsThisGame++;
	// Killed by another player (or their summons' owner): this is a multiplayer game.
	if (event->killer.type == D2RL::Lifecycle::UnitType::Player && g.localPlayerId != 0xFFFFFFFFU && event->killer.id != g.localPlayerId) {
		if (!g.multiplayer) {
			Log("GrailTrackrr: another player is in this game.");
		}
		g.multiplayer = true;
	}
	// Act bosses (Andariel, Duriel, Mephisto, Diablo, Baal): report right away for races.
	const uint32_t cls = event->monster.classId;
	if ((cls == 156 || cls == 211 || cls == 242 || cls == 243 || cls == 545) && !g.charName.empty()) {
		std::string ev = "{";
		JsonStr(ev, "type", "kill");
		JsonStr(ev, "charName", g.charName);
		JsonInt(ev, "classId", static_cast<long long>(cls));
		JsonInt(ev, "levelId", event->levelId);
		JsonInt(ev, "difficulty", static_cast<long long>(event->difficulty));
		JsonInt(ev, "clvl", g.clvl);
		JsonInt(ev, "ts", static_cast<long long>(WallMs()));
		ev += '}';
		QueueEventLocked(std::move(ev));
	}
	// The first kill of a game tells us the difficulty: let the site know right away.
	if (event->difficulty <= 2 && g.difficulty != static_cast<int32_t>(event->difficulty)) {
		g.difficulty    = static_cast<int32_t>(event->difficulty);
		g.presenceDirty = true;
	}
}

// UI messages tell us which panel is open. The exact target names can change
// between game versions, so match loosely and log them in debug mode.
static auto Lower(const char* s) -> std::string {
	std::string out = s != nullptr ? s : "";
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return out;
}

static auto __cdecl OnUiMessage(const D2RL::PluginContext*, const D2RL::SharedEvents::UiMessageEvent* event, void*) noexcept -> D2RL::SharedEvents::UiMessageAction {
	if (event == nullptr || event->structSize < D2RL::SharedEvents::UiMessageEventRequiredSize) {
		return D2RL::SharedEvents::UiMessageAction::Continue;
	}
	try {
		const std::string target  = Lower(event->target);
		const std::string command = Lower(event->command);
		if (g.config.debug) {
			Log("GrailTrackrr ui: target=%s command=%s", target.c_str(), command.c_str());
		}
		const bool opening = command.find("open") != std::string::npos || command.find("show") != std::string::npos;
		const bool closing = command.find("close") != std::string::npos || command.find("hide") != std::string::npos;
		std::string panel;
		if (target.find("gambl") != std::string::npos) {
			panel = "gamble";
		} else if (target.find("vendor") != std::string::npos || target.find("shop") != std::string::npos || target.find("npctrade") != std::string::npos) {
			panel = "vendor";
		} else if (target.find("cube") != std::string::npos) {
			panel = "cube";
		}
		if (!panel.empty() && (opening || closing)) {
			std::lock_guard guard(g.lock);
			if (opening) {
				g.openPanel = panel;
			} else if (g.openPanel == panel) {
				g.openPanel.clear();
			}
		}
	} catch (...) {
	}
	return D2RL::SharedEvents::UiMessageAction::Continue;
}

// ---------------------------------------------------------------------------
// In-game display, drawn by the game's own UI as small native D2R panels.
//   Status line (bottom left, show_status) and the "New grail item" card (popup_seconds).
// D2RLoader 1.3.1 cannot change a widget's text, and the game re-shows hidden widgets
// whenever it refreshes a panel. So every piece of text is its own tiny panel with fixed
// text, and showing something means opening its panel; nothing is ever hidden inside a
// panel. A popup is a card panel (background, colour bar, heading) plus panels for the
// percentage digits and the rest of the line. That is 50 panels, under the loader's 64.
// When a loader with setText ships, this can shrink to one panel with the item's name.
// Every panel call runs on the UI thread (UiTick, queued by the worker).

// Card kinds: heading and colour bar.
struct PopupKind {
	const char* cat;
	const char* heading;
	const char* color;
};
static constexpr PopupKind PopupKinds[] {
	{ "unique",   "New grail unique",     "[0.78, 0.70, 0.47, 1.0]" },
	{ "set",      "New grail set item",   "[0.31, 0.78, 0.36, 1.0]" },
	{ "rune",     "New grail rune",       "[0.94, 0.64, 0.23, 1.0]" },
	{ "runeword", "New grail runeword",   "[0.78, 0.70, 0.47, 1.0]" },
	{ "eth",      "New ethereal find",    "[0.67, 0.71, 0.75, 1.0]" },
	{ "sync",     "Offline finds synced", "[0.60, 0.56, 0.48, 1.0]" },
	{ "badge",    "Badge earned!",        "[0.64, 0.45, 0.90, 1.0]" },
	{ "ready",    "Race check passed: you are ready", "[0.27, 0.79, 0.42, 1.0]" },
	{ "notready", "Race check failed: see the race page", "[1.00, 0.45, 0.35, 1.0]" },
	{ "go",       "GO! The race is on",   "[0.91, 0.72, 0.19, 1.0]" },
	{ "finish",   "Race finished!",       "[0.91, 0.72, 0.19, 1.0]" },
	{ "msg",      "GrailTrackrr",         "[0.60, 0.56, 0.48, 1.0]" },
};

// Status line states. The state comes from the status line the connection code sets.
struct StatusKind {
	const char* id;
	const char* text;
};
static constexpr StatusKind StatusKinds[] {
	{ "connected",  "GrailTrackrr: connected" },
	{ "notstarted", "GrailTrackrr: grail not started, see My grail on the site" },
	{ "offline",    "GrailTrackrr: offline, finds are saved and sent later" },
	{ "badkey",     "GrailTrackrr: API key rejected, copy it again from My grail" },
	{ "nokey",      "GrailTrackrr: set api_key in grailtrackrr.toml" },
	{ "error",      "GrailTrackrr: problem, type grailtrackrr in the console" },
};

// Race line above the status line, by race state.
struct RaceKind {
	const char* id;
	const char* text;
};
static constexpr RaceKind RaceKinds[] {
	{ "check",   "Race: join a game with a new level 1 character to get checked" },
	{ "ready",   "Race: READY. Wait in town for the start" },
	{ "waiting", "Race: the challenge opens soon" },
	{ "running", "Race: on! Good luck" },
	{ "done",    "Race: finished. Well played" },
};
// Countdown digits (M:SS, up to 59:59) after "Race starts in".
static constexpr int CdX = 28, CdY = -122, CdLabel = 300, CdDigit = 20;

static auto StatusKindFor(const std::string& line) -> const char* {
	if (line.rfind("GrailTrackrr connected", 0) == 0) return "connected";
	if (line.rfind("GrailTrackrr not started", 0) == 0) return "notstarted";
	if (line.find("cannot reach") != std::string::npos) return "offline";
	if (line.find("API key rejected") != std::string::npos) return "badkey";
	if (line.find("set api_key") != std::string::npos) return "nokey";
	return "error";
}

static auto JsonString(const std::string& s) -> std::string {
	std::string out = "\"";
	for (const char ch : s) {
		if (ch == '"' || ch == '\\') {
			out += '\\';
		}
		out += ch;
	}
	return out + '"';
}

// Inline text colours render black, so all text uses the game's white; the colour bar
// carries the item's colour.
static auto TextStyle(const char* face, const char* size, const char* align) -> std::string {
	return std::string(R"({"fontFace":")") + face + R"(","fontColor":"$FontColorWhite","pointSize":")" + size + R"(","alignment":{"h":")" + align
		+ R"(","v":"center"},"dropShadow":"$DefaultDropShadow"})";
}

static auto Rect(int x, int y, int w, int h) -> std::string {
	return R"({"x":)" + std::to_string(x) + R"(,"y":)" + std::to_string(y) + R"(,"width":)" + std::to_string(w) + R"(,"height":)" + std::to_string(h) + "}";
}

static auto Label(const std::string& name, const std::string& rect, const std::string& text, const std::string& style) -> std::string {
	return R"({"type":"TextBoxWidget","name":)" + JsonString(name) + R"(,"fields":{"rect":)" + rect + R"(,"text":)" + JsonString(text) + R"(,"style":)" + style + "}}";
}

// One panel: anchor, priority and its widgets.
static auto PanelJson(const std::string& localId, float anchorX, float anchorY, int priority, const std::string& children) -> std::string {
	char anchor[64];
	std::snprintf(anchor, sizeof(anchor), R"({"x":%.1f,"y":%.1f})", anchorX, anchorY);
	return R"({"type":"Panel","name":)" + JsonString(std::string("grailtrackrr/") + localId) + R"(,"fields":{"anchor":)" + anchor + R"(,"priority":)"
		+ std::to_string(priority) + R"(},"children":[)" + children + "]}";
}

// Popup card geometry, relative to the anchor (top or middle of the screen, centred).
static constexpr int CardWidth = 720, CardHeight = 112, CardX = -CardWidth / 2;
// Width of one digit and of "% of your grail found" in layout units, at $MediumFontSize.
static constexpr int PctDigit = 20, PctRest = 340, PctY = 60;

struct PanelDef {
	std::string id;     // local id, also the layout file name
	std::string json;
	D2RL::Panels::RegistrationHandle handle = D2RL::Panels::InvalidHandle;
	bool        open = false;   // opened by us and not closed since
};

// All panels, built at load: Status_<kind>, Card_<kind>, Pct<len>_<pos>_<digit>, PctText<len>.
static auto BuildPanels(bool center) -> std::vector<PanelDef> {
	std::vector<PanelDef> panels;
	const float anchorY = center ? 0.5F : 0.0F;
	const int   cardY   = center ? -200 : 140;
	for (const auto& k : StatusKinds) {
		panels.push_back({ std::string("Status_") + k.id,
			PanelJson(std::string("Status_") + k.id, 0.0F, 1.0F, 8400,
				Label("Text", Rect(28, -66, 1500, 52), k.text, TextStyle("BlizzardGlobal", "$MediumFontSize", "left"))) });
	}
	for (const auto& k : RaceKinds) {
		panels.push_back({ std::string("Race_") + k.id,
			PanelJson(std::string("Race_") + k.id, 0.0F, 1.0F, 8400,
				Label("Text", Rect(28, -122, 1500, 52), k.text, TextStyle("BlizzardGlobal", "$MediumFontSize", "left"))) });
	}
	panels.push_back({ "CdText", PanelJson("CdText", 0.0F, 1.0F, 8410, Label("Text", Rect(CdX, CdY - 56, CdLabel, 52), "Race starts in", TextStyle("BlizzardGlobal", "$MediumFontSize", "left"))) });
	panels.push_back({ "CdColon", PanelJson("CdColon", 0.0F, 1.0F, 8410, Label("Text", Rect(CdX + CdLabel + 2 * CdDigit, CdY - 56, CdDigit / 2 + 4, 52), ":", TextStyle("BlizzardGlobal", "$MediumFontSize", "center"))) });
	for (int pos = 0; pos < 4; ++pos) {
		const int x = CdX + CdLabel + pos * CdDigit + (pos >= 2 ? CdDigit / 2 + 4 : 0);
		for (int d = 0; d <= 9; ++d) {
			const std::string id = "Cd_" + std::to_string(pos) + "_" + std::to_string(d);
			panels.push_back({ id, PanelJson(id, 0.0F, 1.0F, 8410, Label("Digit", Rect(x, CdY - 56, CdDigit, 52), std::to_string(d), TextStyle("BlizzardGlobal", "$MediumFontSize", "center"))) });
		}
	}
	for (const auto& k : PopupKinds) {
		std::string children = R"({"type":"RectangleWidget","name":"Back","fields":{"rect":)" + Rect(CardX, cardY, CardWidth, CardHeight)
			+ R"(,"color":[0.05, 0.04, 0.03, 0.88]}})";
		for (const int y : { 0, CardHeight - 3 }) {
			children += R"(,{"type":"RectangleWidget","name":"Bar)" + std::to_string(y) + R"(","fields":{"rect":)" + Rect(CardX, cardY + y, CardWidth, 3)
				+ R"(,"color":)" + k.color + "}}";
		}
		children += "," + Label("Heading", Rect(CardX + 16, cardY + 10, CardWidth - 32, 50), k.heading, TextStyle("Exocet", "$MediumFontSize", "center"));
		panels.push_back({ std::string("Card_") + k.cat, PanelJson(std::string("Card_") + k.cat, 0.5F, anchorY, 8500, children) });
	}
	// "NN% of your grail found", centred: per length (1, 2 or 3 digits) a panel per digit
	// and position, then the rest of the line. 100 needs only "1 0 0".
	for (int len = 1; len <= 3; ++len) {
		const int start = CardX + (CardWidth - len * PctDigit - PctRest) / 2;
		for (int pos = 0; pos < len; ++pos) {
			const int from = len == 3 ? (pos == 0 ? 1 : 0) : 0, to = len == 3 ? from : 9;
			for (int d = from; d <= to; ++d) {
				const std::string id = "Pct" + std::to_string(len) + "_" + std::to_string(pos) + "_" + std::to_string(d);
				panels.push_back({ id, PanelJson(id, 0.5F, anchorY, 8510,
					Label("Digit", Rect(start + pos * PctDigit, cardY + PctY, PctDigit, 40), std::to_string(d), TextStyle("BlizzardGlobal", "$MediumFontSize", "center"))) });
			}
		}
		const std::string id = "PctText" + std::to_string(len);
		panels.push_back({ id, PanelJson(id, 0.5F, anchorY, 8510,
			Label("Text", Rect(start + len * PctDigit, cardY + PctY, PctRest + 40, 40), "% of your grail found", TextStyle("BlizzardGlobal", "$MediumFontSize", "left"))) });
	}
	return panels;
}

struct PanelUi {
	std::vector<PanelDef>  panels;
	bool                   ready = false;
	// UI thread only from here.
	std::vector<std::string> popupShown;   // popup panels that should be open now
	bool                   popupShowing = false;
	uint64_t               popupUntil   = 0;
	std::atomic<bool>      tickQueued { false };
	std::atomic<uint32_t>  lastError { 0 };   // last failing call, for the status command
	std::string            lastErrorWhat;
};
static PanelUi ui;

static std::string panelSetup = "not started";   // why the panels are off, for the status command

// Plugin load. Without the panel service the plugin still tracks; it just shows nothing in game.
static void SetUpPanels(const D2RL::PluginContext* context) {
	const D2RL::ResourceService* resources = nullptr;
	auto fail = [&](const std::string& why) {
		g.panels   = nullptr;
		panelSetup = why;
		Log("GrailTrackrr: in-game panels are off: %s", why.c_str());
	};
	const auto rq = context->QueryService(&resources);
	if (rq != D2RL::ServiceQueryResult::Success || resources == nullptr) {
		return fail("resource service query " + std::to_string(static_cast<int>(rq)));
	}
	if (!D2RL::HasResourceServiceField(resources, D2RL::ResourceServiceRequiredSize)) {
		return fail("resource service version " + std::to_string(resources->serviceVersion) + " size " + std::to_string(resources->serviceSize));
	}
	const auto pq = context->QueryService(&g.panels);
	if (pq != D2RL::ServiceQueryResult::Success || g.panels == nullptr) {
		return fail("panel service query " + std::to_string(static_cast<int>(pq)));
	}
	if (!D2RL::HasPanelServiceField(g.panels, D2RL::PanelServiceGetPanelInfoFieldEnd) || g.panels->openPanel == nullptr || g.panels->closePanel == nullptr) {
		return fail("panel service version " + std::to_string(g.panels->serviceVersion) + " size " + std::to_string(g.panels->serviceSize));
	}
	ui.panels = BuildPanels(g.config.popupCenter);
	for (auto& p : ui.panels) {
		const std::string path = "data/global/ui/layouts/grailtrackrr/" + p.id + "hd.json";
		const D2RL::Resources::ResourceRegistration resource {
			.structSize = D2RL::Resources::ResourceRegistrationSize,
			.path       = path.c_str(),
			.bytes      = p.json.data(),
			.byteCount  = p.json.size(),
		};
		D2RL::Resources::RegistrationHandle resourceHandle = D2RL::Resources::InvalidHandle;
		if (const auto r = resources->registerResource(context, &resource, &resourceHandle); r != D2RL::Resources::Result::Success) {
			return fail("registering " + path + " returned " + std::to_string(static_cast<uint32_t>(r)));
		}
		const D2RL::Panels::PanelRegistration reg { .structSize = D2RL::Panels::PanelRegistrationSize, .flags = D2RL::Panels::PanelFlags::None, .localId = p.id.c_str() };
		if (const auto r = g.panels->registerPanel(context, &reg, &p.handle); r != D2RL::Panels::Result::Success) {
			return fail("registering panel " + p.id + " returned " + std::to_string(static_cast<uint32_t>(r)));
		}
	}
	panelSetup = "ok";
	ui.ready   = true;
}

static void NoteUiError(const std::string& what, uint32_t code) {
	if (code != 0 && (ui.lastError != code || ui.lastErrorWhat != what)) {
		ui.lastError     = code;
		ui.lastErrorWhat = what;
		Log("GrailTrackrr: %s failed (D2RLoader result %u).", what.c_str(), code);
	}
}

static auto PanelOpen(const D2RL::PluginContext* context, D2RL::Panels::RegistrationHandle handle) -> bool {
	D2RL::Panels::PanelInfo info { .structSize = D2RL::Panels::PanelInfoSize };
	return g.panels->getPanelInfo(context, handle, &info) == D2RL::Panels::Result::Success && info.presentationState == D2RL::Panels::PresentationState::Open;
}

// Opens the wanted panels (again, if the game closed one) and closes the rest that are open.
static void ShowPanels(const D2RL::PluginContext* context, const std::vector<std::string>& wanted) {
	for (auto& p : ui.panels) {
		const bool want = std::find(wanted.begin(), wanted.end(), p.id) != wanted.end();
		if (want && !(p.open && PanelOpen(context, p.handle))) {
			const auto r = g.panels->openPanel(context, p.handle);
			NoteUiError("opening " + p.id, static_cast<uint32_t>(r));
			p.open = r == D2RL::Panels::Result::Success;
		} else if (!want && p.open) {
			NoteUiError("closing " + p.id, static_cast<uint32_t>(g.panels->closePanel(context, p.handle)));
			p.open = false;
		}
	}
}

// The panels for one find: its card, then the percentage.
static auto PopupPanels(const Popup& p) -> std::vector<std::string> {
	// "Name (Ethereal)" is the ethereal copy of a catalog item.
	std::string name = p.name;
	bool        eth  = p.category == "eth";
	if (constexpr std::string_view suffix = " (Ethereal)"; name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
		name.resize(name.size() - suffix.size());
		eth = true;
	}
	const bool known = std::any_of(std::begin(PopupKinds), std::end(PopupKinds), [&](const PopupKind& k) { return p.category == k.cat; });
	std::vector<std::string> shown { std::string("Card_") + (eth ? "eth" : known ? p.category : "msg") };
	// The name and progress go to the console (the card cannot show changing text).
	if (p.category == "badge") {
		Say("GrailTrackrr: badge earned: %s%s", name.c_str(), p.detail.empty() ? "" : (" (" + p.detail + ")").c_str());
		return shown;   // a badge's detail is its description, not progress
	}
	Say("GrailTrackrr: %s%s%s", name.c_str(), eth ? " (Ethereal)" : "", p.detail.empty() ? "" : (" - " + p.detail).c_str());
	// Progress: "123 / 403 found, 30.5%". Whole percent, rounded down (100 only when complete).
	if (const auto pctAt = p.detail.rfind('%'); pctAt != std::string::npos) {
		size_t start = pctAt;
		while (start > 0 && (std::isdigit(static_cast<unsigned char>(p.detail[start - 1])) || p.detail[start - 1] == '.')) {
			--start;
		}
		if (start < pctAt) {
			const int         pct    = std::clamp(static_cast<int>(std::atof(p.detail.substr(start, pctAt - start).c_str())), 0, 100);
			const std::string digits = std::to_string(pct);
			const auto        len    = std::to_string(digits.size());
			for (size_t pos = 0; pos < digits.size(); ++pos) {
				shown.push_back("Pct" + len + "_" + std::to_string(pos) + "_" + digits[pos]);
			}
			shown.push_back("PctText" + len);
		}
	}
	return shown;
}

// UI thread, four times a second.
static void __cdecl UiTick(const D2RL::PluginContext* context, void*) noexcept {
	ui.tickQueued = false;
	try {
		if (!ui.ready) {
			return;
		}
		const uint64_t now = NowMs();
		bool           inGame = false;
		std::string    status;
		std::string    race;
		long long      toStart = 0;
		std::optional<Popup> next;
		{
			std::lock_guard guard(g.lock);
			inGame = g.inGame;
			status = g.statusLine;
			race   = g.raceState;
			toStart = g.raceStartKnown ? static_cast<long long>(g.raceStartMs) - static_cast<long long>(now) : 0;
			if (g.config.popupSeconds <= 0.0F) {
				g.popups.clear();
			} else if (inGame && !ui.popupShowing && !g.popups.empty() && now >= g.popups.front().notBeforeMs) {
				next = g.popups.front();
				g.popups.pop_front();
			}
		}
		if (!inGame) {
			ui.popupShowing = false;
			ui.popupShown.clear();
			ShowPanels(context, {});
			return;
		}
		if (ui.popupShowing && now >= ui.popupUntil) {
			ui.popupShowing = false;
			ui.popupShown.clear();
		}
		if (next) {
			ui.popupShown   = PopupPanels(*next);
			ui.popupShowing = true;
			ui.popupUntil   = now + static_cast<uint64_t>(g.config.popupSeconds * 1000.0F);
		}
		std::vector<std::string> wanted = ui.popupShown;
		if (g.config.showStatus) {
			wanted.push_back(std::string("Status_") + StatusKindFor(status));
		}
		// Races: the state line, and a countdown in the last hour before the start.
		if (std::any_of(std::begin(RaceKinds), std::end(RaceKinds), [&](const RaceKind& k) { return race == k.id; })) {
			wanted.push_back("Race_" + race);
			if ((race == "ready" || race == "check" || race == "waiting") && toStart > 0 && toStart < 3'600'000) {
				const long long secs = (toStart + 999) / 1000;
				const int       m = static_cast<int>(secs / 60), sec = static_cast<int>(secs % 60);
				const int       digits[4] = { m / 10, m % 10, sec / 10, sec % 10 };
				wanted.push_back("CdText");
				wanted.push_back("CdColon");
				for (int pos = 0; pos < 4; ++pos) {
					wanted.push_back("Cd_" + std::to_string(pos) + "_" + std::to_string(digits[pos]));
				}
			}
		}
		ShowPanels(context, wanted);
	} catch (...) {
	}
}

// Worker thread.
// Runs out of game too, so panels left open by a failed close still get closed.
static void QueueUiTick() {
	if (ui.ready && !ui.tickQueued.exchange(true)) {
		if (g.threads->runOnUiThread(g.ctx, UiTick, nullptr) != D2RL::Threads::Result::Success) {
			ui.tickQueued = false;
		}
	}
}

// ---------------------------------------------------------------------------
// Offline queue: unsent events are kept in %LOCALAPPDATA%\GrailTrackrr\outbox.jsonl,
// one JSON event per line after a header line naming the API key they belong to.
// Presence ("playing now") is left out because it is stale by the next session.

static auto OutboxPath() -> fs::path {
	PWSTR raw = nullptr;
	fs::path dir;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw != nullptr) {
		dir = fs::path(raw) / L"GrailTrackrr";
	}
	CoTaskMemFree(raw);
	return dir.empty() ? fs::path() : dir / L"outbox.jsonl";
}

static std::vector<std::string> savedOutbox;

// Worker thread. Writes the queue only when it changed.
static void SaveOutbox() {
	std::vector<std::string> snapshot;
	{
		std::lock_guard guard(g.lock);
		for (const auto& e : g.outbox) {
			if (!IsType(e, "presence")) {
				snapshot.push_back(e);
			}
		}
	}
	if (snapshot == savedOutbox) {
		return;
	}
	const fs::path path = OutboxPath();
	if (path.empty()) {
		return;
	}
	std::error_code ec;
	if (snapshot.empty()) {
		fs::remove(path, ec);
		savedOutbox.clear();
		return;
	}
	fs::create_directories(path.parent_path(), ec);
	const fs::path tmp = path.wstring() + L".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out) {
			return;
		}
		out << "key " << g.config.apiKey << '\n';
		for (const auto& e : snapshot) {
			out << e << '\n';
		}
		if (!out) {
			return;
		}
	}
	fs::rename(tmp, path, ec);
	if (!ec) {
		savedOutbox = std::move(snapshot);
	}
}

// Plugin load: puts events from an earlier offline session in front of the queue.
static void LoadOutbox() {
	const fs::path path = OutboxPath();
	std::error_code ec;
	// Unsent finds saved by the plugin before it was renamed (LiveGrail folder).
	if (!path.empty() && !fs::exists(path, ec)) {
		const fs::path old = path.parent_path().parent_path() / L"LiveGrail" / L"outbox.jsonl";
		if (fs::exists(old, ec)) {
			fs::create_directories(path.parent_path(), ec);
			fs::rename(old, path, ec);
		}
	}
	if (path.empty() || !fs::exists(path, ec)) {
		return;
	}
	std::ifstream in(path, std::ios::binary);
	std::string   header;
	std::getline(in, header);
	if (header != "key " + g.config.apiKey) {
		// Saved for a different account; keep a copy instead of sending it to this one.
		in.close();
		fs::rename(path, path.wstring() + L".other-key", ec);
		Log("GrailTrackrr: saved offline events belong to another API key; moved them aside.");
		return;
	}
	std::vector<std::string> events;
	for (std::string line; std::getline(in, line);) {
		if (!line.empty() && line.front() == '{' && line.back() == '}') {
			events.push_back(line);
		}
	}
	std::lock_guard guard(g.lock);
	g.outbox.insert(g.outbox.begin(), events.begin(), events.end());
	if (g.outbox.size() > OutboxLimit) {
		g.outbox.resize(OutboxLimit);
	}
	savedOutbox = events;
	const auto finds = std::count_if(events.begin(), events.end(), [](const std::string& e) { return IsType(e, "find"); });
	Log("GrailTrackrr: %zu saved event(s) from an earlier session to send (%zu find(s)).", events.size(), static_cast<size_t>(finds));
}

// ---------------------------------------------------------------------------
// Worker thread: drives sending, the connection check and the scan timer on
// its own, so nothing depends on the game drawing frames.

static void TrySpawnLookup();

static std::thread       worker;
static std::atomic<bool> workerStop { false };

static void WorkerLoop() {
	uint64_t nextSaveWatchMs = 0;
	while (!workerStop) {
		try {
			const uint64_t now    = NowMs();
			bool           inGame = false;
			{
				std::lock_guard guard(g.lock);
				inGame = g.inGame;
			}
			// A scan queued just before a game ended was thrown away with that game: try again.
			if (g.scanQueued && now > g.scanQueuedAtMs + 5'000) {
				Log("GrailTrackrr: an item scan never ran (the game changed); scanning again.");
				g.scanQueued = false;
			}
			// Races: check again now and then while waiting, and once at the start.
			{
				std::lock_guard guard(g.lock);
				if (inGame && g.raceNeedCheck && !g.raceCheckPending) {
					const uint64_t every = g.raceState == "check" ? 15'000 : 60'000;
					const bool     atStart = g.raceStartKnown && g.raceState == "ready" && now >= g.raceStartMs && g.raceCheckPhase != "start";
					if (atStart) {
						g.raceCheckPhase   = "start";
						g.raceCheckPending = true;
					} else if (now >= g.lastRaceCheckMs + every) {
						g.raceCheckPending = true;
					}
				}
				// The start: a GO card, even before the server says so.
				if (inGame && g.raceStartKnown && (g.raceState == "ready" || g.raceState == "running") && now >= g.raceStartMs && now < g.raceStartMs + 30'000 && !g.raceGoShown) {
					g.raceGoShown = true;
					g.popups.push_back(Popup { "go", "GO!", g.raceTitle });
				}
			}
			if (inGame && now >= g.nextScanMs && !g.scanQueued.exchange(true)) {
				g.scanQueuedAtMs = now;
				g.nextScanMs = now + 1'000;
				const auto queued = g.config.scanOnGame && g.threads->runOnGameThread != nullptr ? g.threads->runOnGameThread(g.ctx, ScanOnUiThread, nullptr)
				                                                                                : g.threads->runOnUiThread(g.ctx, ScanOnUiThread, nullptr);
				if (queued != D2RL::Threads::Result::Success) {
					g.scanQueued = false;
				}
			}
			{
				bool pending = false;
				{
					std::lock_guard guard(consoleLock);
					pending = !consoleQueue.empty();
				}
				if (pending) {
					(void)g.threads->runOnUiThread(g.ctx, FlushConsoleOnUi, nullptr);
				}
			}
			QueueUiTick();
			TrySend();
			TryPing();
			TrySpawnLookup();
			SaveOutbox();
			if (now >= nextSaveWatchMs) {
				nextSaveWatchMs = now + 5'000;
				WatchSaves();
			}
		} catch (...) {
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
}

// ---------------------------------------------------------------------------
// Test mode: "grailtrackrr spawn <what>" on a test account. The server turns what was
// typed into items (worker thread), the local player is captured on the UI thread, and
// the items are created at the player's feet on the game thread. Picking them up runs
// the normal find path; those finds are reported with source "test".

static auto UrlEncode(const std::string& s) -> std::string {
	static const char* hex = "0123456789ABCDEF";
	std::string        out;
	for (const unsigned char c : s) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out += static_cast<char>(c);
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

static auto PackCode(const std::string& code) -> uint32_t {
	uint32_t v = 0;
	for (int i = 0; i < 4; ++i) {
		const char c = i < static_cast<int>(code.size()) ? code[static_cast<size_t>(i)] : ' ';
		v |= static_cast<uint32_t>(static_cast<unsigned char>(c)) << (8 * i);
	}
	return v;
}

static void OnSpawnReply(const HttpResult& r, void*) {
	try {
		std::vector<std::string> messages;
		{
			std::lock_guard guard(g.lock);
			g.spawnLookup = false;
		}
		// Spawn messages go to the console only; the popup is for grail finds.
		if (!r.reached) {
			Say("GrailTrackrr test: cannot reach the server (%s)", r.error.c_str());
			return;
		}
		int spawns = 0;
		size_t start = 0;
		while (start < r.body.size()) {
			size_t end = r.body.find('\n', start);
			if (end == std::string::npos) {
				end = r.body.size();
			}
			const std::string line = r.body.substr(start, end - start);
			start                  = end + 1;
			std::vector<std::string> parts;
			size_t                   p = 0;
			while (true) {
				const size_t tab = line.find('\t', p);
				parts.push_back(line.substr(p, tab == std::string::npos ? std::string::npos : tab - p));
				if (tab == std::string::npos) {
					break;
				}
				p = tab + 1;
			}
			if (parts[0] == "MSG" && parts.size() >= 2) {
				messages.push_back(parts[1]);
			} else if (parts[0] == "SPAWN" && parts.size() >= 6) {
				std::lock_guard guard(g.lock);
				g.spawnQueue.push_back({ parts[1], parts[2], std::atoi(parts[3].c_str()), parts[4] == "1", parts[5] });
				spawns++;
			}
		}
		if (messages.empty() && spawns == 0) {
			// Not a spawn reply at all: an older site without the spawn lookup, or an error page.
			const std::string first = r.body.substr(0, std::min<size_t>(r.body.find('\n'), 100));
			Say("GrailTrackrr test: unexpected reply from the server (HTTP %u): %s", r.status, first.c_str());
		}
		for (const auto& m : messages) {
			Say("GrailTrackrr test: %s", m.c_str());
		}
	} catch (...) {
	}
}

// Worker thread: look up a typed spawn command.
static void TrySpawnLookup() {
	std::string query;
	{
		std::lock_guard guard(g.lock);
		if (g.spawnQuery.empty() || g.spawnLookup) {
			return;
		}
		query        = g.spawnQuery;
		g.spawnQuery.clear();
		g.spawnLookup = true;
	}
	Say("GrailTrackrr test: asking the server for \"%s\"", query.c_str());
	HttpSend(false, "/api/plugin/spawn?q=" + UrlEncode(query), {}, OnSpawnReply, nullptr);
}

struct SpawnJob {
	std::vector<decltype(g.spawnQueue)::value_type> items;
};

static auto QualityFromName(const std::string& q) -> D2RL::Items::Quality {
	if (q == "unique") return D2RL::Items::Quality::Unique;
	if (q == "set") return D2RL::Items::Quality::Set;
	return D2RL::Items::Quality::Normal;
}

static auto ItemResultName(D2RL::Items::Result r) -> const char* {
	using R = D2RL::Items::Result;
	switch (r) {
	case R::InvalidArgument: return "invalid request";
	case R::Unsupported: return "not supported here";
	case R::Unavailable: return "no room, or not available";
	case R::Conflict: return "conflict";
	case R::NotFound: return "not found";
	case R::Busy: return "busy";
	case R::NotAuthoritative: return "not allowed in this game (single player you host only)";
	case R::PolicyRejected: return "rejected by a page policy";
	default: return "error";
	}
}

// Game thread: create the items in the inventory, else the personal stash, else the Cube.
// Not on the ground: dropping there trips a game assertion (BC_ASSERT hRoom) in some places.
static void __cdecl SpawnOnGameThread(const D2RL::PluginContext* context, void* user) noexcept {
	auto* job = static_cast<SpawnJob*>(user);
	try {
		// The authoritative player, looked up here on the game thread (as the SDK's item sample does).
		D2RL::PlayerHandle player = D2RL::InvalidPlayerHandle;
		if (g.inventory->getLocalPlayer(context, &player) != D2RL::Inventory::Result::Success) {
			Say("GrailTrackrr test: could not find your character on the game thread");
			delete job;
			return;
		}
		int made = 0;
		for (const auto& s : job->items) {
			D2RL::Items::ItemCreateSpec spec {};
			spec.structSize      = D2RL::Items::ItemCreateSpecSize;
			// Testing the same unique twice in one game is fine here.
			spec.flags           = s.quality == "unique" ? D2RL::Items::ItemCreateFlagBit(D2RL::Items::ItemCreateFlag::AllowDuplicateUnique) : 0U;
			spec.code            = PackCode(s.code);
			spec.quality         = QualityFromName(s.quality);
			spec.qualityRecordId = s.row >= 0 ? static_cast<uint32_t>(s.row) : D2RL::Items::RandomQualityRecord;
			spec.itemLevel       = 90;
			spec.seedMode        = D2RL::Items::SeedMode::Random;
			spec.quantity        = D2RL::Items::DefaultValue;
			spec.durability      = D2RL::Items::DefaultValue;
			spec.socketCount     = 0;
			spec.stateFlags      = D2RL::Items::ItemStateIdentified | (s.eth ? D2RL::Items::ItemStateEthereal : 0U);
			spec.destination.structSize = D2RL::Items::ItemDestinationSize;
			spec.destination.placement  = D2RL::Items::Placement::Automatic;
			D2RL::ItemHandle handle {};
			{
				// Marked before creating, so the scan that spots it reports it as a test find.
				std::lock_guard guard(g.lock);
				g.spawnedPending[SpawnSignature(s.code, s.quality, s.row)] = NowMs() + 30 * 60'000;
			}
			auto result = D2RL::Items::Result::Unavailable;
			for (const auto container : { D2RL::Items::ItemContainer::Inventory, D2RL::Items::ItemContainer::PersonalStash, D2RL::Items::ItemContainer::Cube }) {
				spec.destination.container = container;
				result                     = g.items->createItem(context, player, &spec, &handle);
				if (result == D2RL::Items::Result::Success) {
					break;
				}
			}
			if (result == D2RL::Items::Result::Success) {
				made++;
			} else {
				std::lock_guard guard(g.lock);
				g.spawnedPending.erase(SpawnSignature(s.code, s.quality, s.row));
				Say("GrailTrackrr test: could not create %s (code %s): %s", s.name.c_str(), s.code.c_str(), ItemResultName(result));
			}
		}
		if (made > 0) {
			Say("GrailTrackrr test: spawned %d item(s) into your inventory or stash", made);
		}
		if (made < static_cast<int>(job->items.size())) {
			Say("GrailTrackrr test: some items could not be created; make room in the inventory or stash (see the D2RLoader log)");
		}
	} catch (...) {
	}
	delete job;
}

// UI thread (from the scan): hand looked-up items to the game thread with the local player.
static void StartQueuedSpawns() {
	auto* job = new SpawnJob {};
	{
		std::lock_guard guard(g.lock);
		job->items.swap(g.spawnQueue);
	}
	if (job->items.empty()) {
		delete job;
		return;
	}
	Say("GrailTrackrr test: creating %zu item(s)", job->items.size());
	if (g.threads->runOnGameThread == nullptr || g.threads->runOnGameThread(g.ctx, SpawnOnGameThread, job) != D2RL::Threads::Result::Success) {
		delete job;
		Say("GrailTrackrr test: items can only be created in a single player game you host");
	}
}

// ---------------------------------------------------------------------------
// Console

static auto StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	// "grailtrackrr spawn <what>": test accounts only.
	std::string args = command->args != nullptr ? std::string(command->args, command->argsLength) : std::string();
	args.erase(0, args.find_first_not_of(" \t"));
	if (args.rfind("popup", 0) == 0) {
		{
			std::lock_guard guard(g.lock);
			// Shown 3 seconds from now, so there is time to close the console.
			g.popups.push_back(Popup { "unique", "Test popup", "If you can read this, popups work", NowMs() + 3'000 });
		}
		command->plugin->WriteConsoleMessage(ui.ready ? "GrailTrackrr: close the console; a test popup shows in 3 seconds." : ("GrailTrackrr: the in-game panels are off: " + panelSetup).c_str());
		return D2RL::ConsoleCommandResult::Handled;
	}
	if (args.rfind("spawn", 0) == 0) {
		std::string what = args.substr(5);
		what.erase(0, what.find_first_not_of(" \t"));
		bool test = false, inGame = false;
		{
			std::lock_guard guard(g.lock);
			test   = g.testMode;
			inGame = g.inGame;
			if (test && inGame) {
				g.spawnQuery = what.empty() ? "help" : what;
			}
		}
		command->plugin->WriteConsoleMessage(!test ? "GrailTrackrr: spawn only works on test accounts (an admin turns that on)."
			: !inGame                             ? "GrailTrackrr: load a character first."
			                                      : "GrailTrackrr: looking that up...");
		return D2RL::ConsoleCommandResult::Handled;
	}
	char message[1024] {};
	{
		std::lock_guard guard(g.lock);
		const uint32_t err = ui.lastError.load();
		std::snprintf(message, sizeof(message),
			"GrailTrackrr %s | %s | server=%s api_key=%s | character=%s level=%d | queued events=%zu | panels=%s%s%s%s show_status=%s | test_account=%s scan_items=%s scan_thread=%s item_stats=%s item_text=%s",
			PluginVersion, g.statusLine.c_str(), g.config.serverUrl.c_str(), g.config.apiKey.empty() ? "missing" : "set", g.charName.c_str(), g.clvl, g.outbox.size(),
			ui.ready ? "on" : ("off: " + panelSetup).c_str(), err != 0 ? " (last error: " : "", err != 0 ? (ui.lastErrorWhat + " " + std::to_string(err)).c_str() : "", err != 0 ? ")" : "",
			g.config.showStatus ? "on" : "off", g.testMode ? "yes" : "no", g.config.scanItems ? "on" : "off", g.config.scanOnGame ? "game" : "ui", g.config.itemStats ? "on" : "off", g.config.itemText ? "on" : "off");
	}
	command->plugin->WriteConsoleMessage(message);
	return D2RL::ConsoleCommandResult::Handled;
}

// ---------------------------------------------------------------------------
// Entry points

template <typename Service>
static auto Query(const D2RL::PluginContext* context, const Service** service) -> bool {
	return context->QueryService(service) == D2RL::ServiceQueryResult::Success && *service != nullptr;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &GrailTrackrrInfo;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	g.ctx = context;
	try {
		LoadConfig();

		Log("GrailTrackrr %s starting (D2RLoader build %s).", PluginVersion, D2RL::GetBuildVersion(context) != nullptr ? D2RL::GetBuildVersion(context) : "unknown");
		const D2RL::LifecycleService*   lifecycle = nullptr;
		const D2RL::SharedEventService* shared    = nullptr;

		// Each requirement is checked and logged on its own so a missing one is easy to spot.
		const uint32_t forEachEnd   = static_cast<uint32_t>(offsetof(D2RL::InventoryService, forEachInventoryItem) + sizeof(D2RL::Inventory::ForEachInventoryItemFn));
		const uint32_t gameplayEnd  = static_cast<uint32_t>(offsetof(D2RL::LifecycleService, unregisterGameplayEventListener) + sizeof(D2RL::Lifecycle::UnregisterGameplayEventListenerFn));
		const uint32_t deathEnd     = static_cast<uint32_t>(offsetof(D2RL::LifecycleService, registerMonsterDeathListener) + sizeof(D2RL::Lifecycle::RegisterMonsterDeathListenerFn));
		const uint32_t getInfoEnd   = static_cast<uint32_t>(offsetof(D2RL::ItemService, getItemInfo) + sizeof(D2RL::Items::GetItemInfoFn));
		struct Need { const char* name; bool ok; };
		const Need needs[] {
			{ "inventory", Query(context, &g.inventory) && D2RL::HasInventoryServiceField(g.inventory, forEachEnd) },
			{ "item", Query(context, &g.items) && D2RL::HasItemServiceField(g.items, getInfoEnd) },
			{ "thread", Query(context, &g.threads) && D2RL::HasThreadServiceField(g.threads, D2RL::ThreadServiceRequiredSize) },
			{ "HTTP", Query(context, &g.http) && D2RL::HasHttpServiceField(g.http, D2RL::HttpServiceRequiredSize) },
			{ "lifecycle", Query(context, &lifecycle) && D2RL::HasLifecycleServiceField(lifecycle, gameplayEnd) },
		};
		bool missing = false;
		for (const auto& n : needs) {
			if (!n.ok) {
				Log("GrailTrackrr: required D2RLoader %s service is missing or too old.", n.name);
				missing = true;
			}
		}
		if (missing) {
			context->LogError("GrailTrackrr did not load: update D2RLoader to a version with plugin ABI 4 (HTTP support).");
			return false;
		}
		// Optional: without panels there are no popups or status line, but tracking still works.
		SetUpPanels(context);

		static constexpr D2RL::Lifecycle::GameplayEventKind Kinds[] {
			D2RL::Lifecycle::GameplayEventKind::GameJoined,
			D2RL::Lifecycle::GameplayEventKind::GameLeft,
			D2RL::Lifecycle::GameplayEventKind::LocalPlayerReady,
			D2RL::Lifecycle::GameplayEventKind::LevelChanged,
			D2RL::Lifecycle::GameplayEventKind::PlayerLevelChanged,
			D2RL::Lifecycle::GameplayEventKind::QuestCompleted,
			D2RL::Lifecycle::GameplayEventKind::PlayerResurrected,
		};
		for (const auto kind : Kinds) {
			const D2RL::Lifecycle::GameplayEventListener listener {
				.structSize = D2RL::Lifecycle::GameplayEventListenerSize,
				.kind       = kind,
				.callback   = OnGameplayEvent,
			};
			D2RL::Lifecycle::ListenerHandle handle = D2RL::Lifecycle::InvalidHandle;
			if (lifecycle->registerGameplayEventListener(context, &listener, &handle) != D2RL::Lifecycle::Result::Success) {
				// Deaths are optional; everything else is needed.
				if (kind == D2RL::Lifecycle::GameplayEventKind::PlayerResurrected) {
					context->LogWarn("GrailTrackrr: this D2RLoader does not report resurrection; deaths are not counted.");
					continue;
				}
				context->LogError("GrailTrackrr could not register gameplay events.");
				return false;
			}
		}
		const D2RL::Lifecycle::MonsterDeathListener death {
			.structSize = D2RL::Lifecycle::MonsterDeathListenerSize,
			.callback   = OnMonsterDeath,
		};
		D2RL::Lifecycle::ListenerHandle deathHandle = D2RL::Lifecycle::InvalidHandle;
		if (!D2RL::HasLifecycleServiceField(lifecycle, deathEnd) || lifecycle->registerMonsterDeathListener == nullptr
			|| lifecycle->registerMonsterDeathListener(context, &death, &deathHandle) != D2RL::Lifecycle::Result::Success) {
			context->LogWarn("GrailTrackrr: monster deaths unavailable; finds will have no kill context.");
		}

		if (Query(context, &shared) && D2RL::HasSharedEventServiceField(shared, D2RL::SharedEventServiceRequiredSize)) {
			const D2RL::SharedEvents::UiMessageListener listener {
				.structSize = D2RL::SharedEvents::UiMessageListenerSize,
				.priority   = -100,
				.callback   = OnUiMessage,
			};
			D2RL::SharedEvents::ListenerHandle handle = D2RL::SharedEvents::InvalidHandle;
			(void)shared->registerUiMessageListener(context, &listener, &handle);
		}

		(void)context->RegisterConsoleCommand("grailtrackrr", StatusCommand, "Show GrailTrackrr connection and tracking status.");

		const std::string configPath = context->pluginConfigPath != nullptr ? ToUtf8(fs::path(context->pluginConfigPath)) : "grailtrackrr.toml";
		if (g.config.serverUrl.empty() || g.config.apiKey.empty()) {
			g.statusLine = "GrailTrackrr: set api_key in " + configPath;
		} else if (g.config.serverUrl.rfind("http://", 0) == 0 && g.config.serverUrl.find("localhost") == std::string::npos && g.config.serverUrl.find("127.0.0.1") == std::string::npos) {
			context->LogWarn("GrailTrackrr: server_url uses plain http; your API key is sent unencrypted. Use https:// outside your home network.");
		}
		if (!g.config.apiKey.empty()) {
			LoadOutbox();
		}
		workerStop = false;
		worker     = std::thread(WorkerLoop);
		Log("GrailTrackrr %s loaded. Config: %s. Server: %s. API key: %s", PluginVersion, configPath.c_str(), g.config.serverUrl.c_str(),
			g.config.apiKey.empty() ? "missing" : "set");
		return true;
	} catch (...) {
		return false;
	}
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	workerStop = true;
	if (worker.joinable()) {
		worker.join();
	}
	try {
		SaveOutbox();
	} catch (...) {
	}
}
