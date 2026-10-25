// Replay Macro - Geode mod for Geometry Dash 2.2081 (Geode v5.x)
//
// Features:
//  - Record button / Play button in the pause menu
//  - Add new replay, rename replay, switch between replays (per level)
//  - Recording is automatically turned OFF (and the data discarded)
//    when speedhack or noclip is detected.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace geode::prelude;

// Eclipse Menu public API (cheat indicator state). Header-only, talks to Eclipse at runtime.
#include "eclipse/modules.hpp"
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Replay state + helpers
// ---------------------------------------------------------------------------
struct RInput {
	uint32_t frame;
	int button;
	bool down;
	bool p1;
};

namespace RM {
	inline bool recording = false;
	inline bool playing = false;
	inline bool injecting = false;
	inline bool noclipSeen = false;
	inline bool abortActive = false;
	inline std::string abortReason;
	inline std::chrono::steady_clock::time_point abortAt;
	inline bool speedCheat = false;
	inline bool haveLast = false;
	inline double accSim = 0.0, accReal = 0.0;
	inline std::chrono::steady_clock::time_point lastT;
	inline uint32_t tick = 0;
	inline double lastRatio = 1.0;   // debug: last measured game/real speed ratio
	inline uint32_t updates = 0;     // debug: PlayLayer::update calls
	inline uint32_t steps = 0;       // debug: processCommands calls
	inline uint32_t frame = 0;
	inline std::vector<RInput> bestInputs;  // longest attempt so far (temporary, in memory)
	inline uint32_t bestFrames = 0;
	inline bool attemptBanked = false;      // current attempt already stored / dead -> stop recording inputs
	inline size_t playIdx = 0;
	inline int levelID = 0;
	inline std::string name = "main";
	inline std::vector<RInput> inputs;

	inline void notify(std::string const& msg, NotificationIcon icon = NotificationIcon::Success) {
		Notification::create(msg, icon)->show();
	}

	inline std::string lower(std::string s) {
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
		return s;
	}

	inline std::string sanitize(std::string const& in) {
		std::string out;
		for (char c : in) {
			if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') out += c;
		}
		if (out.size() > 24) out.resize(24);
		return out;
	}

	inline fs::path dir() {
		auto p = Mod::get()->getSaveDir() / "replays";
		std::error_code ec;
		fs::create_directories(p, ec);
		return p;
	}

	inline fs::path pathFor(std::string const& n) {
		return dir() / fmt::format("{}__{}.rmr", levelID, n);
	}

	inline bool exists(std::string const& n) {
		std::error_code ec;
		return fs::exists(pathFor(n), ec);
	}

	inline bool save() {
		std::ofstream f(pathFor(name), std::ios::trunc);
		if (!f) return false;
		f << "RMR1\n";
		for (auto& i : inputs) {
			f << i.frame << ' ' << i.button << ' ' << (i.down ? 1 : 0) << ' ' << (i.p1 ? 1 : 0) << '\n';
		}
		return true;
	}

	inline bool load() {
		inputs.clear();
		std::ifstream f(pathFor(name));
		if (!f) return false;
		std::string header;
		std::getline(f, header);
		if (header != "RMR1") return false;
		uint32_t fr; int b, d, p;
		while (f >> fr >> b >> d >> p) {
			inputs.push_back({fr, b, d != 0, p != 0});
		}
		return true;
	}

	inline std::vector<std::string> list() {
		std::vector<std::string> out;
		std::string prefix = fmt::format("{}__", levelID);
		std::error_code ec;
		for (auto& e : fs::directory_iterator(dir(), ec)) {
			if (e.path().extension() != ".rmr") continue;
			auto stem = e.path().stem().string();
			if (stem.rfind(prefix, 0) == 0) out.push_back(stem.substr(prefix.size()));
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	inline void resetAll() {
		recording = playing = injecting = noclipSeen = speedCheat = haveLast = abortActive = false;
		accSim = accReal = 0.0;
		bestInputs.clear();
		bestFrames = 0;
		attemptBanked = false;
		frame = 0;
		playIdx = 0;
		inputs.clear();
	}

	// --- real-time speed measurement (catches speedhacks that scale dt) ---

	inline void sampleSpeed(float dt) {
		updates++;
		auto now = std::chrono::steady_clock::now();
		if (!haveLast) { lastT = now; haveLast = true; return; }
		double real = std::chrono::duration<double>(now - lastT).count();
		lastT = now;
		if (real > 0.5 || dt > 0.5f) { accSim = accReal = 0.0; return; } // pause / hitch
		accSim += dt;
		accReal += real;
		if (accReal >= 0.25) {
			double ratio = accSim / accReal;
			lastRatio = ratio;
			speedCheat = (ratio > 1.15 || ratio < 0.85);
			accSim = accReal = 0.0;
		}
	}

	// Ask Eclipse (its cheat indicator) whether any cheat is active.
	// Returns the name of the active hack, or "" if none / Eclipse not installed.
	// Cached for a few calls to stay cheap; force=true refreshes immediately.
	inline std::string eclipseCheat(bool force) {
		static uint32_t n = 0;
		static std::string cache;
		if (force || (n++ & 7) == 0) {
			cache.clear();
			if (Loader::get()->isModLoaded("eclipse.eclipse-menu")) {
				auto list = eclipse::getEnabledCheats();
				if (!list.empty()) cache = std::string(list[0].name);
				else if (eclipse::hasCheatsEnabled()) cache = "Eclipse cheat";
			}
		}
		return cache;
	}

	// Returns a non-empty reason string if a cheat is active.
	// scanMods = false skips the (slower) loaded-mod scan.
	inline std::string detectCheat(bool scanMods) {
		// 0) Eclipse's own cheat indicator (most reliable when Eclipse is installed)
		if (auto e = eclipseCheat(scanMods); !e.empty()) return e;

		// 1) speedhack: scheduler time scale changed
		auto ts = CCDirector::get()->getScheduler()->getTimeScale();
		if (std::fabs(ts - 1.f) > 0.001f) return "Speedhack";
		if (speedCheat) return "Speedhack";

		// 2) noclip: player survived a hazard hit (see PlayLayer hook below)
		if (noclipSeen) return "Noclip";

		// 3) enabled mods whose id / name match the keyword list
		if (scanMods) {
			auto kwRaw = lower(Mod::get()->getSettingValue<std::string>("cheat-keywords"));
			std::vector<std::string> kws;
			std::stringstream ss(kwRaw);
			std::string tok;
			while (std::getline(ss, tok, ',')) {
				tok.erase(std::remove_if(tok.begin(), tok.end(), ::isspace), tok.end());
				if (!tok.empty()) kws.push_back(tok);
			}
			for (auto mod : Loader::get()->getAllMods()) {
				if (mod == Mod::get() || !mod->isLoaded()) continue;
				auto id = lower(mod->getID());
				auto nm = lower(mod->getName());
				for (auto& kw : kws) {
					if (id.find(kw) != std::string::npos || nm.find(kw) != std::string::npos) {
						return mod->getName();
					}
				}
			}
		}
		return "";
	}

	inline void abortRecording(std::string const& msg) {
		recording = false;
		inputs.clear();
		bestInputs.clear();
		bestFrames = 0;
		abortActive = true;
		abortReason = msg;
		abortAt = std::chrono::steady_clock::now();
		notify("Recording turned off: " + msg, NotificationIcon::Error);
	}

	// Store the current attempt temporarily if it is the longest so far.
	inline void bankAttempt() {
		if (!recording || attemptBanked) return;
		attemptBanked = true;
		if (frame > bestFrames && !inputs.empty()) {
			bestFrames = frame;
			bestInputs = inputs;
		}
	}

	// Stop recording and save the LONGEST attempt, but only if a final cheat check passes.
	inline void finishRecording(bool completed = false) {
		if (completed) {            // level beaten: the finished run is the one to keep
			bestInputs = inputs;
			bestFrames = frame;
		} else {
			bankAttempt();          // count the attempt in progress too
		}
		recording = false;
		uint32_t frames = bestFrames;
		if (frames > 0) inputs = bestInputs;
		bestInputs.clear();
		bestFrames = 0;

		auto why = detectCheat(true);
		if (why.empty() && Loader::get()->isModLoaded("eclipse.eclipse-menu") && eclipse::hasCheatedInAttempt()) {
			why = "Cheat used this attempt";
		}
		if (!why.empty()) {
			inputs.clear();
			abortActive = true;
			abortReason = why + " detected";
			abortAt = std::chrono::steady_clock::now();
			notify("Replay NOT saved: " + why + " detected", NotificationIcon::Error);
			return;
		}
		if (inputs.empty()) {
			notify("Nothing to save (no inputs recorded)", NotificationIcon::Error);
			return;
		}
		if (save()) notify(fmt::format("Replay saved: {} (longest attempt: {} frames)", name, frames));
	}

	// Called every frame while recording: turns recording off on cheats / practice mode.
	inline void guard(PlayLayer* pl) {
		if (!recording || !pl) return;
		bool scan = (tick++ % 120 == 0);
		auto why = detectCheat(scan);
		if (!why.empty()) { abortRecording(why + " detected"); return; }
		if (pl->m_isPracticeMode) abortRecording("practice mode is on");
	}
}

// ---------------------------------------------------------------------------
// Add / Rename popup
// ---------------------------------------------------------------------------
class RMNamePopup : public geode::Popup {
protected:
	bool m_rename = false;
	std::function<void()> m_done;
	TextInput* m_input = nullptr;

	bool init(bool rename, std::function<void()> done) {
		if (!Popup::init(260.f, 150.f)) return false;
		m_rename = rename;
		m_done = std::move(done);
		this->setTitle(rename ? "Rename Replay" : "Add Replay");

		auto size = m_mainLayer->getContentSize();

		m_input = TextInput::create(180.f, "Replay name");
		m_input->setFilter("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");
		m_input->setMaxCharCount(24);
		if (rename) m_input->setString(RM::name);
		m_input->setPosition({size.width / 2, size.height / 2 + 5.f});
		m_mainLayer->addChild(m_input);

		auto spr = ButtonSprite::create(rename ? "Rename" : "Add");
		auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(RMNamePopup::onConfirm));
		btn->setPosition({size.width / 2, 30.f});
		m_buttonMenu->addChild(btn);
		return true;
	}

	void onConfirm(CCObject*) {
		auto n = RM::sanitize(m_input->getString());
		if (n.empty()) {
			RM::notify("Enter a name", NotificationIcon::Error);
			return;
		}
		if (RM::exists(n)) {
			RM::notify("A replay with that name already exists", NotificationIcon::Error);
			return;
		}

		if (m_rename) {
			if (RM::exists(RM::name)) {
				std::error_code ec;
				fs::rename(RM::pathFor(RM::name), RM::pathFor(n), ec);
				if (ec) {
					RM::notify("Rename failed", NotificationIcon::Error);
					return;
				}
			}
			RM::name = n;
			RM::notify("Renamed to " + n);
		} else {
			RM::name = n;
			RM::inputs.clear();
			RM::save();
			RM::notify("Created replay " + n);
		}

		if (m_done) m_done();
		this->onClose(nullptr);
	}

public:
	static RMNamePopup* create(bool rename, std::function<void()> done) {
		auto ret = new RMNamePopup();
		if (ret->init(rename, std::move(done))) {
			ret->autorelease();
			return ret;
		}
		delete ret;
		return nullptr;
	}
};

// ---------------------------------------------------------------------------
// Input recording + playback + cheat detection
// ---------------------------------------------------------------------------
class $modify(RMBaseLayer, GJBaseGameLayer) {
	bool isMainPlay() {
		return PlayLayer::get() && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == this;
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		if (this->isMainPlay()) {
			if (RM::playing && !RM::injecting) return; // block manual input while replaying
			if (RM::recording && !RM::injecting) {
				if (RM::speedCheat || RM::noclipSeen) {
					RM::abortRecording(std::string(RM::speedCheat ? "Speedhack" : "Noclip") + " detected");
				} else if (!RM::attemptBanked) {
					RM::inputs.push_back({RM::frame, button, down, isPlayer1});
				}
			}
		}
		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		if (!this->isMainPlay()) {
			GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
			return;
		}

		RM::steps++;

		// ---- cheat / practice watchdog ----
		if (RM::recording) RM::guard(PlayLayer::get());

		// ---- playback ----
		if (RM::playing) {
			RM::injecting = true;
			while (RM::playIdx < RM::inputs.size() && RM::inputs[RM::playIdx].frame <= RM::frame) {
				auto& i = RM::inputs[RM::playIdx++];
				this->handleButton(i.down, i.button, i.p1);
			}
			RM::injecting = false;
		}

		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		RM::frame++;
	}
};

class $modify(RMPlayLayer, PlayLayer) {
	struct Fields {
		CCDrawNode* dot = nullptr;
		CCLabelBMFont* label = nullptr;
		CCLabelBMFont* dbg = nullptr;
		std::string key;
	};

	void updateRM() {
		if (!m_fields->dot) return;

		// live debug readout (speed ratio, time scale, hook counters)
		if (m_fields->dbg && RM::updates % 15 == 0) {
			auto ts = CCDirector::get()->getScheduler()->getTimeScale();
			m_fields->dbg->setString(fmt::format("x{:.2f} ts{:.2f} u{} s{}",
				RM::lastRatio, ts, RM::updates, RM::steps).c_str());
		}

		int st = -1;
		std::string text;
		auto why = RM::detectCheat(false);
		bool recent = RM::abortActive &&
			std::chrono::duration<double>(std::chrono::steady_clock::now() - RM::abortAt).count() < 4.0;
		if (!why.empty()) { st = 2; text = why; }
		else if (recent) { st = 2; text = RM::abortReason; }
		else if (RM::recording) { st = 0; text = "REC"; }
		else if (RM::playing) { st = 1; text = "PLAY"; }

		std::string key = std::to_string(st) + text;
		if (key == m_fields->key) return;
		m_fields->key = key;

		m_fields->dot->clear();
		if (st < 0) {
			m_fields->label->setString("");
			return;
		}
		ccColor4F c = st == 0 ? ccc4f(0.2f, 1.f, 0.3f, 1.f)
			: st == 1 ? ccc4f(0.3f, 0.6f, 1.f, 1.f)
			: ccc4f(1.f, 0.2f, 0.2f, 1.f);
		m_fields->dot->drawDot({0.f, 0.f}, 8.f, ccc4f(1.f, 1.f, 1.f, 1.f)); // white rim
		m_fields->dot->drawDot({0.f, 0.f}, 6.5f, c);
		m_fields->label->setString(text.c_str());
	}

	static void onModify(auto& self) {
		// run outermost so we can see the final result of destroyPlayer
		(void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::First);
	}

	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		RM::resetAll();
		RM::levelID = level->m_levelID.value();
		RM::name = "main";
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

		// cheat / status indicator: circle at left-center of the screen
		auto win = CCDirector::get()->getWinSize();
		auto dot = CCDrawNode::create();
		dot->setPosition({14.f, win.height / 2});
		m_uiLayer->addChild(dot, 100);
		auto label = CCLabelBMFont::create("", "bigFont.fnt");
		label->setScale(0.3f);
		label->setAnchorPoint({0.f, 0.5f});
		label->setPosition({26.f, win.height / 2});
		m_uiLayer->addChild(label, 100);
		m_fields->dot = dot;
		m_fields->label = label;

		RM::load();
		return true;
	}

	void update(float dt) {
		RM::sampleSpeed(dt);
		PlayLayer::update(dt);
		if (RM::recording) RM::guard(this);
		this->updateRM();
	}

	void togglePracticeMode(bool practice) {
		PlayLayer::togglePracticeMode(practice);
		if (practice && RM::recording) RM::abortRecording("practice mode is on");
	}

	void resetLevel() {
		if (RM::recording) RM::bankAttempt(); // manual restart / respawn without death hook
		PlayLayer::resetLevel();
		RM::frame = 0;
		RM::playIdx = 0;
		if (RM::recording) RM::inputs.clear();
		RM::attemptBanked = false;
	}

	void destroyPlayer(PlayerObject* player, GameObject* obj) {
		bool wasDead = player->m_isDead;
		bool isAnticheat = obj && obj == m_anticheatSpike;
		PlayLayer::destroyPlayer(player, obj);
		// A real hazard hit that did not kill the player = noclip / god mode
		if (!wasDead && !isAnticheat && !m_levelEndAnimationStarted && !player->m_isDead) {
			RM::noclipSeen = true;
			if (RM::recording) RM::abortRecording("Noclip detected");
		} else if (!wasDead && !isAnticheat && player->m_isDead && RM::recording) {
			RM::bankAttempt(); // died: keep this attempt temporarily if it is the longest
		}
	}

	void levelComplete() {
		PlayLayer::levelComplete();
		if (RM::recording) RM::finishRecording(true);
		RM::playing = false;
	}
};

// ---------------------------------------------------------------------------
// Pause menu UI
// ---------------------------------------------------------------------------
class $modify(RMPause, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();
		this->buildRM();
	}

	void buildRM() {
		this->removeChildByID("rm-menu"_spr);

		auto win = CCDirector::get()->getWinSize();
		auto menu = CCMenu::create();
		menu->setID("rm-menu"_spr);
		menu->setPosition({0.f, 0.f});
		const float S = 0.7f;
		menu->setScale(S);
		this->addChild(menu, 50);

		float x = 72.f;
		float y = (win.height / 2 + 62.f) / S;

		auto addBtn = [&](char const* text, char const* tex, SEL_MenuHandler cb, float yy) {
			auto spr = ButtonSprite::create(text, 100, true, "bigFont.fnt", tex, 28.f, .6f);
			auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
			btn->setPosition({x, yy});
			menu->addChild(btn);
		};

		// status label
		std::string status = RM::recording ? "REC" : (RM::playing ? "PLAY" : "idle");
		auto label = CCLabelBMFont::create(
			fmt::format("{}\n[{}] {} inputs", RM::name, status, RM::inputs.size()).c_str(), "bigFont.fnt");
		label->setScale(.35f);
		label->setAlignment(kCCTextAlignmentCenter);
		label->setPosition({x, y});
		menu->addChild(label); // lives in the menu so it is removed on rebuild

		// replay selector arrows
		auto left = CCMenuItemSpriteExtra::create(
			CCSprite::createWithSpriteFrameName("edit_leftBtn_001.png"), this, menu_selector(RMPause::onRMPrev));
		left->setPosition({x - 60.f, y});
		menu->addChild(left);
		auto right = CCMenuItemSpriteExtra::create(
			CCSprite::createWithSpriteFrameName("edit_rightBtn_001.png"), this, menu_selector(RMPause::onRMNext));
		right->setPosition({x + 60.f, y});
		menu->addChild(right);

		addBtn(RM::recording ? "Stop Rec" : "Record",
			RM::recording ? "GJ_button_06.png" : "GJ_button_01.png",
			menu_selector(RMPause::onRMRecord), y - 35.f);
		addBtn(RM::playing ? "Stop" : "Play",
			RM::playing ? "GJ_button_06.png" : "GJ_button_02.png",
			menu_selector(RMPause::onRMPlay), y - 70.f);
		addBtn("Add Replay", "GJ_button_04.png", menu_selector(RMPause::onRMAdd), y - 105.f);
		addBtn("Rename", "GJ_button_04.png", menu_selector(RMPause::onRMRename), y - 140.f);
	}

	void onRMRecord(CCObject*) {
		if (RM::recording) {
			RM::finishRecording();
			this->buildRM();
			return;
		}
		if (PlayLayer::get() && PlayLayer::get()->m_isPracticeMode) {
			RM::notify("Disable practice mode to record", NotificationIcon::Error);
			return;
		}
		auto why = RM::detectCheat(true);
		if (!why.empty()) {
			RM::notify("Cannot record: " + why + " is active", NotificationIcon::Error);
			return;
		}
		RM::noclipSeen = false;
		RM::playing = false;
		RM::recording = true;
		RM::inputs.clear();
		RM::bestInputs.clear();
		RM::bestFrames = 0;
		RM::attemptBanked = true; // the restart below must not bank the old attempt
		RM::notify("Recording... level restarted");
		this->onRestart(nullptr); // recording must start at frame 0
	}

	void onRMPlay(CCObject*) {
		if (RM::playing) {
			RM::playing = false;
			this->buildRM();
			return;
		}
		if (RM::recording) {
			RM::recording = false;
			RM::save();
		}
		if (!RM::load() || RM::inputs.empty()) {
			RM::notify("This replay is empty", NotificationIcon::Error);
			this->buildRM();
			return;
		}
		RM::playing = true;
		RM::notify("Playing " + RM::name);
		this->onRestart(nullptr);
	}

	void onRMAdd(CCObject*) {
		if (RM::recording || RM::playing) {
			RM::notify("Stop recording / playing first", NotificationIcon::Error);
			return;
		}
		RMNamePopup::create(false, [this]() { this->buildRM(); })->show();
	}

	void onRMRename(CCObject*) {
		if (RM::recording || RM::playing) {
			RM::notify("Stop recording / playing first", NotificationIcon::Error);
			return;
		}
		RMNamePopup::create(true, [this]() { this->buildRM(); })->show();
	}

	void cycleRM(int d) {
		if (RM::recording || RM::playing) {
			RM::notify("Stop recording / playing first", NotificationIcon::Error);
			return;
		}
		auto list = RM::list();
		if (list.empty()) {
			RM::notify("No saved replays for this level", NotificationIcon::Error);
			return;
		}
		int n = static_cast<int>(list.size());
		auto it = std::find(list.begin(), list.end(), RM::name);
		int i = (it == list.end()) ? 0 : (static_cast<int>(it - list.begin()) + d + n) % n;
		RM::name = list[i];
		RM::load();
		this->buildRM();
	}

	void onRMPrev(CCObject*) { this->cycleRM(-1); }
	void onRMNext(CCObject*) { this->cycleRM(1); }
};
