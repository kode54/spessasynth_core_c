/* This little program uses SpessaSynth TypeScript, to take the existing SFe
 * demo XG bank, then swap in the same named samples from GeneralUser GS 2.0.3,
 * to repair the degradation that occurred up until the great sample restoration
 * that occurred at the 2.0.3 update. It loads two banks into memory, then
 * iterates over the samples in the first bank, locating identically named
 * samples in the second bank, and replacing the ones in the first bank with
 * the matches from the second bank. Then it saves it to a third output bank.
 */

// Process arguments
import * as fs from "fs/promises";
import { BasicSoundBank, SoundBankLoader } from "../src";

const args = process.argv.slice(2);
if (args.length !== 3) {
    console.info("Usage: tsx index.ts <sf2 input path> <sf2 for replacement samples> <sf2 output path>");
    process.exit();
}

const sf2InPath = args[0];
const sf2FixPath = args[1];
const sf2OutPath = args[2];

await BasicSoundBank.isSF3DecoderReady;
const sf2 = await fs.readFile(sf2InPath);
console.time("Loaded in");
const bank = SoundBankLoader.fromArrayBuffer(sf2.buffer);
console.timeEnd("Loaded in");
const sf2fix = await fs.readFile(sf2FixPath);
console.time("Loaded fix bank");
const fixbank = SoundBankLoader.fromArrayBuffer(sf2fix.buffer);
console.timeEnd("Loaded fix bank");

console.info(`Name: ${bank.soundBankInfo.name}`);

console.time("Fixing samples")
bank.samples.forEach((sample, index, array) => {
    const samples = fixbank.samples.filter(fixsample => fixsample.name === sample.name);
    if(samples.length > 0) {
        sample.originalKey = samples[0].originalKey;
        sample.pitchCorrection = samples[0].pitchCorrection;
        sample.sampleType = samples[0].sampleType;
        sample.loopStart = samples[0].loopStart;
        sample.loopEnd = samples[0].loopEnd;
        sample.setAudioData(samples[0].getAudioData(), samples[0].sampleRate);
        console.info(`Fixed: ${sample.name}`);
    }
});
console.timeEnd("Fixing samples")

console.time("Converted in");
const outSF2 = await bank.writeSF2();
console.timeEnd("Converted in");
console.info(`Writing file...`);
await fs.writeFile(sf2OutPath, new Uint8Array(outSF2));
console.info(`File written to ${sf2OutPath}`);
