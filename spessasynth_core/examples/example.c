#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio_io.h"

#include "spessasynth/synthesizer/synth.h"

#include "spessasynth/midi/midi.h"
#include "spessasynth/sequencer/sequencer.h"

// Holds the global instance pointer
SS_MIDIFile *g_midiFile;
SS_Sequencer *g_sequencer;

SS_Processor *g_processor;
SS_SoundBank *g_soundBank;

// Temporary buffers
float sampleLeft[128], sampleRight[128];

// Callback function called by the audio thread
static void AudioCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
	float* stream = (float*)pOutput;
	for (ma_uint32 SampleBlock = 128; frameCount; frameCount -= SampleBlock, stream += SampleBlock * 2) // 2 channel output
	{
		//We progress the MIDI playback and then process TSF_RENDER_EFFECTSAMPLEBLOCK samples at once
		if (SampleBlock > frameCount) SampleBlock = frameCount;

		//Loop through all MIDI messages which need to be played up until the current playback time
		ss_sequencer_tick(g_sequencer, SampleBlock);

		// Render the block of audio samples in float format
		ss_processor_render(g_processor, sampleLeft, sampleRight, SampleBlock);
		for (ma_uint32 i = 0; i < SampleBlock; i++)
		{
			stream[i * 2] = sampleLeft[i];
			stream[i * 2 + 1] = sampleRight[i];
		}
	}
}

int main(int argc, char *argv[])
{
	// This implements a small program that you can launch without
	// parameters for a default file & soundfont, or with these arguments:
	//
	// ./example3-... <yourfile>.mid <yoursoundfont>.sf2

	// Define the desired audio output format we request
	ma_device device;
	ma_device_config deviceConfig;
	deviceConfig = ma_device_config_init(ma_device_type_playback);
	deviceConfig.playback.format = ma_format_f32;
	deviceConfig.playback.channels = 2;
	deviceConfig.sampleRate = 44100;
	deviceConfig.dataCallback = AudioCallback;

	// Initialize the audio system
	if (ma_device_init(NULL, &deviceConfig, &device) != MA_SUCCESS)
	{
		fprintf(stderr, "Could not initialize audio hardware or driver\n");
		return 1;
	}

	//Venture (Original WIP) by Ximon
	//https://musescore.com/user/2391686/scores/841451
	//License: Creative Commons copyright waiver (CC0)
	const char *midiName = (argc >= 2 ? argv[1] : "venture.mid");

	uint8_t *midiFile;
	FILE *fMidiFile = fopen(midiName, "rb");
	if (!fMidiFile)
	{
		fprintf(stderr, "Could not load MIDI file\n");
		return 1;
	}

	fseek(fMidiFile, 0, SEEK_END);
	size_t midiSize = ftell(fMidiFile);
	fseek(fMidiFile, 0, SEEK_SET);

	midiFile = malloc(midiSize);
	if (!midiFile)
	{
		fprintf(stderr, "Out of memory\n");
		return 1;
	}

	fread(midiFile, 1, midiSize, fMidiFile);
	fclose(fMidiFile);

	g_midiFile = ss_midi_load(midiFile, midiSize, midiName);
	if (!g_midiFile)
	{
		fprintf(stderr, "Could not load MIDI file\n");
		return 1;
	}
	free(midiFile);

	// Load the SoundFont from a file
	const char *soundBankName = (argc >= 3 ? argv[2] : "florestan-subset.sf2");
	FILE *fSoundBank = fopen(soundBankName, "rb");
	if (!fSoundBank)
	{
		fprintf(stderr, "Could not load SoundFont\n");
		return 1;
	}

	fseek(fSoundBank, 0, SEEK_END);
	size_t soundBankSize = ftell(fSoundBank);
	fseek(fSoundBank, 0, SEEK_SET);

	uint8_t *soundBank = malloc(soundBankSize);
	if (!soundBank)
	{
		fprintf(stderr, "Out of memory");
		return 1;
	}

	fread(soundBank, 1, soundBankSize, fSoundBank);
	fclose(fSoundBank);

	g_soundBank = ss_soundbank_load(soundBank, soundBankSize);
	if (!g_soundBank)
	{
		fprintf(stderr, "Could not load SoundFont\n");
		return 1;
	}
	free(soundBank);

	// Set the SoundFont rendering output mode
	g_processor = ss_processor_create((int)deviceConfig.sampleRate, NULL);
	if (!g_processor)
	{
		fprintf(stderr, "Could not create the synthesizer\n");
		return 1;
	}

	if (!ss_processor_load_soundbank(g_processor, g_soundBank, "theBank"))
	{
		fprintf(stderr, "Could not add the bank to the synthesizer\n");
		return 1;
	}

	g_sequencer = ss_sequencer_create(g_processor);
	if (!g_sequencer)
	{
		fprintf(stderr, "Out of memory");
		return 1;
	}

	if (!ss_sequencer_load_midi(g_sequencer, g_midiFile))
	{
		fprintf(stderr, "Could not load MIDI file into sequencer\n");
		return 1;
	}

	// Start the actual audio playback here
	// The audio thread will begin to call our AudioCallback function
	if (ma_device_start(&device) != MA_SUCCESS)
	{
		fprintf(stderr, "Failed to start playback device.\n");
		ma_device_uninit(&device);
		return 1;
	}

	ss_sequencer_play(g_sequencer);

	// Wait until the entire MIDI file has been played back (until the end of the linked message list is reached)
	while (!ss_sequencer_is_finished(g_sequencer)) ma_sleep(100);

	ma_device_uninit(&device);

	// We could call tsf_close(g_TinySoundFontSynth) and tml_free(TinyMidiLoader)
	// here to free the memory and resources but we just let the OS clean up
	// because the process ends here.
	return 0;
}
