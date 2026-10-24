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
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace geode::prelude;
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
	inline uint32_t frame = 0;
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
		recording = playing = injecting = noclipSeen = false;
		frame = 0;
		playIdx = 0;
		inputs.clear();
	}

	// Returns a non-empty reason string if a cheat is active.
	// scanMods = false skips the (slower) loaded-mod scan.
	inline std::string detectCheat(bool scanMods) {
		// 1) speedhack: scheduler time scale changed
		auto ts = CCDirector::get()->getScheduler()->getTimeScale();
		if (std::fabs(ts - 1.f) > 0.001f) return "Speedhack";

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

	inline void abortRecording(std::string const& why) {
		recording = false;
		inputs.clear();
		notify("Recording turned off: " + why + " detected", NotificationIcon::Error);
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
				RM::inputs.push_back({RM::frame, button, down, isPlayer1});
			}
		}
		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		if (!this->isMainPlay()) {
			GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
			return;
		}

		// ---- cheat watchdog: auto-disable recording ----
		if (RM::recording) {
			bool scan = (RM::frame % 120 == 0);
			auto why = RM::detectCheat(scan);
			if (!why.empty()) RM::abortRecording(why);
			else if (PlayLayer::get()->m_isPracticeMode) {
				RM::recording = false;
				RM::inputs.clear();
				RM::notify("Recording is not supported in practice mode", NotificationIcon::Error);
			}
		}

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
	static void onModify(auto& self) {
		// run outermost so we can see the final result of destroyPlayer
		(void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::First);
	}

	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		RM::resetAll();
		RM::levelID = level->m_levelID.value();
		RM::name = "main";
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
		RM::load();
		return true;
	}

	void resetLevel() {
		PlayLayer::resetLevel();
		RM::frame = 0;
		RM::playIdx = 0;
		if (RM::recording) RM::inputs.clear();
	}

	void destroyPlayer(PlayerObject* player, GameObject* obj) {
		bool wasDead = player->m_isDead;
		bool isAnticheat = obj && obj == m_anticheatSpike;
		PlayLayer::destroyPlayer(player, obj);
		// A real hazard hit that did not kill the player = noclip / god mode
		if (!wasDead && !isAnticheat && !m_levelEndAnimationStarted && !player->m_isDead) {
			RM::noclipSeen = true;
			if (RM::recording) RM::abortRecording("Noclip");
		}
	}

	void levelComplete() {
		PlayLayer::levelComplete();
		if (RM::recording) {
			RM::recording = false;
			if (RM::save()) RM::notify("Replay saved: " + RM::name);
		}
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
		this->addChild(menu, 50);

		float x = 80.f;
		float y = win.height / 2 + 75.f;

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
		this->addChild(label, 50);
		label->setID("rm-label"_spr);

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
			RM::recording = false;
			if (RM::save()) RM::notify("Replay saved: " + RM::name);
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
