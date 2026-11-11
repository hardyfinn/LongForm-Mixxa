// LongFormEngine.hpp
// The arrangement brain of Long Form, with no VCV Rack code in it, so it can be
// tested on its own (test/engine_test.cpp).
//
// Long Form counts clock pulses into beats and bars and walks through up to 8
// sections. Each section has a length in bars, an energy (0 to 10) and a set
// of 8 lanes that play in it. At the end of a section it picks the next one:
// in order (FOLLOW), or by how close each section's energy is to STEER.
// DRIFT makes one small edit to the form after a section ends; the edits sit
// on top of what you drew, so RESTORE can always take them away.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <algorithm>

namespace lf {

static const int ROWS = 8;
static const int LANES = 8;

// LEN knob positions -> bars (0 = the row is off)
static const int NUM_LENS = 11;
static const int LEN_BARS[NUM_LENS] = {0, 1, 2, 4, 8, 12, 16, 24, 32, 48, 64};

static const char* const DEFAULT_ROW_NAMES[ROWS] = {"Intro", "Groove", "Build", "Peak", "Break", "Drop", "Peak 2", "Outro"};
static const char* const DEFAULT_LANE_NAMES[LANES] = {"kick", "bass", "hats", "clap", "perc", "chords", "lead", "texture"};
static const char* const DEFAULT_MASKS[ROWS] = {"11000000", "11110010", "11111110", "11111111", "00001101", "10000011", "11111111", "01001000"};
static const int DEFAULT_LEN_INDEX[ROWS] = {6, 8, 4, 8, 6, 4, 8, 6};   // 16 32 8 32 16 8 32 16
static const int DEFAULT_ENERGY[ROWS] = {2, 4, 6, 8, 3, 7, 9, 1};

// small, seedable random generator (xorshift64*), so drift replays the same way
struct Rng {
	uint64_t s = 0x9E3779B97F4A7C15ull;
	void seed(uint64_t v) { s = v ? v : 0x9E3779B97F4A7C15ull; }
	uint64_t next() {
		s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
		return s * 2685821657736338717ull;
	}
	double uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
	int below(int n) { return n <= 0 ? 0 : (int) (uniform() * n) % n; }
};

struct LongFormEngine {
	// ---- what you drew (the knobs and the grid) ----
	bool mask[ROWS][LANES];
	int lenIndex[ROWS];       // from the LEN knobs
	int energyKnob[ROWS];     // from the ENERGY knobs
	std::string rowName[ROWS];
	std::string laneName[LANES];

	// ---- drift edits on top of it ----
	bool flip[ROWS][LANES];   // lane cells flipped by drift
	int lenShift[ROWS];       // x2 per step (-2..+2)
	int energyShift[ROWS];
	int order[ROWS];          // play order (drift swaps neighbours)
	std::string lastEdit;
	int lastEditRow = -1, lastEditLane = -1;  // for the flash on the grid
	uint32_t editCount = 0;

	// ---- performance settings (set every sample by the module) ----
	float steer = 5.f;        // 0..10
	float follow = 1.f;       // 0..1
	float drift = 0.f;        // 0..1
	bool hold = false;
	bool lock = false;
	int ppqn = 4;             // clock pulses per beat (4 = 16th notes)
	int cuePhrase = 4;        // cued rows start on this bar grid
	int phraseLen = 4;        // PHRASE output every N bars
	int fillIndex = 1;        // 1/2, 1, 2, 4 bars
	bool loopForm = true;     // false: stop at the end
	uint64_t seed = 1;

	// ---- play state ----
	int cur = -1;             // current row, -1 = nothing playable
	int bar = 0;              // bar within the section
	int pulse = 0;            // pulse within the bar
	bool started = false;     // false until the first pulse after a reset
	bool stopped = false;
	int cued = -1;
	bool nextRequested = false;
	Rng rng;

	// events from the last pulse (the module turns them into triggers)
	bool evBeat = false, evBar = false, evPhrase = false, evChange = false, evEnd = false, evSixteenth = false;

	LongFormEngine() { init(); }

	void init() {
		for (int r = 0; r < ROWS; r++) {
			for (int l = 0; l < LANES; l++) mask[r][l] = DEFAULT_MASKS[r][l] == '1';
			lenIndex[r] = DEFAULT_LEN_INDEX[r];
			energyKnob[r] = DEFAULT_ENERGY[r];
			rowName[r] = DEFAULT_ROW_NAMES[r];
		}
		for (int l = 0; l < LANES; l++) laneName[l] = DEFAULT_LANE_NAMES[l];
		restore();
		reset();
	}

	void restore() {
		for (int r = 0; r < ROWS; r++) {
			for (int l = 0; l < LANES; l++) flip[r][l] = false;
			lenShift[r] = 0;
			energyShift[r] = 0;
			order[r] = r;
		}
		lastEdit = "";
		lastEditRow = lastEditLane = -1;
	}

	// ---- effective values (drawn + drift) ----
	bool lane(int r, int l) const { return mask[r][l] != flip[r][l]; }
	int bars(int r) const {
		int b = LEN_BARS[std::max(0, std::min(NUM_LENS - 1, lenIndex[r]))];
		if (b == 0) return 0;
		int s = lenShift[r];
		while (s > 0) { b *= 2; s--; }
		while (s < 0) { b = std::max(1, b / 2); s++; }
		return std::min(64, b);
	}
	int energy(int r) const { return std::max(0, std::min(10, energyKnob[r] + energyShift[r])); }
	bool playable(int r) const { return r >= 0 && r < ROWS && bars(r) > 0; }
	int positionOf(int r) const {
		for (int p = 0; p < ROWS; p++) if (order[p] == r) return p;
		return 0;
	}
	int firstPlayable() const {
		for (int p = 0; p < ROWS; p++) if (playable(order[p])) return order[p];
		return -1;
	}
	// the next playable row in play order; sets wrapped if it went past the end
	int nextInOrder(int r, bool* wrapped) const {
		int p = r < 0 ? -1 : positionOf(r);
		for (int k = 1; k <= ROWS; k++) {
			int q = p + k;
			if (q >= ROWS && wrapped) *wrapped = true;
			int cand = order[q % ROWS];
			if (playable(cand)) return cand;
		}
		return -1;
	}
	int pulsesPerBar() const { return std::max(1, ppqn) * 4; }
	int fillPulses() const {
		static const float FILL_BARS[4] = {0.5f, 1.f, 2.f, 4.f};
		return (int) std::round(FILL_BARS[std::max(0, std::min(3, fillIndex))] * pulsesPerBar());
	}

	// ---- control ----
	void reset() {
		rng.seed(seed * 0x2545F4914F6CDD1Dull + 7);
		cur = firstPlayable();
		bar = 0;
		pulse = 0;
		started = false;
		stopped = false;
		cued = -1;
		nextRequested = false;
	}
	void cue(int r) { if (r >= 0 && r < ROWS) cued = (cued == r) ? -1 : r; }
	void cueSet(int r) { if (r >= 0 && r < ROWS) cued = r; }
	void requestNext() { nextRequested = true; }
	void toggleLane(int r, int l) { if (r >= 0 && r < ROWS && l >= 0 && l < LANES) mask[r][l] = !mask[r][l]; }

	// ---- choosing where to go ----
	int choose(bool* ended) {
		bool wrapped = false;
		int seq = nextInOrder(cur, &wrapped);
		if (seq < 0) return -1;
		if (rng.uniform() < follow) {
			if (wrapped) *ended = true;
			return seq;
		}
		// free choice: weight every playable row by how close its energy is to STEER
		double w[ROWS], total = 0.0;
		int count = 0;
		for (int r = 0; r < ROWS; r++) {
			w[r] = 0.0;
			if (!playable(r)) continue;
			count++;
			if (r == cur) continue;
			double d = energy(r) - steer;
			w[r] = std::exp(-d * d / (2.0 * 1.5 * 1.5)) + 1e-4;
			total += w[r];
		}
		if (count <= 1 || total <= 0.0) return seq;
		double x = rng.uniform() * total;
		for (int r = 0; r < ROWS; r++) {
			if (w[r] <= 0.0) continue;
			x -= w[r];
			if (x <= 0.0) return r;
		}
		return seq;
	}

	// one small edit to row r (the section that just finished)
	void driftEdit(int r) {
		if (lock || drift <= 0.f || r < 0) return;
		if (rng.uniform() >= drift) return;
		for (int attempt = 0; attempt < 4; attempt++) {
			int kind = rng.below(4);
			char buf[96];
			if (kind == 0) {
				int l = rng.below(LANES);
				flip[r][l] = !flip[r][l];
				std::snprintf(buf, sizeof(buf), "%s: %s %s", rowName[r].c_str(), laneName[l].c_str(), lane(r, l) ? "in" : "out");
				lastEditRow = r; lastEditLane = l;
			}
			else if (kind == 1) {
				int b = bars(r);
				bool up = b < 8 ? true : (b >= 64 ? false : rng.uniform() < 0.5);
				int nb = up ? b * 2 : b / 2;
				if (nb < 4 || nb > 64) continue;
				lenShift[r] += up ? 1 : -1;
				if (bars(r) == b) { lenShift[r] -= up ? 1 : -1; continue; }
				std::snprintf(buf, sizeof(buf), "%s: %d bars", rowName[r].c_str(), bars(r));
				lastEditRow = r; lastEditLane = -1;
			}
			else if (kind == 2) {
				int p = positionOf(r);
				int q = (p + 1) % ROWS;
				std::swap(order[p], order[q]);
				std::snprintf(buf, sizeof(buf), "swap %s and %s", rowName[order[q]].c_str(), rowName[order[p]].c_str());
				lastEditRow = r; lastEditLane = -1;
			}
			else {
				int e = energy(r);
				int d = (e <= 0) ? 1 : (e >= 10 ? -1 : (rng.uniform() < 0.5 ? 1 : -1));
				energyShift[r] += d;
				std::snprintf(buf, sizeof(buf), "%s: energy %d", rowName[r].c_str(), energy(r));
				lastEditRow = r; lastEditLane = -1;
			}
			lastEdit = buf;
			editCount++;
			return;
		}
	}

	void enter(int r) {
		cur = r;
		bar = 0;
		evChange = true;
		if (cued == r) cued = -1;
	}

	// ---- the clock ----
	// Call on every clock pulse. Returns true if anything is playing.
	bool tick() {
		evBeat = evBar = evPhrase = evChange = evEnd = evSixteenth = false;
		if (stopped) return false;
		if (cur < 0 || !playable(cur)) {
			if (!started) cur = firstPlayable();
			if (cur < 0) return false;
		}
		int ppb = pulsesPerBar();
		if (!started) {
			started = true;
			pulse = 0;
			bar = 0;
			evChange = true;
		}
		else {
			pulse++;
			if (pulse >= ppb) {
				pulse = 0;
				bar++;
				bool natural = !playable(cur) || bar >= bars(cur);
				int target = -2;      // -2 = stay
				if (nextRequested) {
					nextRequested = false;
					target = (cued >= 0 && playable(cued)) ? cued : -3;
				}
				else if (cued >= 0 && playable(cued) && (bar % std::max(1, cuePhrase) == 0 || natural)) {
					target = cued;
				}
				else if (natural) {
					if (hold && playable(cur)) { bar = 0; }
					else target = -3;
				}
				if (target != -2) {
					int leaving = cur;
					if (natural) driftEdit(leaving);
					if (target == -3) {
						bool ended = false;
						target = choose(&ended);
						if (ended) {
							evEnd = true;
							if (!loopForm) {
								stopped = true;
								cur = leaving;
								return false;
							}
						}
					}
					if (target < 0) return false;
					enter(target);
				}
			}
		}
		if (pulse == 0) {
			evBar = true;
			if (bar % std::max(1, phraseLen) == 0) evPhrase = true;
		}
		int p = std::max(1, ppqn);
		if (pulse % p == 0) evBeat = true;
		int six = std::max(1, p / 4);
		if (pulse % six == 0) evSixteenth = true;
		return true;
	}

	// ---- outputs ----
	bool active() const { return started && !stopped && cur >= 0 && playable(cur); }
	bool laneOn(int l) const { return active() && lane(cur, l); }
	float energyNow() const { return active() ? (float) energy(cur) : 0.f; }
	// 0..1 through the current section
	float progress() const {
		if (!active()) return 0.f;
		int total = bars(cur) * pulsesPerBar();
		return total > 0 ? std::min(1.f, (float) (bar * pulsesPerBar() + pulse) / total) : 0.f;
	}
	bool fill() const {
		if (!active()) return false;
		int total = bars(cur) * pulsesPerBar();
		int done = bar * pulsesPerBar() + pulse;
		return total - done <= fillPulses();
	}
	// what the display calls "next": cued, held, or the next in order
	int nextShown() const {
		if (cued >= 0) return cued;
		if (hold) return cur;
		bool w = false;
		return nextInOrder(cur, &w);
	}
};

}  // namespace lf
