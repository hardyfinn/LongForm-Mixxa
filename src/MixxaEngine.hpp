// MixxaEngine.hpp
// Mixxa's mute logic and its little DSP pieces (fades, kick ducking, glue
// compressor), with no VCV Rack code, so they can be tested on their own.
//
// Mutes are layered:
//   1. External: lanes from Long Form (LINK) or the poly ACT input.
//   2. Manual: your MUTE presses, scene recalls and random rolls. A manual
//      state overrides the external one for that channel until the next
//      section change (or until you press again, with "Manual mutes stay").
// Every change waits for the next beat (or 16th, or happens at once, from the
// right-click menu). LINK lanes are already on the beat, so they apply at once.
#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace mx {

static const int CH = 8;
static const int SCENES = 4;

enum Timing { NEXT_BEAT = 0, NEXT_SIXTEENTH = 1, IMMEDIATE = 2 };

struct Rng {
	uint64_t s = 0x2545F4914F6CDD1Dull;
	uint64_t next() {
		s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
		return s * 2685821657736338717ull;
	}
	double uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
	int below(int n) { return n <= 0 ? 0 : (int) (uniform() * n) % n; }
};

struct MuteLogic {
	// external layer
	bool extPresent = false;
	bool ext[CH] = {};             // true = muted by Long Form / ACT
	bool extPending[CH] = {};      // ACT changes waiting for the beat
	bool extTarget[CH] = {};
	// manual layer
	bool override_[CH] = {};       // manual state in charge
	bool manual[CH] = {};          // true = muted by hand
	bool pending[CH] = {};         // a press waiting for the beat
	bool target[CH] = {};
	// solo
	bool solo[CH] = {};
	bool soloPending[CH] = {};
	// scenes
	uint8_t scene[SCENES] = {0, 0, 0, 0};
	bool sceneStored[SCENES] = {};
	int sceneShown = -1;
	// random
	bool diceArmed = false;
	int count = 2;
	bool includeKick = true;       // the panel's KICK IN/OUT switch
	bool exclude[CH] = {};         // right-click: leave out of random mutes (channels 2-8)
	bool connected[CH] = {};       // set by the module: channel has an input
	bool avoidRepeat = true;
	bool backArmed = false;
	bool haveBack = false;
	bool backOverride[CH] = {}, backManual[CH] = {};
	Rng rng;

	bool manualStays = false;

	bool muted(int i) const { return override_[i] ? manual[i] : ext[i]; }
	bool anySolo() const {
		for (int i = 0; i < CH; i++) if (solo[i]) return true;
		return false;
	}
	bool audible(int i) const {
		if (muted(i)) return false;
		if (anySolo() && !solo[i]) return false;
		return true;
	}
	// amber light: you have overridden the external layer
	bool overridden(int i) const { return extPresent && override_[i] && manual[i] != ext[i]; }
	bool armed(int i) const { return pending[i] || extPending[i] || soloPending[i]; }

	// ---- presses ----
	void pressMute(int i) {
		if (pending[i]) { pending[i] = false; return; }   // second press cancels
		pending[i] = true;
		target[i] = !muted(i);
	}
	void pressSolo(int i) { soloPending[i] = !soloPending[i]; }
	void storeScene(int s) {
		uint8_t m = 0;
		for (int i = 0; i < CH; i++) if (muted(i)) m |= (uint8_t) (1 << i);
		scene[s] = m;
		sceneStored[s] = true;
		sceneShown = s;
	}
	void recallScene(int s) {
		if (s < 0 || s >= SCENES) return;
		for (int i = 0; i < CH; i++) {
			bool want = (scene[s] >> i) & 1;
			if (want != muted(i)) { pending[i] = true; target[i] = want; }
			else pending[i] = false;
		}
		sceneShown = s;
	}
	void dice() { diceArmed = true; }
	void back() { if (haveBack) backArmed = true; }

	// ---- external layer ----
	// fromLink: already on the beat, so apply now; otherwise wait like a press.
	void setExternal(int i, bool m, bool fromLink, bool quantise) {
		if (fromLink || !quantise) {
			ext[i] = m;
			extPending[i] = false;
			return;
		}
		if (m == ext[i]) { extPending[i] = false; return; }
		extPending[i] = true;
		extTarget[i] = m;
	}
	void clearExternal() {
		for (int i = 0; i < CH; i++) { ext[i] = false; extPending[i] = false; }
	}
	// Long Form started a new section: hand the channels back to it
	void sectionChange() {
		if (manualStays) return;
		for (int i = 0; i < CH; i++) override_[i] = false;
	}

	bool candidate(int i) const {
		if (!connected[i]) return false;
		if (i == 0 && !includeKick) return false;
		if (i > 0 && exclude[i]) return false;
		if (ext[i]) return false;      // Long Form / ACT has it muted already
		return true;
	}

	void roll() {
		int cand[CH], n = 0;
		for (int i = 0; i < CH; i++) if (candidate(i)) cand[n++] = i;
		if (n == 0) return;
		int k = std::max(0, std::min(count, n));
		// remember the state for BACK
		for (int i = 0; i < CH; i++) { backOverride[i] = override_[i]; backManual[i] = manual[i]; }
		haveBack = true;
		uint8_t before = 0;
		for (int j = 0; j < n; j++) if (muted(cand[j])) before |= (uint8_t) (1 << cand[j]);
		uint8_t chosen = 0;
		for (int attempt = 0; attempt < 8; attempt++) {
			int idx[CH];
			for (int j = 0; j < n; j++) idx[j] = cand[j];
			for (int j = n - 1; j > 0; j--) std::swap(idx[j], idx[rng.below(j + 1)]);
			chosen = 0;
			for (int j = 0; j < k; j++) chosen |= (uint8_t) (1 << idx[j]);
			// a new set only makes sense when there is more than one possible set
			if (!avoidRepeat || chosen != before || k == 0 || k == n) break;
		}
		for (int j = 0; j < n; j++) {
			int i = cand[j];
			override_[i] = true;
			manual[i] = (chosen >> i) & 1;
			pending[i] = false;
		}
		sceneShown = -1;
	}

	// ---- the beat: everything waiting happens now ----
	void quantum() {
		for (int i = 0; i < CH; i++) {
			if (extPending[i]) { ext[i] = extTarget[i]; extPending[i] = false; }
		}
		if (backArmed) {
			backArmed = false;
			for (int i = 0; i < CH; i++) { override_[i] = backOverride[i]; manual[i] = backManual[i]; pending[i] = false; }
			haveBack = false;
		}
		if (diceArmed) {
			diceArmed = false;
			roll();
		}
		for (int i = 0; i < CH; i++) {
			if (pending[i]) {
				pending[i] = false;
				override_[i] = true;
				manual[i] = target[i];
			}
			if (soloPending[i]) {
				soloPending[i] = false;
				solo[i] = !solo[i];
			}
		}
	}
};

// ---- a short linear fade per channel, so mutes never click ----
struct Fade {
	float g = 1.f;
	void process(bool on, float step) {
		float t = on ? 1.f : 0.f;
		if (g < t) g = std::min(t, g + step);
		else if (g > t) g = std::max(t, g - step);
	}
};

// ---- kick ducking ----
// A kick (trigger or audio) starts an envelope that falls back from 1 to 0
// over RELEASE. SHAPE bends the curve: 0 = gate-like (stays down, then snaps
// back), 0.5 = pump, 1 = smooth swell.
struct Ducker {
	float env = 0.f;      // follower of the kick signal
	bool high = false;
	double t = 1e9;       // seconds since the last kick
	float amount = 0.f;   // 0..1, how far down right now

	void process(float x, float dt, float release, float shape) {
		float a = std::fabs(x);
		env = a > env ? a : env * std::exp(-dt / 0.02f);
		if (!high && env > 2.5f) { high = true; t = 0.0; }
		else if (high && env < 1.0f) high = false;
		t += dt;
		amount = curve((float) (t / std::max(0.01f, release)), shape) * std::min(1.f, (float) (t / 0.002));
	}
	static float curve(float u, float s) {
		if (u >= 1.f) return 0.f;
		if (u <= 0.f) return 1.f;
		float a, b;
		if (s < 0.5f) { a = std::pow(8.f, 1.f - 2.f * s); b = 1.f + 2.f * s; }
		else { a = 1.f; b = 2.f + 4.f * (s - 0.5f); }
		return std::pow(1.f - std::pow(u, a), b);
	}
};

// ---- glue: a gentle stereo bus compressor ----
// 2:1, 10 ms attack, 150 ms release, 6 dB soft knee. GLUE (0..1) lowers the
// threshold from 0 to -8 dB (re 5 V) and adds up to 3 dB of make-up gain.
struct Glue {
	float env = 0.f;      // detector level in dB
	float grDb = 0.f;     // gain reduction now, in dB (positive)
	float sampleRate = 48000.f;

	float gainFor(float l, float r, float amount, float dt) {
		if (amount <= 0.001f) { grDb = 0.f; env = -120.f; return 1.f; }
		float peak = std::max(std::fabs(l), std::fabs(r));
		float lvl = 20.f * std::log10(std::max(1e-6f, peak / 5.f));
		float att = std::exp(-dt / 0.010f), rel = std::exp(-dt / 0.150f);
		// smooth the over-threshold amount, not the level
		float thr = -8.f * amount, knee = 6.f, ratio = 2.f;
		float over = lvl - thr;
		float gr;
		if (over <= -knee / 2) gr = 0.f;
		else if (over >= knee / 2) gr = over * (1.f - 1.f / ratio);
		else { float z = over + knee / 2; gr = (1.f - 1.f / ratio) * z * z / (2 * knee); }
		grDb = gr > grDb ? att * grDb + (1 - att) * gr : rel * grDb + (1 - rel) * gr;
		float makeup = 3.f * amount;
		return std::pow(10.f, (makeup - grDb) / 20.f);
	}
};

// ---- level helpers ----
// fader 0..1 -> gain; 0.7937 = 0 dB, 1 = +6 dB
inline float faderGain(float x) { return 2.f * x * x * x; }
// equal-power pan for mono sources, centre = unity on both sides
inline void panMono(float pan, float* gl, float* gr) {
	float th = (pan + 1.f) * 0.25f * 3.14159265f;
	*gl = std::cos(th) * 1.41421356f;
	*gr = std::sin(th) * 1.41421356f;
}
// balance for stereo sources
inline void panStereo(float pan, float* gl, float* gr) {
	*gl = std::min(1.f, 1.f - pan);
	*gr = std::min(1.f, 1.f + pan);
}

}  // namespace mx
