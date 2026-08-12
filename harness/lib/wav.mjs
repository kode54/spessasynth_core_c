/**
 * Minimal WAV reader/writer for the comparison harness.
 *
 * The writer only emits 32-bit float so nothing on the comparison path is
 * quantized or normalized; the reader also accepts 8/16/24/32-bit PCM so
 * renders from other synths can be dropped in as references.
 */

export function writeWavF32(channels, sampleRate) {
    const numChannels = channels.length;
    if (numChannels === 0) {
        throw new Error("No channels to write");
    }
    const frames = channels[0].length;
    for (const ch of channels) {
        if (ch.length !== frames) {
            throw new Error("Channel length mismatch");
        }
    }

    const dataBytes = frames * numChannels * 4;
    const buffer = Buffer.alloc(44 + dataBytes);

    buffer.write("RIFF", 0, "latin1");
    buffer.writeUInt32LE(36 + dataBytes, 4);
    buffer.write("WAVE", 8, "latin1");

    buffer.write("fmt ", 12, "latin1");
    buffer.writeUInt32LE(16, 16);
    buffer.writeUInt16LE(3, 20); // IEEE float
    buffer.writeUInt16LE(numChannels, 22);
    buffer.writeUInt32LE(sampleRate, 24);
    buffer.writeUInt32LE(sampleRate * numChannels * 4, 28);
    buffer.writeUInt16LE(numChannels * 4, 32);
    buffer.writeUInt16LE(32, 34);

    buffer.write("data", 36, "latin1");
    buffer.writeUInt32LE(dataBytes, 40);

    let offset = 44;
    for (let i = 0; i < frames; i++) {
        for (let c = 0; c < numChannels; c++) {
            buffer.writeFloatLE(channels[c][i], offset);
            offset += 4;
        }
    }
    return buffer;
}

export function readWav(buffer) {
    const view = new DataView(
        buffer.buffer,
        buffer.byteOffset,
        buffer.byteLength
    );
    const tag = (off) => String.fromCharCode(...buffer.subarray(off, off + 4));

    if (tag(0) !== "RIFF" || tag(8) !== "WAVE") {
        throw new Error("Not a RIFF/WAVE file");
    }

    let pos = 12;
    let fmt = null;
    let dataOffset = -1;
    let dataLength = 0;

    while (pos + 8 <= buffer.length) {
        const id = tag(pos);
        const size = view.getUint32(pos + 4, true);
        const body = pos + 8;
        if (id === "fmt ") {
            fmt = {
                formatTag: view.getUint16(body, true),
                channels: view.getUint16(body + 2, true),
                sampleRate: view.getUint32(body + 4, true),
                bitsPerSample: view.getUint16(body + 14, true)
            };
            if (fmt.formatTag === 0xfffe && size >= 40) {
                // WAVE_FORMAT_EXTENSIBLE: the real tag lives in the GUID.
                fmt.formatTag = view.getUint16(body + 24, true);
            }
        } else if (id === "data") {
            dataOffset = body;
            dataLength = Math.min(size, buffer.length - body);
        }
        pos = body + size + (size & 1);
    }

    if (!fmt) throw new Error("No fmt chunk");
    if (dataOffset < 0) throw new Error("No data chunk");

    const { channels: numChannels, sampleRate, bitsPerSample, formatTag } = fmt;
    const bytesPerSample = bitsPerSample >> 3;
    const frames = Math.floor(dataLength / (numChannels * bytesPerSample));

    const out = [];
    for (let c = 0; c < numChannels; c++) {
        out.push(new Float32Array(frames));
    }

    const isFloat = formatTag === 3;
    if (!isFloat && formatTag !== 1) {
        throw new Error(`Unsupported WAV format tag ${formatTag}`);
    }

    for (let i = 0; i < frames; i++) {
        for (let c = 0; c < numChannels; c++) {
            const at = dataOffset + (i * numChannels + c) * bytesPerSample;
            let v;
            if (isFloat) {
                v =
                    bitsPerSample === 64
                        ? view.getFloat64(at, true)
                        : view.getFloat32(at, true);
            } else if (bitsPerSample === 8) {
                v = (view.getUint8(at) - 128) / 128;
            } else if (bitsPerSample === 16) {
                v = view.getInt16(at, true) / 32768;
            } else if (bitsPerSample === 24) {
                const raw =
                    view.getUint8(at) |
                    (view.getUint8(at + 1) << 8) |
                    (view.getInt8(at + 2) << 16);
                v = raw / 8388608;
            } else if (bitsPerSample === 32) {
                v = view.getInt32(at, true) / 2147483648;
            } else {
                throw new Error(`Unsupported bit depth ${bitsPerSample}`);
            }
            out[c][i] = v;
        }
    }

    return { channels: out, sampleRate, bitsPerSample, formatTag };
}
