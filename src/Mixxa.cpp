// Mixxa.cpp
// An 8-channel performance mixer: every mute, solo, scene and random roll
// lands on the next beat. Mute logic and DSP helpers are in MixxaEngine.hpp;
// positions come from Layout.hpp (res/make_panels.py).
#include "plugin.hpp"
#include "MixxaEngine.hpp"
#include "Layout.hpp"

using namespace mx;

static const int PPQN_CHOICES[4] = {4, 24, 2, 1};
static const float FADE_MS[5] = {2.f, 4.f, 8.f, 12.f, 20.f};

struct Mixxa : Module {
	enum ParamId {
		ENUMS(LEVEL_PARAM, CH),
		ENUMS(PAN_PARAM, CH),
		ENUMS(DUCK_PARAM, CH),
		ENUMS(MUTE_PARAM, CH),
		ENUMS(SOLO_PARAM, CH),
		DICE_PARAM, COUNT_PARAM, BACK_PARAM, KICK_SWITCH_PARAM,
		ENUMS(SCENE_PARAM, SCENES),
		RELEASE_PARAM, SHAPE_PARAM, GLUE_PARAM, MASTER_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		ENUMS(IN_L_INPUT, CH),
		ENUMS(IN_R_INPUT, CH),
		TRIG_INPUT, COUNT_INPUT, SCENE_INPUT, KICK_INPUT, CLOCK_INPUT, ACT_INPUT, LINK_INPUT,
		INPUTS_LEN
	};
	enum OutputId { OUT_L_OUTPUT, OUT_R_OUTPUT, STEMS_OUTPUT, OUTPUTS_LEN };
	enum LightId {
		ENUMS(MUTE_LIGHT, CH * 3),
		ENUMS(SOLO_LIGHT, CH),
		ENUMS(METER_LIGHT, CH * 6),
		ENUMS(DICE_LIGHT, 3),
		BACK_LIGHT,
		ENUMS(SCENE_LIGHT, SCENES),
		ENUMS(GR_LIGHT, 6),
		ENUMS(OUT_LIGHT, 16),
		LIGHTS_LEN
	};

	MuteLogic m;
	Fade fade[CH];
	Ducker duck;
	Glue glue;
	float meter[CH] = {}, outMeter[2] = {};

	int timing = NEXT_BEAT;
	int ppqnIndex = 0;
	int fadeIndex = 1;

	dsp::BooleanTrigger muteBtn[CH], soloBtn[CH], diceBtn, backBtn;
	dsp::SchmittTrigger trigIn, clockIn, linkBeat, linkSix, linkChange;
	bool scenePressed[SCENES] = {};
	float sceneHeld[SCENES] = {};
	bool sceneDone[SCENES] = {};
	int lastSceneCv = -1;
	int pulseCount = 0;
	dsp::ClockDivider lightDiv;
	float blink = 0.f;

	Mixxa() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int i = 0; i < CH; i++) {
			configParam(LEVEL_PARAM + i, 0.f, 1.f, 0.7937f, string::f("Channel %d level", i + 1), " dB", -10.f, 60.f, 6.0206f);
			configParam(PAN_PARAM + i, -1.f, 1.f, 0.f, string::f("Channel %d pan", i + 1), "%", 0.f, 100.f);
			configParam(DUCK_PARAM + i, 0.f, 1.f, 0.f, i == 0 ? "Channel 1 is the kick: it never ducks" : string::f("Channel %d duck depth", i + 1), "%", 0.f, 100.f);
			configButton(MUTE_PARAM + i, string::f("Channel %d mute (on the next beat)", i + 1));
			configButton(SOLO_PARAM + i, string::f("Channel %d solo (on the next beat)", i + 1));
			configInput(IN_L_INPUT + i, string::f("Channel %d left (mono)", i + 1));
			configInput(IN_R_INPUT + i, string::f("Channel %d right", i + 1));
		}
		configButton(DICE_PARAM, "DICE: mute a random set of channels on the next beat");
		configParam(COUNT_PARAM, 0.f, 8.f, 2.f, "COUNT: how many channels a roll mutes");
		getParamQuantity(COUNT_PARAM)->snapEnabled = true;
		configButton(BACK_PARAM, "BACK: return to the mutes from before the last roll");
		configSwitch(KICK_SWITCH_PARAM, 0.f, 1.f, 1.f, "Kick (channel 1) in random mutes", {"OUT: never picked", "IN: can be picked"});
		for (int s = 0; s < SCENES; s++) configButton(SCENE_PARAM + s, string::f("Scene %c: tap to recall on the next beat, hold 1 s to store", 'A' + s));
		configParam(RELEASE_PARAM, 0.04f, 0.4f, 0.16f, "Duck release", " ms", 0.f, 1000.f);
		configParam(SHAPE_PARAM, 0.f, 1.f, 0.5f, "Duck shape (gate, pump, swell)");
		configParam(GLUE_PARAM, 0.f, 1.f, 0.f, "GLUE: 2:1 bus compressor with make-up gain", "%", 0.f, 100.f);
		configParam(MASTER_PARAM, 0.f, 1.f, 0.7937f, "Master level", " dB", -10.f, 60.f, 6.0206f);
		configInput(TRIG_INPUT, "Random trigger: roll on the next beat");
		configInput(COUNT_INPUT, "COUNT CV: -5 to +5 V adds 0 to 8 (as Tiny Tricks' Random Mute x8)");
		configInput(SCENE_INPUT, "Scene CV: 0 to 4 V picks A to D (1 V each)");
		configInput(KICK_INPUT, "Kick for ducking: trigger or audio (unpatched: channel 1's audio)");
		configInput(CLOCK_INPUT, "Clock for the beat (16th notes; ignored when LINK is patched)");
		configInput(ACT_INPUT, "ACT mutes (poly, 8 channels: 10 V = play, 0 V = muted). Quantised to the beat");
		configInput(LINK_INPUT, "LINK from Long Form");
		configOutput(OUT_L_OUTPUT, "Mix left");
		configOutput(OUT_R_OUTPUT, "Mix right");
		configOutput(STEMS_OUTPUT, "Stems (poly 16: 1-8 left, 9-16 right, post-fader and post-mute)");
		lightDiv.setDivision(64);
		m.rng.s ^= random::u64();
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		m = MuteLogic();
		m.rng.s ^= random::u64();
		timing = NEXT_BEAT; ppqnIndex = 0; fadeIndex = 1;
	}

	void process(const ProcessArgs& args) override {
		float dt = args.sampleTime;
		// ---- where the beat comes from ----
		bool linked = inputs[LINK_INPUT].isConnected() && inputs[LINK_INPUT].getChannels() >= 13;
		bool beat = false, six = false;
		bool haveClock = linked || inputs[CLOCK_INPUT].isConnected();
		if (linked) {
			Input& in = inputs[LINK_INPUT];
			beat = linkBeat.process(in.getVoltage(8), 0.1f, 1.f);
			six = linkSix.process(in.getVoltage(12), 0.1f, 1.f);
		}
		else if (inputs[CLOCK_INPUT].isConnected()) {
			if (clockIn.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
				int ppqn = PPQN_CHOICES[ppqnIndex];
				if (pulseCount % ppqn == 0) beat = true;
				if (pulseCount % std::max(1, ppqn / 4) == 0) six = true;
				pulseCount = (pulseCount + 1) % (ppqn * 4);
			}
		}

		// ---- external mutes: LINK lanes (already on the beat) or ACT ----
		bool quantise = haveClock && timing != IMMEDIATE;
		if (linked) {
			m.extPresent = true;
			for (int i = 0; i < CH; i++) m.setExternal(i, inputs[LINK_INPUT].getVoltage(i) < 5.f, true, quantise);
			if (linkChange.process(inputs[LINK_INPUT].getVoltage(9), 0.1f, 1.f)) m.sectionChange();
		}
		else if (inputs[ACT_INPUT].isConnected()) {
			m.extPresent = true;
			int n = inputs[ACT_INPUT].getChannels();
			for (int i = 0; i < CH; i++) {
				bool mt = i < n && inputs[ACT_INPUT].getVoltage(i) < 5.f;
				m.setExternal(i, mt, false, quantise);
			}
		}
		else if (m.extPresent) {
			m.extPresent = false;
			m.clearExternal();
		}

		// ---- presses ----
		for (int i = 0; i < CH; i++) {
			m.connected[i] = inputs[IN_L_INPUT + i].isConnected() || inputs[IN_R_INPUT + i].isConnected();
			if (muteBtn[i].process(params[MUTE_PARAM + i].getValue() > 0.f)) m.pressMute(i);
			if (soloBtn[i].process(params[SOLO_PARAM + i].getValue() > 0.f)) m.pressSolo(i);
		}
		if (diceBtn.process(params[DICE_PARAM].getValue() > 0.f) | trigIn.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f)) m.dice();
		if (backBtn.process(params[BACK_PARAM].getValue() > 0.f)) m.back();
		int count = (int) std::round(params[COUNT_PARAM].getValue());
		if (inputs[COUNT_INPUT].isConnected()) count += (int) std::round((clamp(inputs[COUNT_INPUT].getVoltage(), -5.f, 5.f) + 5.f) * 0.8f);
		m.count = clamp(count, 0, CH);
		m.includeKick = params[KICK_SWITCH_PARAM].getValue() > 0.5f;
		for (int s = 0; s < SCENES; s++) {
			bool down = params[SCENE_PARAM + s].getValue() > 0.f;
			if (down && !scenePressed[s]) { sceneHeld[s] = 0.f; sceneDone[s] = false; }
			if (down) {
				sceneHeld[s] += dt;
				if (sceneHeld[s] > 1.f && !sceneDone[s]) { m.storeScene(s); sceneDone[s] = true; }
			}
			if (!down && scenePressed[s] && !sceneDone[s]) m.recallScene(s);
			scenePressed[s] = down;
		}
		if (inputs[SCENE_INPUT].isConnected()) {
			int s = clamp((int) std::floor(inputs[SCENE_INPUT].getVoltage()), 0, SCENES - 1);
			if (s != lastSceneCv) { m.recallScene(s); lastSceneCv = s; }
		}
		else lastSceneCv = -1;

		// ---- the quantum ----
		if (!haveClock || timing == IMMEDIATE) m.quantum();
		else if (timing == NEXT_BEAT ? beat : six) m.quantum();

		// ---- audio ----
		float kick = inputs[KICK_INPUT].isConnected() ? inputs[KICK_INPUT].getVoltage() : inputs[IN_L_INPUT].getVoltage();
		duck.process(kick, dt, params[RELEASE_PARAM].getValue(), params[SHAPE_PARAM].getValue());
		float fadeStep = 1.f / std::max(1.f, FADE_MS[fadeIndex] * 0.001f * args.sampleRate);
		float sumL = 0.f, sumR = 0.f;
		float stemL[CH], stemR[CH];
		float mdecay = std::exp(-dt / 0.25f);
		for (int i = 0; i < CH; i++) {
			fade[i].process(m.audible(i), fadeStep);
			stemL[i] = stemR[i] = 0.f;
			if (!m.connected[i]) { meter[i] *= mdecay; continue; }
			bool stereo = inputs[IN_R_INPUT + i].isConnected() && inputs[IN_L_INPUT + i].isConnected();
			float l = inputs[IN_L_INPUT + i].isConnected() ? inputs[IN_L_INPUT + i].getVoltage() : inputs[IN_R_INPUT + i].getVoltage();
			float r = stereo ? inputs[IN_R_INPUT + i].getVoltage() : l;
			float gl, gr;
			float pan = params[PAN_PARAM + i].getValue();
			if (stereo) panStereo(pan, &gl, &gr);
			else panMono(pan, &gl, &gr);
			float d = i == 0 ? 0.f : params[DUCK_PARAM + i].getValue() * duck.amount;
			float g = faderGain(params[LEVEL_PARAM + i].getValue()) * fade[i].g * (1.f - d);
			stemL[i] = l * gl * g;
			stemR[i] = r * gr * g;
			sumL += stemL[i];
			sumR += stemR[i];
			float pk = std::max(std::fabs(stemL[i]), std::fabs(stemR[i]));
			meter[i] = pk > meter[i] ? pk : meter[i] * mdecay;
		}
		float gg = glue.gainFor(sumL, sumR, params[GLUE_PARAM].getValue(), dt);
		float master = faderGain(params[MASTER_PARAM].getValue()) * gg;
		float oL = sumL * master, oR = sumR * master;
		outputs[OUT_L_OUTPUT].setVoltage(oL);
		outputs[OUT_R_OUTPUT].setVoltage(oR);
		outMeter[0] = std::fabs(oL) > outMeter[0] ? std::fabs(oL) : outMeter[0] * mdecay;
		outMeter[1] = std::fabs(oR) > outMeter[1] ? std::fabs(oR) : outMeter[1] * mdecay;
		outputs[STEMS_OUTPUT].setChannels(16);
		for (int i = 0; i < CH; i++) {
			outputs[STEMS_OUTPUT].setVoltage(stemL[i], i);
			outputs[STEMS_OUTPUT].setVoltage(stemR[i], i + 8);
		}

		if (lightDiv.process()) updateLights(dt * lightDiv.getDivision());
	}

	static float db(float v) { return 20.f * std::log10(std::max(1e-6f, v / 5.f)); }

	void updateLights(float ldt) {
		blink += ldt * 4.f;
		if (blink >= 1.f) blink -= 1.f;
		bool on = blink < 0.5f;
		static const float CH_DB[6] = {-30.f, -18.f, -9.f, -3.f, 0.f, 6.f};
		static const float OUT_DB[8] = {-36.f, -24.f, -15.f, -9.f, -4.f, -1.f, 2.f, 6.f};
		static const float GR_DB[6] = {0.5f, 1.f, 2.f, 3.f, 4.f, 6.f};
		for (int i = 0; i < CH; i++) {
			float r = 0.f, g = 0.f, b = 0.f;
			if (m.pending[i] || m.extPending[i]) { if (on) { r = 1.f; g = 0.45f; } }
			else if (m.overridden(i)) { r = 1.f; g = 0.7f; }
			else if (m.muted(i)) { r = 1.f; }
			lights[MUTE_LIGHT + 3 * i + 0].setBrightness(r);
			lights[MUTE_LIGHT + 3 * i + 1].setBrightness(g);
			lights[MUTE_LIGHT + 3 * i + 2].setBrightness(b);
			float s = m.soloPending[i] ? (on ? 1.f : 0.f) : (m.solo[i] ? 1.f : 0.f);
			lights[SOLO_LIGHT + i].setBrightness(s);
			float lv = db(meter[i]);
			for (int k = 0; k < 6; k++) lights[METER_LIGHT + 6 * i + k].setBrightness(lv >= CH_DB[k] ? 1.f : 0.f);
		}
		bool dice = m.diceArmed;
		lights[DICE_LIGHT + 0].setBrightness(dice ? (on ? 1.f : 0.f) : 0.15f);
		lights[DICE_LIGHT + 1].setBrightness(dice ? (on ? 0.45f : 0.f) : 0.07f);
		lights[DICE_LIGHT + 2].setBrightness(0.f);
		lights[BACK_LIGHT].setBrightness(m.backArmed ? (on ? 1.f : 0.f) : (m.haveBack ? 0.3f : 0.f));
		for (int s = 0; s < SCENES; s++)
			lights[SCENE_LIGHT + s].setBrightness(m.sceneShown == s ? 1.f : (m.sceneStored[s] ? 0.15f : 0.f));
		for (int k = 0; k < 6; k++) lights[GR_LIGHT + k].setBrightness(glue.grDb >= GR_DB[k] ? 1.f : 0.f);
		for (int c = 0; c < 2; c++) {
			float lv = db(outMeter[c]);
			for (int k = 0; k < 8; k++) lights[OUT_LIGHT + 8 * c + k].setBrightness(lv >= OUT_DB[k] ? 1.f : 0.f);
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_t* sc = json_array();
		for (int s = 0; s < SCENES; s++) json_array_append_new(sc, json_integer(m.sceneStored[s] ? m.scene[s] : -1));
		json_object_set_new(root, "scenes", sc);
		std::string ov, man, solo, excl;
		for (int i = 0; i < CH; i++) {
			ov += m.override_[i] ? '1' : '0';
			man += m.manual[i] ? '1' : '0';
			solo += m.solo[i] ? '1' : '0';
			excl += m.exclude[i] ? '1' : '0';
		}
		json_object_set_new(root, "override", json_string(ov.c_str()));
		json_object_set_new(root, "manual", json_string(man.c_str()));
		json_object_set_new(root, "solo", json_string(solo.c_str()));
		json_object_set_new(root, "exclude", json_string(excl.c_str()));
		json_object_set_new(root, "timing", json_integer(timing));
		json_object_set_new(root, "ppqnIndex", json_integer(ppqnIndex));
		json_object_set_new(root, "fadeIndex", json_integer(fadeIndex));
		json_object_set_new(root, "manualStays", json_boolean(m.manualStays));
		json_object_set_new(root, "avoidRepeat", json_boolean(m.avoidRepeat));
		return root;
	}

	static void readBits(json_t* root, const char* key, bool* out) {
		json_t* j = json_object_get(root, key);
		if (!j || !json_is_string(j)) return;
		std::string s = json_string_value(j);
		for (int i = 0; i < CH && i < (int) s.size(); i++) out[i] = s[i] == '1';
	}

	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "scenes")))
			for (int s = 0; s < SCENES; s++) {
				int v = (int) json_integer_value(json_array_get(j, s));
				m.sceneStored[s] = v >= 0;
				m.scene[s] = (uint8_t) std::max(0, v);
			}
		readBits(root, "override", m.override_);
		readBits(root, "manual", m.manual);
		readBits(root, "solo", m.solo);
		readBits(root, "exclude", m.exclude);
		if ((j = json_object_get(root, "timing"))) timing = clamp((int) json_integer_value(j), 0, 2);
		if ((j = json_object_get(root, "ppqnIndex"))) ppqnIndex = clamp((int) json_integer_value(j), 0, 3);
		if ((j = json_object_get(root, "fadeIndex"))) fadeIndex = clamp((int) json_integer_value(j), 0, 4);
		if ((j = json_object_get(root, "manualStays"))) m.manualStays = json_is_true(j);
		if ((j = json_object_get(root, "avoidRepeat"))) m.avoidRepeat = json_is_true(j);
	}
};

struct MixxaWidget : ModuleWidget {
	MixxaWidget(Mixxa* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Mixxa.svg")));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		using namespace mxl;
		using M = Mixxa;

		for (int i = 0; i < CH; i++) {
			float cx = STRIP_X[i];
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(cx - 4.9f, STRIP_Y[0])), module, M::IN_L_INPUT + i));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(cx + 4.9f, STRIP_Y[0])), module, M::IN_R_INPUT + i));
			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(cx, STRIP_Y[1])), module, M::PAN_PARAM + i));
			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(cx, STRIP_Y[2])), module, M::DUCK_PARAM + i));
			addParam(createParamCentered<VCVSlider>(mm2px(Vec(cx - 2.8f, STRIP_Y[3])), module, M::LEVEL_PARAM + i));
			for (int k = 0; k < 6; k++) {
				Vec p = mm2px(Vec(cx + METER[0], METER[1] - k * METER[2]));
				int id = M::METER_LIGHT + 6 * i + k;
				if (k == 5) addChild(createLightCentered<SmallLight<RedLight>>(p, module, id));
				else if (k == 4) addChild(createLightCentered<SmallLight<YellowLight>>(p, module, id));
				else addChild(createLightCentered<SmallLight<GreenLight>>(p, module, id));
			}
			addParam(createLightParamCentered<VCVLightBezel<RedGreenBlueLight>>(mm2px(Vec(cx - 5.6f, STRIP_Y[4])), module, M::MUTE_PARAM + i, M::MUTE_LIGHT + 3 * i));
			addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(cx + 5.6f, STRIP_Y[4])), module, M::SOLO_PARAM + i, M::SOLO_LIGHT + i));
		}

		float rc = RANDOM[0];
		addParam(createLightParamCentered<VCVLightBezel<RedGreenBlueLight>>(mm2px(Vec(rc, RANDOM[1])), module, M::DICE_PARAM, M::DICE_LIGHT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rc, RANDOM[2])), module, M::TRIG_INPUT));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(rc, RANDOM[3])), module, M::COUNT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(rc, RANDOM[4])), module, M::COUNT_INPUT));
		addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(rc, RANDOM[5])), module, M::BACK_PARAM, M::BACK_LIGHT));
		addParam(createParamCentered<CKSS>(mm2px(Vec(rc, RANDOM[6])), module, M::KICK_SWITCH_PARAM));

		const float* c = MCOL;
		for (int s = 0; s < SCENES; s++)
			addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(c[s], 33.f)), module, M::SCENE_PARAM + s, M::SCENE_LIGHT + s));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(c[4] - 0.5f, 33.f)), module, M::SCENE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(c[0], 60.f)), module, M::KICK_INPUT));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(c[1] + 3.f, 60.f)), module, M::RELEASE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(c[3] - 1.f, 60.f)), module, M::SHAPE_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(c[4], 60.f)), module, M::CLOCK_INPUT));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(c[0] + 3.f, 87.f)), module, M::GLUE_PARAM));
		for (int k = 0; k < 6; k++)
			addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(c[1] + 3.5f, 80.f + k * 3.f)), module, M::GR_LIGHT + k));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(c[2] + 7.f, 87.f)), module, M::MASTER_PARAM));
		for (int ch = 0; ch < 2; ch++)
			for (int k = 0; k < 8; k++) {
				Vec p = mm2px(Vec(c[4] + (ch == 0 ? -1.8f : 1.8f), 96.f - k * 2.5f));
				int id = M::OUT_LIGHT + 8 * ch + k;
				if (k == 7) addChild(createLightCentered<TinyLight<RedLight>>(p, module, id));
				else if (k >= 5) addChild(createLightCentered<TinyLight<YellowLight>>(p, module, id));
				else addChild(createLightCentered<TinyLight<GreenLight>>(p, module, id));
			}
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(c[0], 111.f)), module, M::ACT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(c[1], 111.f)), module, M::LINK_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(c[2] + 0.5f, 111.f)), module, M::OUT_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(c[3] + 0.5f, 111.f)), module, M::OUT_R_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(c[4] + 0.5f, 111.f)), module, M::STEMS_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Mixxa* mod = getModule<Mixxa>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Mute timing", {"Next beat", "Next 16th", "Immediate"}, &mod->timing));
		menu->addChild(createIndexPtrSubmenuItem("Clock input", {"16th notes (4 per beat)", "24 PPQN (MIDI clock)", "8th notes", "Quarter notes"}, &mod->ppqnIndex));
		menu->addChild(createIndexPtrSubmenuItem("Mute fade", {"2 ms", "4 ms", "8 ms", "12 ms", "20 ms"}, &mod->fadeIndex));
		menu->addChild(createBoolPtrMenuItem("Manual mutes stay (past Long Form's section changes)", "", &mod->m.manualStays));
		menu->addChild(createBoolPtrMenuItem("Random: always a new set", "", &mod->m.avoidRepeat));
		menu->addChild(createSubmenuItem("Leave out of random mutes", "", [=](Menu* sub) {
			sub->addChild(createMenuLabel("Channel 1: use the KICK switch"));
			for (int i = 1; i < CH; i++)
				sub->addChild(createBoolPtrMenuItem(string::f("Channel %d", i + 1), "", &mod->m.exclude[i]));
		}));
		menu->addChild(createMenuItem("Clear all mutes and solos", "", [=]() {
			for (int i = 0; i < CH; i++) { mod->m.override_[i] = false; mod->m.manual[i] = false; mod->m.solo[i] = false; mod->m.pending[i] = false; mod->m.soloPending[i] = false; }
		}));
	}
};

Model* modelMixxa = createModel<Mixxa, MixxaWidget>("Mixxa");
