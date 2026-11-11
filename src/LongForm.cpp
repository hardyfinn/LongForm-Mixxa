// LongForm.cpp
// The arrangement conductor. The engine (LongFormEngine.hpp) does the
// counting and choosing; this file connects it to the panel, the jacks and
// the display. Control positions come from Layout.hpp (res/make_panels.py).
#include "plugin.hpp"
#include "LongFormEngine.hpp"
#include "Layout.hpp"

using namespace lf;

static const int PPQN_CHOICES[4] = {4, 24, 2, 1};
static const int CUE_PHRASE_CHOICES[4] = {1, 2, 4, 8};
static const int PHRASE_CHOICES[4] = {2, 4, 8, 16};

struct LenQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		int i = clamp((int) std::round(getValue()), 0, NUM_LENS - 1);
		return LEN_BARS[i] == 0 ? "Off (skipped)" : string::f("%d bar%s", LEN_BARS[i], LEN_BARS[i] == 1 ? "" : "s");
	}
};

struct FillQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		static const char* n[4] = {"1/2 bar", "1 bar", "2 bars", "4 bars"};
		return n[clamp((int) std::round(getValue()), 0, 3)];
	}
};

struct LongForm : Module {
	enum ParamId {
		ENUMS(LEN_PARAM, ROWS),
		ENUMS(ENERGY_PARAM, ROWS),
		STEER_PARAM, FOLLOW_PARAM, DRIFT_PARAM,
		HOLD_PARAM, NEXT_PARAM, LOCK_PARAM, RESTORE_PARAM,
		GLIDE_PARAM, FILL_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT, RESET_INPUT, RUN_INPUT, HOLD_INPUT, NEXT_INPUT, CUE_INPUT,
		STEER_INPUT, FOLLOW_INPUT, DRIFT_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(LANE_OUTPUT, LANES),
		ACT_OUTPUT, ENERGY_OUTPUT, RAMP_OUTPUT, FILL_OUTPUT, CHANGE_OUTPUT, BAR_OUTPUT, PHRASE_OUTPUT, END_OUTPUT,
		LINK_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId { HOLD_LIGHT, NEXT_LIGHT, LOCK_LIGHT, RESTORE_LIGHT, LIGHTS_LEN };

	LongFormEngine e;
	int ppqnIndex = 0, cuePhraseIndex = 2, phraseIndex = 1;
	bool loopForm = true;

	dsp::SchmittTrigger clockTrig, resetTrig, nextTrig;
	dsp::BooleanTrigger nextBtn, restoreBtn;
	dsp::PulseGenerator pChange, pBar, pPhrase, pEnd, pBeat, pSix, pReset, nextFlash, restoreFlash;
	dsp::ClockDivider lightDiv;
	int lastCueRow = -1;
	double samplesSincePulse = 0.0, pulseSamples = 0.0;
	float energyOut = 0.f, energyRate = 0.f, energyTarget = -1.f;
	float sampleRate = 48000.f;

	LongForm() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int r = 0; r < ROWS; r++) {
			configParam<LenQuantity>(LEN_PARAM + r, 0.f, NUM_LENS - 1, DEFAULT_LEN_INDEX[r], string::f("Section %d length", r + 1));
			getParamQuantity(LEN_PARAM + r)->snapEnabled = true;
			configParam(ENERGY_PARAM + r, 0.f, 10.f, DEFAULT_ENERGY[r], string::f("Section %d energy", r + 1));
			getParamQuantity(ENERGY_PARAM + r)->snapEnabled = true;
		}
		configParam(STEER_PARAM, 0.f, 10.f, 5.f, "STEER: the energy you want next (used when FOLLOW lets it choose)");
		configParam(FOLLOW_PARAM, 0.f, 1.f, 1.f, "FOLLOW: chance of playing the next section in order", "%", 0.f, 100.f);
		configParam(DRIFT_PARAM, 0.f, 1.f, 0.f, "DRIFT: chance of one small edit to the form after each section", "%", 0.f, 100.f);
		configSwitch(HOLD_PARAM, 0.f, 1.f, 0.f, "HOLD: repeat this section", {"Off", "On"});
		configButton(NEXT_PARAM, "NEXT: move on at the next bar line");
		configSwitch(LOCK_PARAM, 0.f, 1.f, 0.f, "LOCK: no drift edits", {"Off", "On"});
		configButton(RESTORE_PARAM, "RESTORE: undo every drift edit");
		configParam(GLIDE_PARAM, 0.f, 8.f, 0.f, "Energy glide", " bars");
		configParam<FillQuantity>(FILL_PARAM, 0.f, 3.f, 1.f, "FILL gate length");
		getParamQuantity(FILL_PARAM)->snapEnabled = true;
		configInput(CLOCK_INPUT, "Clock (16th notes; other rates in the right-click menu)");
		configInput(RESET_INPUT, "Reset: back to the first section, bar 1");
		configInput(RUN_INPUT, "Run gate: counts while high (unpatched: always)");
		configInput(HOLD_INPUT, "Hold gate");
		configInput(NEXT_INPUT, "Next trigger");
		configInput(CUE_INPUT, "Cue CV: 0 to 8 V picks a section (1 V each)");
		configInput(STEER_INPUT, "STEER CV (0 to 10 V adds the full range)");
		configInput(FOLLOW_INPUT, "FOLLOW CV (0 to 10 V adds 0 to 100%)");
		configInput(DRIFT_INPUT, "DRIFT CV (0 to 10 V adds 0 to 100%)");
		for (int l = 0; l < LANES; l++) configOutput(LANE_OUTPUT + l, string::f("Lane %d gate", l + 1));
		configOutput(ACT_OUTPUT, "ACT (poly, 8 lanes: 10 V = play). Channels 1-6 match Francois Markovian A-F");
		configOutput(ENERGY_OUTPUT, "Energy (0 to 10 V, 1 V per step)");
		configOutput(RAMP_OUTPUT, "Ramp: 0 to 10 V over each section");
		configOutput(FILL_OUTPUT, "Fill gate: high near the end of each section");
		configOutput(CHANGE_OUTPUT, "Change trigger: a new section starts");
		configOutput(BAR_OUTPUT, "Bar trigger");
		configOutput(PHRASE_OUTPUT, "Phrase trigger");
		configOutput(END_OUTPUT, "End trigger: the form finished");
		configOutput(LINK_OUTPUT, "LINK to Mixxa (poly: lanes, beat, change, energy, reset, 16ths)");
		lightDiv.setDivision(32);
		e.seed = random::u64();
		e.reset();
	}

	void onSampleRateChange(const SampleRateChangeEvent& ev) override { sampleRate = ev.sampleRate; }

	void onReset(const ResetEvent& ev) override {
		Module::onReset(ev);
		e.init();
		ppqnIndex = 0; cuePhraseIndex = 2; phraseIndex = 1; loopForm = true;
		e.seed = random::u64();
		e.reset();
	}

	float cv(int id, float scale) { return inputs[id].isConnected() ? inputs[id].getVoltage() * scale : 0.f; }

	void process(const ProcessArgs& args) override {
		// ---- settings ----
		for (int r = 0; r < ROWS; r++) {
			e.lenIndex[r] = (int) std::round(params[LEN_PARAM + r].getValue());
			e.energyKnob[r] = (int) std::round(params[ENERGY_PARAM + r].getValue());
		}
		e.steer = clamp(params[STEER_PARAM].getValue() + cv(STEER_INPUT, 1.f), 0.f, 10.f);
		e.follow = clamp(params[FOLLOW_PARAM].getValue() + cv(FOLLOW_INPUT, 0.1f), 0.f, 1.f);
		e.drift = clamp(params[DRIFT_PARAM].getValue() + cv(DRIFT_INPUT, 0.1f), 0.f, 1.f);
		e.hold = params[HOLD_PARAM].getValue() > 0.5f || inputs[HOLD_INPUT].getVoltage() > 1.f;
		e.lock = params[LOCK_PARAM].getValue() > 0.5f;
		e.ppqn = PPQN_CHOICES[ppqnIndex];
		e.cuePhrase = CUE_PHRASE_CHOICES[cuePhraseIndex];
		e.phraseLen = PHRASE_CHOICES[phraseIndex];
		e.fillIndex = (int) std::round(params[FILL_PARAM].getValue());
		e.loopForm = loopForm;

		// ---- buttons and gates ----
		if (nextBtn.process(params[NEXT_PARAM].getValue() > 0.f) | nextTrig.process(inputs[NEXT_INPUT].getVoltage(), 0.1f, 1.f)) {
			e.requestNext();
			nextFlash.trigger(0.2f);
		}
		if (restoreBtn.process(params[RESTORE_PARAM].getValue() > 0.f)) {
			e.restore();
			restoreFlash.trigger(0.2f);
		}
		if (inputs[CUE_INPUT].isConnected()) {
			int row = clamp((int) std::floor(inputs[CUE_INPUT].getVoltage()), 0, ROWS - 1);
			if (row != lastCueRow) { e.cueSet(row); lastCueRow = row; }
		}
		else lastCueRow = -1;

		// ---- clock ----
		if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
			e.reset();
			pReset.trigger(1e-3f);
		}
		samplesSincePulse += 1.0;
		bool running = !inputs[RUN_INPUT].isConnected() || inputs[RUN_INPUT].getVoltage() > 1.f;
		if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (samplesSincePulse < args.sampleRate * 2.0) pulseSamples = samplesSincePulse;
			samplesSincePulse = 0.0;
			if (running) {
				e.tick();
				if (e.evChange) pChange.trigger(1e-3f);
				if (e.evBar) pBar.trigger(1e-3f);
				if (e.evPhrase) pPhrase.trigger(1e-3f);
				if (e.evEnd) pEnd.trigger(1e-3f);
				if (e.evBeat) pBeat.trigger(1e-3f);
				if (e.evSixteenth) pSix.trigger(1e-3f);
			}
		}

		// ---- energy with glide ----
		float target = e.energyNow();
		if (target != energyTarget) {
			energyTarget = target;
			double beatSec = pulseSamples > 0.0 ? pulseSamples * e.ppqn / args.sampleRate : 0.5;
			float glideSec = params[GLIDE_PARAM].getValue() * 4.f * (float) beatSec;
			energyRate = glideSec > 1e-4f ? std::fabs(target - energyOut) / glideSec : 1e9f;
		}
		float stepV = energyRate * args.sampleTime;
		if (energyOut < energyTarget) energyOut = std::min(energyTarget, energyOut + stepV);
		else if (energyOut > energyTarget) energyOut = std::max(energyTarget, energyOut - stepV);

		// ---- outputs ----
		float dt = args.sampleTime;
		bool change = pChange.process(dt), bar = pBar.process(dt), phrase = pPhrase.process(dt), end = pEnd.process(dt);
		bool beat = pBeat.process(dt), six = pSix.process(dt), rst = pReset.process(dt);
		outputs[ACT_OUTPUT].setChannels(LANES);
		for (int l = 0; l < LANES; l++) {
			float v = e.laneOn(l) ? 10.f : 0.f;
			outputs[LANE_OUTPUT + l].setVoltage(v);
			outputs[ACT_OUTPUT].setVoltage(v, l);
		}
		outputs[ENERGY_OUTPUT].setVoltage(energyOut);
		outputs[RAMP_OUTPUT].setVoltage(e.progress() * 10.f);
		outputs[FILL_OUTPUT].setVoltage(e.fill() ? 10.f : 0.f);
		outputs[CHANGE_OUTPUT].setVoltage(change ? 10.f : 0.f);
		outputs[BAR_OUTPUT].setVoltage(bar ? 10.f : 0.f);
		outputs[PHRASE_OUTPUT].setVoltage(phrase ? 10.f : 0.f);
		outputs[END_OUTPUT].setVoltage(end ? 10.f : 0.f);
		// LINK: 1-8 lanes, 9 beat, 10 change, 11 energy, 12 reset, 13 sixteenths
		Output& link = outputs[LINK_OUTPUT];
		link.setChannels(13);
		for (int l = 0; l < LANES; l++) link.setVoltage(e.laneOn(l) ? 10.f : 0.f, l);
		link.setVoltage(beat ? 10.f : 0.f, 8);
		link.setVoltage(change ? 10.f : 0.f, 9);
		link.setVoltage(energyOut, 10);
		link.setVoltage(rst ? 10.f : 0.f, 11);
		link.setVoltage(six ? 10.f : 0.f, 12);

		if (lightDiv.process()) {
			float ldt = dt * lightDiv.getDivision();
			lights[HOLD_LIGHT].setBrightness(e.hold ? 1.f : 0.f);
			lights[LOCK_LIGHT].setBrightness(e.lock ? 1.f : 0.f);
			lights[NEXT_LIGHT].setBrightness(nextFlash.process(ldt) || e.nextRequested ? 1.f : 0.f);
			lights[RESTORE_LIGHT].setBrightness(restoreFlash.process(ldt) ? 1.f : 0.f);
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		std::string masks, flips;
		for (int r = 0; r < ROWS; r++)
			for (int l = 0; l < LANES; l++) { masks += e.mask[r][l] ? '1' : '0'; flips += e.flip[r][l] ? '1' : '0'; }
		json_object_set_new(root, "masks", json_string(masks.c_str()));
		json_object_set_new(root, "flips", json_string(flips.c_str()));
		json_t* rows = json_array();
		json_t* lanes = json_array();
		json_t* ord = json_array();
		json_t* ls = json_array();
		json_t* es = json_array();
		for (int r = 0; r < ROWS; r++) {
			json_array_append_new(rows, json_string(e.rowName[r].c_str()));
			json_array_append_new(ord, json_integer(e.order[r]));
			json_array_append_new(ls, json_integer(e.lenShift[r]));
			json_array_append_new(es, json_integer(e.energyShift[r]));
		}
		for (int l = 0; l < LANES; l++) json_array_append_new(lanes, json_string(e.laneName[l].c_str()));
		json_object_set_new(root, "rowNames", rows);
		json_object_set_new(root, "laneNames", lanes);
		json_object_set_new(root, "order", ord);
		json_object_set_new(root, "lenShift", ls);
		json_object_set_new(root, "energyShift", es);
		json_object_set_new(root, "lastEdit", json_string(e.lastEdit.c_str()));
		json_object_set_new(root, "seed", json_string(string::f("%llu", (unsigned long long) e.seed).c_str()));
		json_object_set_new(root, "ppqnIndex", json_integer(ppqnIndex));
		json_object_set_new(root, "cuePhraseIndex", json_integer(cuePhraseIndex));
		json_object_set_new(root, "phraseIndex", json_integer(phraseIndex));
		json_object_set_new(root, "loopForm", json_boolean(loopForm));
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "masks")) && json_is_string(j)) {
			std::string s = json_string_value(j);
			for (int i = 0; i < ROWS * LANES && i < (int) s.size(); i++) e.mask[i / LANES][i % LANES] = s[i] == '1';
		}
		if ((j = json_object_get(root, "flips")) && json_is_string(j)) {
			std::string s = json_string_value(j);
			for (int i = 0; i < ROWS * LANES && i < (int) s.size(); i++) e.flip[i / LANES][i % LANES] = s[i] == '1';
		}
		if ((j = json_object_get(root, "rowNames")))
			for (int r = 0; r < ROWS; r++) { json_t* v = json_array_get(j, r); if (v && json_is_string(v)) e.rowName[r] = json_string_value(v); }
		if ((j = json_object_get(root, "laneNames")))
			for (int l = 0; l < LANES; l++) { json_t* v = json_array_get(j, l); if (v && json_is_string(v)) e.laneName[l] = json_string_value(v); }
		if ((j = json_object_get(root, "order"))) {
			int tmp[ROWS];
			bool seen[ROWS] = {};
			bool ok = json_array_size(j) == (size_t) ROWS;
			for (int r = 0; ok && r < ROWS; r++) {
				tmp[r] = (int) json_integer_value(json_array_get(j, r));
				if (tmp[r] < 0 || tmp[r] >= ROWS || seen[tmp[r]]) ok = false;
				else seen[tmp[r]] = true;
			}
			if (ok) for (int r = 0; r < ROWS; r++) e.order[r] = tmp[r];
		}
		if ((j = json_object_get(root, "lenShift")))
			for (int r = 0; r < ROWS; r++) e.lenShift[r] = clamp((int) json_integer_value(json_array_get(j, r)), -6, 6);
		if ((j = json_object_get(root, "energyShift")))
			for (int r = 0; r < ROWS; r++) e.energyShift[r] = clamp((int) json_integer_value(json_array_get(j, r)), -10, 10);
		if ((j = json_object_get(root, "lastEdit")) && json_is_string(j)) e.lastEdit = json_string_value(j);
		if ((j = json_object_get(root, "seed")) && json_is_string(j)) e.seed = std::strtoull(json_string_value(j), nullptr, 10);
		if ((j = json_object_get(root, "ppqnIndex"))) ppqnIndex = clamp((int) json_integer_value(j), 0, 3);
		if ((j = json_object_get(root, "cuePhraseIndex"))) cuePhraseIndex = clamp((int) json_integer_value(j), 0, 3);
		if ((j = json_object_get(root, "phraseIndex"))) phraseIndex = clamp((int) json_integer_value(j), 0, 3);
		if ((j = json_object_get(root, "loopForm"))) loopForm = json_is_true(j);
		e.reset();
	}
};

// ---- colours ----
static const NVGcolor C_BG = nvgRGB(0x1D, 0x1E, 0x22);
static const NVGcolor C_INK = nvgRGB(0xF3, 0xEF, 0xE8);
static const NVGcolor C_SOFT = nvgRGB(0xB9, 0xB3, 0xA9);
static const NVGcolor C_ORANGE = nvgRGB(0xEF, 0x7A, 0x37);
static const NVGcolor C_CYAN = nvgRGB(0x3F, 0xD0, 0xE0);
static const NVGcolor C_AMBER = nvgRGB(0xF0, 0xB0, 0x30);
static const NVGcolor C_OFF = nvgRGB(0x2C, 0x2C, 0x2F);
static const NVGcolor C_EDGE = nvgRGB(0x18, 0x18, 0x1A);

static void setFont(NVGcontext* vg) {
	std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
	if (font && font->handle >= 0) nvgFontFaceId(vg, font->handle);
}

// ---- the form strip and status line ----
struct FormDisplay : Widget {
	LongForm* module = nullptr;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { Widget::drawLayer(args, layer); return; }
		NVGcontext* vg = args.vg;
		float W = box.size.x, Hh = box.size.y;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, W, Hh, mm2px(1.2f));
		nvgFillColor(vg, C_BG);
		nvgFill(vg);
		setFont(vg);
		static LongFormEngine preview;
		LongFormEngine& e = module ? module->e : preview;

		float pad = mm2px(2.f), by = mm2px(2.f), bh = mm2px(5.f);
		int total = 0;
		for (int p = 0; p < ROWS; p++) total += e.bars(e.order[p]);
		if (total <= 0) {
			nvgFontSize(vg, mm2px(3.f));
			nvgFillColor(vg, C_SOFT);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgText(vg, pad, Hh / 2, "every section is Off: turn up a LEN knob", NULL);
			return;
		}
		float bw = W - 2 * pad, x = pad, headX = -1.f;
		nvgFontSize(vg, mm2px(2.3f));
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		for (int p = 0; p < ROWS; p++) {
			int r = e.order[p];
			int b = e.bars(r);
			if (b <= 0) continue;
			float ww = bw * b / total;
			float a = 0.25f + e.energy(r) * 0.075f;
			nvgBeginPath(vg);
			nvgRoundedRect(vg, x + 0.6f, by, ww - 1.2f, bh, 1.5f);
			nvgFillColor(vg, nvgTransRGBAf(C_ORANGE, a));
			nvgFill(vg);
			if (r == e.cued) {
				nvgStrokeColor(vg, C_CYAN);
				nvgStrokeWidth(vg, 1.3f);
				nvgStroke(vg);
			}
			if (ww > mm2px(14.f)) {
				nvgFillColor(vg, C_BG);
				nvgText(vg, x + ww / 2, by + bh / 2, e.rowName[r].c_str(), NULL);
			}
			if (r == e.cur && e.active()) headX = x + ww * e.progress();
			x += ww;
		}
		if (headX >= 0.f) {
			nvgBeginPath(vg);
			nvgMoveTo(vg, headX, by - 2);
			nvgLineTo(vg, headX, by + bh + 2);
			nvgStrokeColor(vg, nvgRGB(255, 255, 255));
			nvgStrokeWidth(vg, 1.5f);
			nvgStroke(vg);
		}

		float ty = mm2px(11.6f);
		nvgFontSize(vg, mm2px(3.f));
		std::string left;
		if (!module) left = "LONG FORM";
		else if (e.stopped) left = "END: send a reset to start again";
		else if (!e.started) left = "waiting for the clock";
		else if (e.active()) left = string::f("%s  ·  bar %d of %d", e.rowName[e.cur].c_str(), e.bar + 1, e.bars(e.cur));
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, C_INK);
		nvgText(vg, pad, ty, left.c_str(), NULL);

		std::string mid;
		NVGcolor mc = C_SOFT;
		if (module && e.started && !e.stopped) {
			if (e.cued >= 0) { mid = string::f("next: %s (cued)", e.rowName[e.cued].c_str()); mc = C_CYAN; }
			else if (e.hold) { mid = "HOLD: repeating"; mc = C_AMBER; }
			else if (e.follow >= 0.999f) { int n = e.nextShown(); if (n >= 0) mid = string::f("next: %s", e.rowName[n].c_str()); }
			else mid = string::f("next: steering to energy %.0f", e.steer);
		}
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, mc);
		nvgText(vg, W / 2 + mm2px(6.f), ty, mid.c_str(), NULL);

		if (!e.lastEdit.empty()) {
			std::string right = "drift: " + e.lastEdit;
			nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, C_SOFT);
			nvgText(vg, W - pad, ty, right.c_str(), NULL);
		}
	}
};

// ---- the 8 x 8 grid with the CUE buttons ----
struct FormGrid : OpaqueWidget {
	LongForm* module = nullptr;
	uint32_t seenEdits = 0;
	float flash = 0.f;
	float t = 0.f;
	// positions relative to this widget, in px
	float cueX, gx0, gy0, pitch, rp, left, top;

	void setup() {
		left = mm2px(lfl::GRID[0] - 4.5f);
		top = mm2px(lfl::GRID[2] - lfl::GRID[4] / 2);
		cueX = mm2px(lfl::GRID[0]) - left;
		gx0 = mm2px(lfl::GRID[1]) - left;
		gy0 = mm2px(lfl::GRID[2]) - top;
		pitch = mm2px(lfl::GRID[3]);
		rp = mm2px(lfl::GRID[4]);
		box.pos = Vec(left, top);
		box.size = Vec(gx0 + 7 * pitch + pitch / 2, rp * ROWS);
	}

	void step() override {
		OpaqueWidget::step();
		float dt = APP->window->getLastFrameDuration();
		if (!std::isfinite(dt) || dt <= 0.f) dt = 1.f / 60.f;
		t += dt;
		if (module && module->e.editCount != seenEdits) { seenEdits = module->e.editCount; flash = 1.5f; }
		if (flash > 0.f) flash -= dt;
	}

	void onButton(const ButtonEvent& ev) override {
		if (ev.action == GLFW_PRESS && ev.button == GLFW_MOUSE_BUTTON_LEFT && module) {
			int r = (int) std::floor(ev.pos.y / rp);
			if (r >= 0 && r < ROWS) {
				if (std::fabs(ev.pos.x - cueX) < mm2px(3.5f)) { module->e.cue(r); ev.consume(this); return; }
				int c = (int) std::floor((ev.pos.x - gx0 + pitch / 2) / pitch);
				if (c >= 0 && c < LANES && std::fabs(ev.pos.x - (gx0 + c * pitch)) < pitch / 2) {
					module->e.toggleLane(r, c);
					ev.consume(this);
					return;
				}
			}
		}
		OpaqueWidget::onButton(ev);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
		NVGcontext* vg = args.vg;
		static LongFormEngine preview;
		LongFormEngine& e = module ? module->e : preview;
		float half = mm2px(2.9f), cr = mm2px(2.4f);
		bool blink = std::fmod(t, 0.5f) < 0.25f;
		for (int r = 0; r < ROWS; r++) {
			float y = gy0 + r * rp;
			bool play = e.playable(r);
			// cue button
			NVGcolor cc = C_OFF;
			if (module && r == e.cued) cc = blink ? C_CYAN : nvgTransRGBAf(C_CYAN, 0.35f);
			else if (module && r == e.cur && e.active()) cc = C_INK;
			nvgBeginPath(vg);
			nvgCircle(vg, cueX, y, cr + 1.5f);
			nvgFillColor(vg, C_EDGE);
			nvgFill(vg);
			nvgBeginPath(vg);
			nvgCircle(vg, cueX, y, cr);
			nvgFillColor(vg, cc);
			nvgFill(vg);
			for (int l = 0; l < LANES; l++) {
				float x = gx0 + l * pitch;
				bool on = e.lane(r, l);
				NVGcolor col = on ? C_ORANGE : C_OFF;
				if (on && !play) col = nvgTransRGBAf(C_ORANGE, 0.35f);
				if (on && module && r == e.cur && e.active()) col = nvgRGB(0xFF, 0x96, 0x55);
				if (flash > 0.f && r == e.lastEditRow && l == e.lastEditLane && std::fmod(flash, 0.3f) < 0.15f) col = C_AMBER;
				nvgBeginPath(vg);
				nvgRoundedRect(vg, x - half, y - half, 2 * half, 2 * half, 2.f);
				nvgFillColor(vg, C_EDGE);
				nvgFill(vg);
				nvgBeginPath(vg);
				nvgRoundedRect(vg, x - half + 1.8f, y - half + 1.8f, 2 * half - 3.6f, 2 * half - 3.6f, 1.4f);
				nvgFillColor(vg, col);
				nvgFill(vg);
				if (e.flip[r][l]) {   // drift-edited cells get a small corner mark
					nvgBeginPath(vg);
					nvgCircle(vg, x + half - 3.f, y - half + 3.f, 1.4f);
					nvgFillColor(vg, C_AMBER);
					nvgFill(vg);
				}
			}
		}
	}
};

struct NameField : ui::TextField {
	std::string* target = nullptr;
	void onChange(const ChangeEvent& ev) override {
		if (target) *target = text.substr(0, 16);
	}
};

struct LongFormWidget : ModuleWidget {
	LongFormWidget(LongForm* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/LongForm.svg")));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		using namespace lfl;
		using L = LongForm;

		FormDisplay* d = createWidget<FormDisplay>(mm2px(Vec(DISPLAY[0], DISPLAY[1])));
		d->box.size = mm2px(Vec(DISPLAY[2], DISPLAY[3]));
		d->module = module;
		addChild(d);

		FormGrid* g = new FormGrid;
		g->module = module;
		g->setup();
		addChild(g);

		for (int r = 0; r < ROWS; r++) {
			float y = GRID[2] + r * GRID[4];
			addParam(createParamCentered<Trimpot>(mm2px(Vec(LEN_X, y)), module, L::LEN_PARAM + r));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(ENERGY_X, y)), module, L::ENERGY_PARAM + r));
		}
		const int big[3] = {L::STEER_PARAM, L::FOLLOW_PARAM, L::DRIFT_PARAM};
		const int bigCv[3] = {L::STEER_INPUT, L::FOLLOW_INPUT, L::DRIFT_INPUT};
		for (int i = 0; i < 3; i++) {
			addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(BIG_X[i], BIG_Y)), module, big[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(BIG_X[i] + BIG_CV_DX, BIG_Y + 1.5f)), module, bigCv[i]));
		}
		addParam(createLightParamCentered<VCVLightBezelLatch<WhiteLight>>(mm2px(Vec(ROW2_X[0], ROW2_Y)), module, L::HOLD_PARAM, L::HOLD_LIGHT));
		addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(ROW2_X[1], ROW2_Y)), module, L::NEXT_PARAM, L::NEXT_LIGHT));
		addParam(createLightParamCentered<VCVLightBezelLatch<WhiteLight>>(mm2px(Vec(ROW2_X[2], ROW2_Y)), module, L::LOCK_PARAM, L::LOCK_LIGHT));
		addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(ROW2_X[3], ROW2_Y)), module, L::RESTORE_PARAM, L::RESTORE_LIGHT));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(ROW2_X[4], ROW2_Y + 0.5f)), module, L::GLIDE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(ROW2_X[5], ROW2_Y + 0.5f)), module, L::FILL_PARAM));
		const int ins[6] = {L::CLOCK_INPUT, L::RESET_INPUT, L::RUN_INPUT, L::HOLD_INPUT, L::NEXT_INPUT, L::CUE_INPUT};
		for (int i = 0; i < 6; i++) addInput(createInputCentered<PJ301MPort>(mm2px(Vec(ROW2_X[i], IN_Y)), module, ins[i]));
		for (int l = 0; l < LANES; l++) addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(OUT_X[l], OUT_Y[0])), module, L::LANE_OUTPUT + l));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(OUT_X[8], OUT_Y[0])), module, L::ACT_OUTPUT));
		const int outs[7] = {L::ENERGY_OUTPUT, L::RAMP_OUTPUT, L::FILL_OUTPUT, L::CHANGE_OUTPUT, L::BAR_OUTPUT, L::PHRASE_OUTPUT, L::END_OUTPUT};
		for (int i = 0; i < 7; i++) addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(OUT_X[i], OUT_Y[1])), module, outs[i]));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(OUT_X[8], OUT_Y[1])), module, L::LINK_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		LongForm* m = getModule<LongForm>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Clock input", {"16th notes (4 per beat)", "24 PPQN (MIDI clock)", "8th notes", "Quarter notes"}, &m->ppqnIndex));
		menu->addChild(createIndexPtrSubmenuItem("Cued sections start on", {"The next bar", "Every 2 bars", "Every 4 bars", "Every 8 bars"}, &m->cuePhraseIndex));
		menu->addChild(createIndexPtrSubmenuItem("PHRASE output every", {"2 bars", "4 bars", "8 bars", "16 bars"}, &m->phraseIndex));
		menu->addChild(createBoolPtrMenuItem("Loop the form (off: stop at the end)", "", &m->loopForm));
		menu->addChild(createSubmenuItem("Section names", "", [=](Menu* sub) {
			for (int r = 0; r < ROWS; r++) {
				NameField* f = new NameField;
				f->box.size.x = 160;
				f->text = m->e.rowName[r];
				f->target = &m->e.rowName[r];
				sub->addChild(createMenuLabel(string::f("Section %d", r + 1)));
				sub->addChild(f);
			}
		}));
		menu->addChild(createSubmenuItem("Lane names", "", [=](Menu* sub) {
			for (int l = 0; l < LANES; l++) {
				NameField* f = new NameField;
				f->box.size.x = 160;
				f->text = m->e.laneName[l];
				f->target = &m->e.laneName[l];
				sub->addChild(createMenuLabel(string::f("Lane %d", l + 1)));
				sub->addChild(f);
			}
		}));
		menu->addChild(createMenuItem("New drift seed", "", [=]() { m->e.seed = random::u64(); m->e.rng.seed(m->e.seed * 0x2545F4914F6CDD1Dull + 7); }));
		menu->addChild(createMenuItem("Clear the grid", "", [=]() {
			for (int r = 0; r < ROWS; r++) for (int l = 0; l < LANES; l++) { m->e.mask[r][l] = false; m->e.flip[r][l] = false; }
		}));
	}
};

Model* modelLongForm = createModel<LongForm, LongFormWidget>("LongForm");
