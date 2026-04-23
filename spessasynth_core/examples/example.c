#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio_io.h"

#include "spessasynth/synthesizer/synth.h"

#include "spessasynth/midi/midi.h"
#include "spessasynth/sequencer/sequencer.h"
#include "spessasynth/sflist/sflist.h"

// Holds the global instance pointer
SS_MIDIFile *g_midiFile;
SS_Sequencer *g_sequencer;

SS_Processor *g_processor;
SS_SoundBank *g_soundBank;

static bool has_ext_ci(const char *path, const char *ext) {
	size_t plen = strlen(path);
	size_t elen = strlen(ext);
	if(plen < elen) return false;
	const char *tail = path + plen - elen;
	for(size_t i = 0; i < elen; i++) {
		char a = tail[i], b = ext[i];
		if(a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
		if(b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
		if(a != b) return false;
	}
	return true;
}

/* Load an sflist (JSON or legacy text) and register it with the processor. */
static bool load_sflist(SS_Processor *proc, const char *sflistPath) {
	FILE *f = fopen(sflistPath, "rb");
	if(!f) {
		fprintf(stderr, "Could not open sflist '%s'\n", sflistPath);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(len < 0) { fclose(f); return false; }

	char *buf = (char *)malloc((size_t)len + 1);
	if(!buf) { fclose(f); fprintf(stderr, "Out of memory\n"); return false; }
	if(len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
		fclose(f); free(buf);
		fprintf(stderr, "Could not read sflist '%s'\n", sflistPath);
		return false;
	}
	buf[len] = '\0';
	fclose(f);

	/* Derive base path from the sflist file location so relative
	 * SoundFont paths resolve correctly. */
	char base_path[4096];
	const char *slash = strrchr(sflistPath, '/');
#ifdef _WIN32
	const char *bslash = strrchr(sflistPath, '\\');
	if(bslash && (!slash || bslash > slash)) slash = bslash;
#endif
	if(slash) {
		size_t n = (size_t)(slash - sflistPath);
		if(n >= sizeof(base_path)) n = sizeof(base_path) - 1;
		memcpy(base_path, sflistPath, n);
		base_path[n] = '\0';
	} else {
		strcpy(base_path, ".");
	}

	char err[sflist_max_error] = "";
	SS_FilteredBanks *banks = sflist_load(buf, (size_t)len, base_path, err);
	free(buf);
	if(!banks) {
		fprintf(stderr, "Could not parse sflist '%s': %s\n", sflistPath, err);
		return false;
	}

	if(!ss_processor_load_filtered_banks(proc, banks, "theBank", false)) {
		fprintf(stderr, "Could not register sflist with the synthesizer\n");
		sflist_free(banks);
		return false;
	}
	return true;
}

// Callback function called by the audio thread
static void AudioCallback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount) {
	float *stream = (float *)pOutput;
	for(ma_uint32 SampleBlock = 128; frameCount; frameCount -= SampleBlock, stream += SampleBlock * 2) // 2 channel output
	{
		// We progress the MIDI playback and then process TSF_RENDER_EFFECTSAMPLEBLOCK samples at once
		if(SampleBlock > frameCount) SampleBlock = frameCount;

		// Loop through all MIDI messages which need to be played up until the current playback time
		ss_sequencer_tick(g_sequencer, SampleBlock);

		// Render the block of audio samples in float format
		ss_processor_render_interleaved(g_processor, stream, SampleBlock);
	}
}

bool load_midi_file(SS_Sequencer *seq, const char *midiName) {
	SS_File *fMidiFile = ss_file_open_from_file(midiName);
	if(!fMidiFile) {
		fprintf(stderr, "Could not load MIDI file\n");
		return false;
	}

	g_midiFile = ss_midi_load(fMidiFile, midiName);
	ss_file_close(fMidiFile);
	if(!g_midiFile) {
		fprintf(stderr, "Could not load MIDI file\n");
		return false;
	}

	/* Filter it */
	if(ss_midi_has_emidi(g_midiFile)) {
		ss_midi_remove_emidi_non_gm(g_midiFile);
	}

	if(!ss_sequencer_load_midi(seq, g_midiFile)) {
		fprintf(stderr, "Could not load MIDI file into sequencer\n");
		return false;
	}

	return true;
}

int main(int argc, char *argv[]) {
	// This implements a small program that you can launch without
	// parameters for a default file & soundfont, or with these arguments:
	//
	// ./example3-... <yoursoundfont>.sf2 <yourfile>.mid [.. more midi files]
	// Optionally: --inf - play forever

	bool playForever = false;
	int argPlayForever = -1;
	int argSoundBank = -1;
	for(int i = 1; i < argc; i++) {
		if(strcmp(argv[i], "--inf") == 0) {
			playForever = true;
			argPlayForever = i;
			break;
		}
	}

	// Define the desired audio output format we request
	ma_device device;
	ma_device_config deviceConfig;
	deviceConfig = ma_device_config_init(ma_device_type_playback);
	deviceConfig.playback.format = ma_format_f32;
	deviceConfig.playback.channels = 2;
	deviceConfig.sampleRate = 44100;
	deviceConfig.dataCallback = AudioCallback;
	deviceConfig.performanceProfile = ma_performance_profile_conservative;

	// Initialize the audio system
	if(ma_device_init(NULL, &deviceConfig, &device) != MA_SUCCESS) {
		fprintf(stderr, "Could not initialize audio hardware or driver\n");
		return 1;
	}

	// Pick the SoundFont / sflist argument
	const char *soundBankName = NULL;
	for(int i = 1; i < argc; i++) {
		if(i == argPlayForever) continue;
		argSoundBank = i;
		soundBankName = argv[i];
		break;
	}
	if(!soundBankName) soundBankName = "florestan-subset.sf2";
	const bool isSflist = has_ext_ci(soundBankName, ".json") ||
	                      has_ext_ci(soundBankName, ".sflist");

	/* Plain SoundFont: load eagerly here; sflist is loaded after the
	 * processor exists since it registers directly with it. */
	if(!isSflist) {
		SS_File *fSoundBank = ss_file_open_from_file(soundBankName);
		if(!fSoundBank) {
			fprintf(stderr, "Could not load SoundFont\n");
			return 1;
		}
		g_soundBank = ss_soundbank_load(fSoundBank);
		ss_file_close(fSoundBank);
		if(!g_soundBank) {
			fprintf(stderr, "Could not load SoundFont\n");
			return 1;
		}
	}

	// Set the SoundFont rendering output mode
	g_processor = ss_processor_create((int)deviceConfig.sampleRate, NULL);
	if(!g_processor) {
		fprintf(stderr, "Could not create the synthesizer\n");
		return 1;
	}

	g_sequencer = ss_sequencer_create(g_processor);
	if(!g_sequencer) {
		fprintf(stderr, "Out of memory");
		return 1;
	}

	size_t midisLoaded = 0;
	for(int i = 1; i < argc; i++) {
		if(i == argPlayForever || i == argSoundBank) continue;
		if(!load_midi_file(g_sequencer, argv[i])) {
			return 1;
		}
		midisLoaded++;
	}
	if(midisLoaded < 1) {
		// Venture (Original WIP) by Ximon
		// https://musescore.com/user/2391686/scores/841451
		// License: Creative Commons copyright waiver (CC0)
		if(!load_midi_file(g_sequencer, "venture.mid")) {
			return 1;
		}
	}

	if(isSflist) {
		if(!load_sflist(g_processor, soundBankName)) {
			return 1;
		}
	} else {
		if(!ss_processor_load_soundbank(g_processor, g_soundBank, "theBank", 0, false)) {
			fprintf(stderr, "Could not add the bank to the synthesizer\n");
			return 1;
		}
	}

	// Start the actual audio playback here
	// The audio thread will begin to call our AudioCallback function
	if(ma_device_start(&device) != MA_SUCCESS) {
		fprintf(stderr, "Failed to start playback device.\n");
		ma_device_uninit(&device);
		return 1;
	}

	ss_sequencer_set_loop_count(g_sequencer, playForever ? -1 : 1);

	ss_sequencer_play(g_sequencer);

	// Wait until the entire MIDI file has been played back (until the end of the linked message list is reached)
	while(!ss_sequencer_is_finished(g_sequencer)) ma_sleep(100);

	ma_device_uninit(&device);

	// We could call tsf_close(g_TinySoundFontSynth) and tml_free(TinyMidiLoader)
	// here to free the memory and resources but we just let the OS clean up
	// because the process ends here.
	return 0;
}
