// Tests for the Long Form and Mixxa engines (no VCV Rack needed).
//   c++ -std=c++11 -O2 -o test/engine_test test/engine_test.cpp && test/engine_test
#include "../src/LongFormEngine.hpp"
#include "../src/MixxaEngine.hpp"
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

using namespace lf;

// run whole bars and record which row each bar played
static void runBars(LongFormEngine& e, int bars, std::vector<int>* rows = nullptr) {
	for (int b = 0; b < bars; b++) {
		for (int p = 0; p < e.pulsesPerBar(); p++) {
			e.tick();
			if (p == 0 && rows) rows->push_back(e.cur);
		}
	}
}

static void testFormInOrder() {
	LongFormEngine e;
	std::vector<int> rows;
	int total = 0;
	for (int r = 0; r < ROWS; r++) total += e.bars(r);
	CHECK(total == 16 + 32 + 8 + 32 + 16 + 8 + 32 + 16);
	runBars(e, total + 1, &rows);
	// bar by bar it follows the rows in order
	int idx = 0;
	for (int r = 0; r < ROWS; r++)
		for (int b = 0; b < e.bars(r); b++) {
			CHECK(rows[idx] == r);
			idx++;
		}
	CHECK(rows[idx] == 0);   // loops back to the start
}

static void testEventsAndLanes() {
	LongFormEngine e;
	e.tick();
	CHECK(e.evChange && e.evBar && e.evBeat && e.evPhrase && e.evSixteenth);
	CHECK(e.laneOn(0) && e.laneOn(1) && !e.laneOn(2));   // Intro: kick + bass
	CHECK(e.energyNow() == 2.f);
	e.tick();
	CHECK(!e.evBeat && e.evSixteenth && !e.evBar);
	for (int i = 0; i < 3; i++) e.tick();
	CHECK(e.evBeat && !e.evBar);
	// fill is high in the last bar of the 16-bar intro
	LongFormEngine f;
	runBars(f, 14);
	f.tick();
	CHECK(!f.fill());
	for (int i = 0; i < 16; i++) f.tick();
	CHECK(f.fill());
	CHECK(f.cur == 0);
	for (int i = 0; i < 15; i++) f.tick();
	CHECK(f.cur == 0);
	f.tick();
	CHECK(f.cur == 1 && f.evChange);
	CHECK(f.progress() == 0.f);
}

static void testHoldNextCue() {
	LongFormEngine e;
	e.hold = true;
	std::vector<int> rows;
	runBars(e, 40, &rows);
	for (int r : rows) CHECK(r == 0);
	e.hold = false;
	e.requestNext();
	runBars(e, 1, &rows);
	CHECK(e.cur == 1);
	// cue row 5 (Break): starts on the next 4-bar line
	LongFormEngine c;
	runBars(c, 2);
	c.cue(4);
	runBars(c, 2);
	CHECK(c.cur == 0);
	runBars(c, 1);
	CHECK(c.cur == 4 && c.cued == -1);
}

static void testOffRowsAndStop() {
	LongFormEngine e;
	for (int r = 1; r < ROWS; r++) e.lenIndex[r] = 0;
	e.lenIndex[0] = 1;   // 1 bar
	e.loopForm = false;
	std::vector<int> rows;
	runBars(e, 1, &rows);
	CHECK(rows[0] == 0);
	runBars(e, 1);
	CHECK(e.stopped && !e.active());
	LongFormEngine none;
	for (int r = 0; r < ROWS; r++) none.lenIndex[r] = 0;
	none.reset();
	CHECK(!none.tick());
	CHECK(!none.laneOn(0));
}

static void testSteer() {
	// FOLLOW 0: the next row is picked by energy near STEER
	LongFormEngine e;
	e.follow = 0.f;
	e.steer = 9.f;
	int high = 0, low = 0;
	std::vector<int> rows;
	runBars(e, 4000, &rows);
	for (size_t i = 1; i < rows.size(); i++)
		if (rows[i] != rows[i - 1]) { if (e.energy(rows[i]) >= 7) high++; else low++; }
	CHECK(high > 3 * low);
}

static void testDrift() {
	LongFormEngine a, b;
	a.drift = b.drift = 1.f;
	std::vector<int> ra, rb;
	runBars(a, 600, &ra);
	runBars(b, 600, &rb);
	CHECK(ra == rb);              // same seed, same drift
	CHECK(a.editCount > 5);
	CHECK(!a.lastEdit.empty());
	for (int r = 0; r < ROWS; r++) {
		int bb = a.bars(r);
		CHECK(bb >= 1 && bb <= 64);
		CHECK(a.energy(r) >= 0 && a.energy(r) <= 10);
	}
	std::set<int> perm(a.order, a.order + ROWS);
	CHECK(perm.size() == (size_t) ROWS);
	a.restore();
	for (int r = 0; r < ROWS; r++) { CHECK(a.order[r] == r); CHECK(a.lenShift[r] == 0); }
	LongFormEngine l;
	l.drift = 1.f;
	l.lock = true;
	runBars(l, 300);
	CHECK(l.editCount == 0);
}

static void testPpqn24() {
	LongFormEngine e;
	e.ppqn = 24;
	int beats = 0, six = 0;
	for (int i = 0; i < 96; i++) { e.tick(); beats += e.evBeat; six += e.evSixteenth; }
	CHECK(beats == 4 && six == 16);
}

// ---------------------------------------------------------------- Mixxa
using namespace mx;

static void testMutesWaitForBeat() {
	MuteLogic m;
	m.pressMute(3);
	CHECK(!m.muted(3) && m.armed(3));
	m.quantum();
	CHECK(m.muted(3) && !m.armed(3));
	m.pressMute(3);
	m.pressMute(3);   // cancel
	m.quantum();
	CHECK(m.muted(3));
	m.pressSolo(2);
	CHECK(m.audible(4));
	m.quantum();
	CHECK(m.solo[2] && !m.audible(4) && m.audible(2));
}

static void testExternalAndOverride() {
	MuteLogic m;
	m.extPresent = true;
	m.setExternal(1, true, true, true);
	CHECK(m.muted(1));
	m.pressMute(1);
	m.quantum();
	CHECK(!m.muted(1) && m.overridden(1));
	m.sectionChange();
	CHECK(m.muted(1) && !m.overridden(1));
	m.manualStays = true;
	m.pressMute(1);
	m.quantum();
	m.sectionChange();
	CHECK(!m.muted(1));
	// ACT changes wait for the beat
	MuteLogic a;
	a.setExternal(5, true, false, true);
	CHECK(!a.muted(5));
	a.quantum();
	CHECK(a.muted(5));
}

static void testScenes() {
	MuteLogic m;
	m.pressMute(0); m.pressMute(7);
	m.quantum();
	m.storeScene(1);
	m.pressMute(0);
	m.quantum();
	CHECK(!m.muted(0));
	m.recallScene(1);
	CHECK(!m.muted(0));
	m.quantum();
	CHECK(m.muted(0) && m.muted(7) && !m.muted(3));
}

static void testRandom() {
	MuteLogic m;
	for (int i = 0; i < CH; i++) m.connected[i] = true;
	m.count = 3;
	for (int trial = 0; trial < 200; trial++) {
		uint8_t before = 0;
		for (int i = 0; i < CH; i++) if (m.muted(i)) before |= 1 << i;
		m.dice();
		m.quantum();
		int n = 0;
		uint8_t after = 0;
		for (int i = 0; i < CH; i++) if (m.muted(i)) { n++; after |= 1 << i; }
		CHECK(n == 3);
		if (trial > 0) CHECK(after != before);   // always a new set
	}
	// kick switch OUT keeps channel 1 playing
	MuteLogic k;
	for (int i = 0; i < CH; i++) k.connected[i] = true;
	k.includeKick = false;
	k.count = 7;
	for (int t = 0; t < 50; t++) { k.dice(); k.quantum(); CHECK(!k.muted(0)); }
	// only connected channels count, and BACK undoes the roll
	MuteLogic b;
	b.connected[2] = b.connected[4] = true;
	b.count = 8;
	b.dice();
	b.quantum();
	CHECK(b.muted(2) && b.muted(4) && !b.muted(3));
	b.back();
	b.quantum();
	CHECK(!b.muted(2) && !b.muted(4));
	// channels Long Form has muted are left alone
	MuteLogic e;
	for (int i = 0; i < CH; i++) e.connected[i] = true;
	e.setExternal(6, true, true, true);
	e.count = 0;
	e.dice();
	e.quantum();
	CHECK(e.muted(6));
}

static void testDsp() {
	CHECK(std::fabs(faderGain(0.7937f) - 1.f) < 0.01f);
	float gl, gr;
	panMono(0.f, &gl, &gr);
	CHECK(std::fabs(gl - 1.f) < 1e-3f && std::fabs(gr - 1.f) < 1e-3f);
	CHECK(Ducker::curve(0.f, 0.5f) == 1.f && Ducker::curve(1.f, 0.5f) == 0.f);
	CHECK(Ducker::curve(0.5f, 0.f) > Ducker::curve(0.5f, 1.f));   // gate stays down longer
	Ducker d;
	float dt = 1.f / 48000.f;
	d.process(10.f, dt, 0.16f, 0.5f);
	for (int i = 0; i < 480; i++) d.process(0.f, dt, 0.16f, 0.5f);
	CHECK(d.amount > 0.8f);
	for (int i = 0; i < 48000; i++) d.process(0.f, dt, 0.16f, 0.5f);
	CHECK(d.amount == 0.f);
	Glue g;
	float gain = 1.f;
	for (int i = 0; i < 48000; i++) gain = g.gainFor(5.f, 5.f, 1.f, dt);
	CHECK(g.grDb > 3.f && g.grDb < 5.f);
	CHECK(gain < 1.f);
	CHECK(g.gainFor(5.f, 5.f, 0.f, dt) == 1.f);
}

int main() {
	testFormInOrder();
	testEventsAndLanes();
	testHoldNextCue();
	testOffRowsAndStop();
	testSteer();
	testDrift();
	testPpqn24();
	testMutesWaitForBeat();
	testExternalAndOverride();
	testScenes();
	testRandom();
	testDsp();
	if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
	std::printf("all engine tests passed\n");
	return 0;
}
