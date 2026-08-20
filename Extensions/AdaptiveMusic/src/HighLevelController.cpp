#include <EngineExt/AdaptiveMusic.hpp>
#include <Engine/Core/Log.hpp>

#include <sstream>
#include <cctype>
#include <cstdlib>
#include <random>

namespace EnderEngine::Extensions::AdaptiveMusic {

// ===================================================================
// Lexer/parser helpers (anonymous namespace)
// ===================================================================
namespace {

String toUpper(String s) {
	for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	return s;
}

String trim(const String& s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == String::npos) return "";
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

Vector<String> splitLine(const String& line) {
	Vector<String> out;
	std::istringstream iss(line);
	String t;
	while (iss >> t) out.push_back(t);
	return out;
}

bool isKw(const String& t, const char* kw) { return toUpper(t) == kw; }

bool parseF32(const String& s, F32& out) {
	const char* b = s.c_str();
	char* end = nullptr;
	out = std::strtof(b, &end);
	return end != b && *end == '\0';
}

// Parse-time statement (symbols not yet resolved)
struct ParsedStmt {
	bool isLabel = false;
	UInt32 line = 0;
	String labelName;

	HighLevelController::CmdType type = HighLevelController::CmdType::Nop;
	String trackName;
	F32 vol = 1.0f, pan = 0.0f, speed = 1.0f, pitch = 1.0f, fadeInSec = 0.0f; // Play
	F32 fadeOutSec = 0.0f;                                                   // Stop
	bool fadeIn = false; F32 toVol = 0.0f, sec = 0.0f; bool hasTo = false;   // Fade
	SetProperty setProp = SetProperty::Vol; F32 setVal = 0.0f;               // Set
	bool untilFade = false;                                                  // Wait
	Vector<String> randTargets;                                              // Random
};

bool parseLine(const Vector<String>& toks, UInt32 lineNo, ParsedStmt& out, HighLevelController::CompileError& err) {
	size_t i = 0;

	auto fail = [&](const String& msg) {
		err.type = HighLevelController::CompileError::ErrorType::UnexpectedToken;
		err.lineNum = lineNo;
		err.message = msg;
		return false;
	};
	auto eatKw = [&](const char* kw) -> bool {
		if (i < toks.size() && isKw(toks[i], kw)) { ++i; return true; }
		return false;
	};
	auto eatNum = [&](F32& out) -> bool {
		if (i >= toks.size()) return false;
		if (!parseF32(toks[i], out)) return false;
		++i;
		return true;
	};

	String kw = toUpper(toks[i++]);

	if (kw == "PLAY") {
		out.type = HighLevelController::CmdType::Play;
		if (!eatKw("TRACK") || i >= toks.size()) return fail("PLAY requires TRACK <name>");
		out.trackName = toks[i++];
		while (i < toks.size()) {
			if (eatKw("VOL")) { if (!eatNum(out.vol)) return fail("VOL expects a number"); }
			else if (eatKw("PAN")) { if (!eatNum(out.pan)) return fail("PAN expects a number"); }
			else if (eatKw("SPEED")) { if (!eatNum(out.speed)) return fail("SPEED expects a number"); }
			else if (eatKw("PITCH")) { if (!eatNum(out.pitch)) return fail("PITCH expects a number"); }
			else if (eatKw("FADEIN")) { if (!eatKw("SEC") || !eatNum(out.fadeInSec)) return fail("FADEIN requires SEC <num>"); }
			else return fail("Unknown argument in PLAY: '" + toks[i] + "'");
		}
		return true;
	}
	if (kw == "STOP") {
		out.type = HighLevelController::CmdType::Stop;
		if (!eatKw("TRACK") || i >= toks.size()) return fail("STOP requires TRACK <name>");
		out.trackName = toks[i++];
		if (i < toks.size()) {
			if (eatKw("FADEOUT")) { if (!eatKw("SEC") || !eatNum(out.fadeOutSec)) return fail("FADEOUT requires SEC <num>"); }
			else return fail("Unknown argument in STOP: '" + toks[i] + "'");
		}
		return true;
	}
	if (kw == "FADE") {
		out.type = HighLevelController::CmdType::Fade;
		if (i >= toks.size()) return fail("FADE requires IN or OUT");
		String dir = toUpper(toks[i++]);
		if (dir == "IN") out.fadeIn = true;
		else if (dir == "OUT") out.fadeIn = false;
		else return fail("FADE requires IN or OUT, got '" + toks[i - 1] + "'");
		if (!eatKw("TRACK") || i >= toks.size()) return fail("FADE requires TRACK <name>");
		out.trackName = toks[i++];
		if (i < toks.size() && eatKw("TO")) { if (!eatNum(out.toVol)) return fail("TO expects a number"); out.hasTo = true; }
		if (!eatKw("SEC") || !eatNum(out.sec)) return fail("FADE requires SEC <num>");
		if (!out.hasTo) out.toVol = out.fadeIn ? 1.0f : 0.0f;
		if (i < toks.size()) return fail("Unexpected trailing argument in FADE: '" + toks[i] + "'");
		return true;
	}
	if (kw == "SET") {
		out.type = HighLevelController::CmdType::Set;
		if (!eatKw("TRACK") || i >= toks.size()) return fail("SET requires TRACK <name>");
		out.trackName = toks[i++];
		if (i >= toks.size()) return fail("SET requires VOL|PAN|SPEED|PITCH <num>");
		String prop = toUpper(toks[i++]);
		if (prop == "VOL") out.setProp = SetProperty::Vol;
		else if (prop == "PAN") out.setProp = SetProperty::Pan;
		else if (prop == "SPEED") out.setProp = SetProperty::Speed;
		else if (prop == "PITCH") out.setProp = SetProperty::Pitch;
		else return fail("Unknown SET property: '" + toks[i - 1] + "'");
		if (!eatNum(out.setVal)) return fail("SET property expects a number");
		if (i < toks.size()) return fail("Unexpected trailing argument in SET: '" + toks[i] + "'");
		return true;
	}
	if (kw == "WAIT") {
		out.type = HighLevelController::CmdType::Wait;
		if (!eatKw("TRACK") || i >= toks.size()) return fail("WAIT requires TRACK <name>");
		out.trackName = toks[i++];
		if (i < toks.size() && eatKw("FADE")) out.untilFade = true;
		if (i < toks.size()) return fail("Unexpected trailing argument in WAIT: '" + toks[i] + "'");
		return true;
	}
	if (kw == "RANDOM") {
		out.type = HighLevelController::CmdType::Random;
		while (i < toks.size()) out.randTargets.push_back(toks[i++]);
		if (out.randTargets.empty()) return fail("RANDOM requires at least one label");
		return true;
	}
	if (kw == "HALT") {
		out.type = HighLevelController::CmdType::Halt;
		if (i < toks.size()) return fail("HALT takes no arguments");
		return true;
	}
	return fail("Unknown command: '" + toks[0] + "'");
}

} // namespace

// ===================================================================
// HighLevelController::Impl
// ===================================================================
struct HighLevelController::Impl {
	Player* player = nullptr;
	HashMap<String, TrackHandle> trackTable;
	Vector<Command> program;
	HashMap<String, UInt32> labels;

	UInt32 ip = 0;
	bool running = false;

	bool waiting = false;
	TrackHandle waitTrack = InvalidTrackHandle;
	bool waitFadeComplete = false;

	struct Fade {
		TrackHandle id = InvalidTrackHandle;
		F32 from = 0.0f, to = 0.0f, sec = 0.0f, elapsed = 0.0f;
		bool fadeOut = false;
	};
	Vector<Fade> fades;

	std::mt19937 rng{ std::random_device{}() };

	// ---------------- fade ----------------

	bool isFadeActive(TrackHandle id) const {
		for (const auto& f : fades) if (f.id == id) return true;
		return false;
	}

	void startFade(TrackHandle id, F32 from, F32 to, F32 sec, bool fadeOut) {
		if (sec <= 0.0f) {
			if (fadeOut) { player->setTrackVolume(id, 0.0f); player->stopTrack(id); }
			else player->setTrackVolume(id, to);
			return;
		}
		for (auto& f : fades) {
			if (f.id == id) {
				// The new fade takes over from the old one's *current*
				// interpolated volume, so back-to-back fades (e.g. a FADE OUT
				// issued right after a FADE IN) do not jump or restart from 0.
				F32 t = f.sec > 0.0f ? Clamp(f.elapsed / f.sec, 0.0f, 1.0f) : 1.0f;
				F32 cur = f.from + (f.to - f.from) * t;
				f = Fade{ id, cur, to, sec, 0.0f, fadeOut };
				return;
			}
		}
		fades.push_back(Fade{ id, from, to, sec, 0.0f, fadeOut });
	}

	void updateFades(F32 dt) {
		for (auto it = fades.begin(); it != fades.end();) {
			Fade& f = *it;
			f.elapsed += dt;
			F32 t = f.sec > 0.0f ? Clamp(f.elapsed / f.sec, 0.0f, 1.0f) : 1.0f;
			F32 v = f.from + (f.to - f.from) * t;
			player->setTrackVolume(f.id, v);
			if (t >= 1.0f) {
				if (f.fadeOut) player->stopTrack(f.id);
				it = fades.erase(it);
			}
			else {
				++it;
			}
		}
	}

	// ---------------- command execution ----------------

	void executePlay(const PlayCmdArgs& a) {
		if (a.fadeInSec > 0.0f) {
			player->setTrackVolume(a.id, 0.0f);
			player->playTrack(a.id);
			startFade(a.id, 0.0f, a.vol, a.fadeInSec, false);
		}
		else {
			player->setTrackVolume(a.id, a.vol);
			player->playTrack(a.id);
		}
		player->setTrackPan(a.id, a.pan);
		player->setTrackSpeed(a.id, a.speed);
		player->setTrackPitch(a.id, a.pitch);
	}

	void executeStop(const StopCmdArgs& a) {
		if (a.fadeOutSec > 0.0f) {
			startFade(a.id, player->getTrackVolume(a.id), 0.0f, a.fadeOutSec, true);
		}
		else {
			player->stopTrack(a.id);
		}
	}

	void executeFade(const FadeCmdArgs& a) {
		startFade(a.id, player->getTrackVolume(a.id), a.toVol, a.sec, !a.fadeIn);
	}

	void executeSet(const SetCmdArgs& a) {
		switch (a.prop) {
		case SetProperty::Vol:   player->setTrackVolume(a.id, a.value); break;
		case SetProperty::Pan:   player->setTrackPan(a.id, a.value); break;
		case SetProperty::Speed: player->setTrackSpeed(a.id, a.value); break;
		case SetProperty::Pitch: player->setTrackPitch(a.id, a.value); break;
		}
	}

	void runUntilBlocked() {
		while (running && ip < program.size()) {
			const Command& c = program[ip];
			switch (c.type) {
			case CmdType::Play: executePlay(std::get<PlayCmdArgs>(c.args)); ++ip; break;
			case CmdType::Stop: executeStop(std::get<StopCmdArgs>(c.args)); ++ip; break;
			case CmdType::Fade: executeFade(std::get<FadeCmdArgs>(c.args)); ++ip; break;
			case CmdType::Set:  executeSet(std::get<SetCmdArgs>(c.args));  ++ip; break;
			case CmdType::Random: {
				const auto& r = std::get<RandomCmdArgs>(c.args);
				if (r.targets.empty()) { ++ip; break; }
				std::uniform_int_distribution<size_t> d(0, r.targets.size() - 1);
				ip = r.targets[d(rng)];
				break;
			}
			case CmdType::Wait: {
				const auto& w = std::get<WaitCmdArgs>(c.args);
				waiting = true;
				waitTrack = w.id;
				waitFadeComplete = w.untilFadeComplete;
				++ip;
				return; // block until the condition is met in a later update()
			}
			case CmdType::Nop: ++ip; break;
			case CmdType::Halt:
			default:
				running = false;
				return;
			}
		}
		if (ip >= program.size()) running = false; // reached end of program
	}
};

// ===================================================================
// Public API
// ===================================================================
HighLevelController::HighLevelController() : m_impl(std::make_unique<Impl>()) {}
HighLevelController::~HighLevelController() = default;

void HighLevelController::attachPlayer(Player* player) { m_impl->player = player; }
Player* HighLevelController::player() const { return m_impl->player; }

void HighLevelController::bindTrack(const String& name, TrackHandle id) { m_impl->trackTable[name] = id; }
void HighLevelController::unbindTrack(const String& name) { m_impl->trackTable.erase(name); }
void HighLevelController::clearTracks() { m_impl->trackTable.clear(); }

bool HighLevelController::compile(const String& script, CompileError* outError) {
	auto& p = *m_impl;
	CompileError err; // local error, written back at the end

	p.program.clear();
	p.labels.clear();
	p.ip = 0;
	p.running = false;
	p.waiting = false;
	p.fades.clear();

	// ---- Pass 1: parse each line into a symbolic statement ----
	Vector<ParsedStmt> stmts;
	{
		std::istringstream iss(script);
		String raw;
		UInt32 lineNo = 0;
		while (std::getline(iss, raw)) {
			++lineNo;
			size_t semi = raw.find(';');
			String line = (semi == String::npos) ? raw : raw.substr(0, semi);
			line = trim(line);
			if (line.empty()) continue;

			// Label: a whole line ending with ':'
			if (line.back() == ':') {
				String name = trim(line.substr(0, line.size() - 1));
				if (name.empty()) {
					err = { CompileError::ErrorType::UnexpectedToken, lineNo, "Empty label name" };
					goto fail;
				}
				ParsedStmt s; s.isLabel = true; s.line = lineNo; s.labelName = name;
				stmts.push_back(std::move(s));
				continue;
			}

			auto toks = splitLine(line);
			if (toks.empty()) continue;
			ParsedStmt s; s.line = lineNo;
			if (!parseLine(toks, lineNo, s, err)) goto fail;
			stmts.push_back(std::move(s));
		}
	}

	// ---- Pass 2: label table (label -> next instruction index) + dup check ----
	{
		UInt32 instr = 0;
		for (const auto& s : stmts) {
			if (s.isLabel) {
				if (p.labels.contains(s.labelName)) {
					err = { CompileError::ErrorType::DuplicateLabel, s.line, "Duplicate label '" + s.labelName + "'" };
					goto fail;
				}
				p.labels[s.labelName] = instr;
			}
			else {
				++instr;
			}
		}
	}

	// ---- Pass 3: resolve symbols and emit commands ----
	for (const auto& s : stmts) {
		if (s.isLabel) continue;
		Command c; c.type = s.type;
		switch (s.type) {
		case CmdType::Play: {
			auto it = p.trackTable.find(s.trackName);
			if (it == p.trackTable.end()) {
				err = { CompileError::ErrorType::UnknownTrack, s.line, "Unknown track '" + s.trackName + "'" };
				goto fail;
			}
			PlayCmdArgs a; a.id = it->second; a.vol = s.vol; a.pan = s.pan; a.speed = s.speed; a.pitch = s.pitch; a.fadeInSec = s.fadeInSec;
			c.args = a;
			break;
		}
		case CmdType::Stop: {
			auto it = p.trackTable.find(s.trackName);
			if (it == p.trackTable.end()) {
				err = { CompileError::ErrorType::UnknownTrack, s.line, "Unknown track '" + s.trackName + "'" };
				goto fail;
			}
			StopCmdArgs a; a.id = it->second; a.fadeOutSec = s.fadeOutSec;
			c.args = a;
			break;
		}
		case CmdType::Fade: {
			auto it = p.trackTable.find(s.trackName);
			if (it == p.trackTable.end()) {
				err = { CompileError::ErrorType::UnknownTrack, s.line, "Unknown track '" + s.trackName + "'" };
				goto fail;
			}
			FadeCmdArgs a; a.id = it->second; a.fadeIn = s.fadeIn; a.toVol = s.toVol; a.sec = s.sec;
			c.args = a;
			break;
		}
		case CmdType::Set: {
			auto it = p.trackTable.find(s.trackName);
			if (it == p.trackTable.end()) {
				err = { CompileError::ErrorType::UnknownTrack, s.line, "Unknown track '" + s.trackName + "'" };
				goto fail;
			}
			SetCmdArgs a; a.id = it->second; a.prop = s.setProp; a.value = s.setVal;
			c.args = a;
			break;
		}
		case CmdType::Wait: {
			auto it = p.trackTable.find(s.trackName);
			if (it == p.trackTable.end()) {
				err = { CompileError::ErrorType::UnknownTrack, s.line, "Unknown track '" + s.trackName + "'" };
				goto fail;
			}
			WaitCmdArgs a; a.id = it->second; a.untilFadeComplete = s.untilFade;
			c.args = a;
			break;
		}
		case CmdType::Random: {
			RandomCmdArgs a;
			for (const auto& ln : s.randTargets) {
				auto lit = p.labels.find(ln);
				if (lit == p.labels.end()) {
					err = { CompileError::ErrorType::UndefinedLabel, s.line, "Undefined label '" + ln + "'" };
					goto fail;
				}
				a.targets.push_back(lit->second);
			}
			c.args = a;
			break;
		}
		case CmdType::Halt:
		case CmdType::Nop:
		default:
			break;
		}
		p.program.push_back(std::move(c));
	}

	if (p.program.empty()) {
		err = { CompileError::ErrorType::EmptyScript, 0, "Script contains no commands" };
		goto fail;
	}

	if (outError) *outError = {};
	return true;

fail:
	p.program.clear();
	p.labels.clear();
	if (outError) *outError = err;
	return false;
}

void HighLevelController::start() {
	auto& p = *m_impl;
	if (!p.player || p.program.empty()) return;
	p.ip = 0;
	p.waiting = false;
	p.waitTrack = InvalidTrackHandle;
	p.waitFadeComplete = false;
	p.fades.clear();
	p.running = true;
	p.runUntilBlocked();
}

void HighLevelController::stop() {
	m_impl->running = false;
	m_impl->waiting = false;
	// HALT semantics: tracks and in-flight fades are unaffected and keep playing
}

bool HighLevelController::isRunning() const { return m_impl->running; }

void HighLevelController::update(F32 dt) {
	auto& p = *m_impl;
	if (!p.player) return;
	if (dt < 0.0f) dt = 0.0f;

	p.updateFades(dt);

	if (!p.running) return;

	if (p.waiting) {
		bool satisfied = p.waitFadeComplete
			? !p.isFadeActive(p.waitTrack)
			: p.player->isTrackFinished(p.waitTrack);
		if (!satisfied) return;
		p.waiting = false;
		p.waitTrack = InvalidTrackHandle;
		p.waitFadeComplete = false;
	}

	p.runUntilBlocked();
}

} // namespace EnderEngine::Extensions::AdaptiveMusic
