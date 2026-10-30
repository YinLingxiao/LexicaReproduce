import assert from 'node:assert/strict';
import { createSpeechController, SpeechError } from '../src/lib/speech-controller.ts';
function harness() {
    const plays = [];
    const urls = [];
    const revoked = [];
    const cancels = [];
    const requests = [];
    const audios = [];
    const timers = [];
    let nextTimer = 1;
    let playImpl = async () => { };
    let synthesizeImpl = async (request) => ({
        mime: 'audio/wav',
        bytes: Uint8Array.from([request.token]),
        voiceId: request.voiceId ?? 'uk',
        voiceLabel: 'Hazel',
        language: 'en-GB',
        requestedAccent: request.accent,
        actualAccent: 'UK',
        substituted: false
    });
    const controller = createSpeechController({
        async synthesize(request) {
            requests.push(request);
            return synthesizeImpl(request);
        },
        async cancel(token) {
            cancels.push(token);
        },
        createAudio(url) {
            const audio = {
                url,
                onended: null,
                onerror: null,
                play() {
                    plays.push(url);
                    return playImpl();
                },
                pause() { },
                setCurrentTime() { }
            };
            audios.push(audio);
            return audio;
        },
        createObjectURL(bytes) {
            const url = `blob:${bytes.join(',')}`;
            urls.push(url);
            return url;
        },
        revokeObjectURL(url) {
            revoked.push(url);
        },
        setTimer(fn) {
            const id = nextTimer++;
            timers.push({ id, fn });
            return id;
        },
        clearTimer(id) {
            const index = timers.findIndex((timer) => timer.id === id);
            if (index >= 0)
                timers.splice(index, 1);
        }
    });
    return {
        controller,
        plays,
        urls,
        revoked,
        cancels,
        requests,
        audios,
        timers,
        setSynthesize(fn) {
            synthesizeImpl = fn;
        },
        setPlay(fn) {
            playImpl = fn;
        }
    };
}
async function flush() {
    await new Promise((resolve) => setTimeout(resolve, 0));
}
const tests = [];
function test(name, fn) {
    tests.push([name, fn]);
}
test('no English voice is an error', async () => {
    const h = harness();
    h.setSynthesize(async () => {
        throw new SpeechError('RESOURCE_ERROR', '没有可用的英语语音。请安装文字转语音。');
    });
    h.controller.speak({ text: 'hello', playKey: 'word' });
    await flush();
    assert.equal(h.controller.snapshot().phase, 'error');
    assert.match(h.controller.snapshot().error, /文字转语音/);
    assert.equal(h.plays.length, 0);
});
test('synthesis and playback failures stay visible', async () => {
    const failed = harness();
    failed.setSynthesize(async () => {
        throw new SpeechError('RESOURCE_ERROR', '合成失败');
    });
    failed.controller.speak({ text: 'hello', playKey: 'word' });
    await flush();
    assert.equal(failed.controller.snapshot().phase, 'error');
    assert.equal(failed.controller.snapshot().error, '合成失败');
    const playback = harness();
    playback.controller.speak({ text: 'hello', playKey: 'word' });
    await flush();
    playback.audios[0].onerror?.();
    await flush();
    assert.equal(playback.controller.snapshot().phase, 'error');
    assert.equal(playback.controller.snapshot().error, "Audio playback failed.");
    const blocked = harness();
    blocked.setPlay(() => Promise.reject(Object.assign(new Error('play'), { name: 'NotAllowedError' })));
    blocked.controller.speak({ text: 'hello', playKey: 'word' });
    await flush();
    assert.equal(blocked.controller.snapshot().phase, 'error');
    assert.match(blocked.controller.snapshot().error, /Autoplay was blocked/);
    assert.equal(blocked.controller.snapshot().phase === 'playing', false);
});
test('a late result cannot replace the newer playback', async () => {
    const h = harness();
    const pending = [];
    h.setSynthesize((request) => new Promise((resolve) => pending.push((value) => resolve(value ?? {
        mime: 'audio/wav',
        bytes: Uint8Array.of(request.token),
        voiceId: 'uk',
        voiceLabel: 'Hazel',
        language: 'en-GB',
        requestedAccent: null,
        actualAccent: 'UK',
        substituted: false
    }))));
    for (let i = 1; i <= 8; i++)
        h.controller.speak({ text: `word-${i}`, playKey: `k${i}`, voiceId: 'uk', accent: 'UK', rate: 1 });
    await flush();
    assert.equal(h.requests.length, 1);
    assert.equal(h.requests[0].text, 'word-1');
    assert.deepEqual(h.cancels, [8]);
    pending[0]();
    await flush();
    assert.equal(h.plays.length, 0);
    assert.equal(h.requests.length, 2);
    assert.equal(h.requests[1].text, 'word-8');
    pending[1]();
    await flush();
    assert.equal(h.plays.length, 1);
    assert.equal(h.controller.snapshot().playKey, 'k8');
    assert.equal(h.controller.snapshot().phase, 'playing');
});
test('stop cancels the pending repeat and is not an error', async () => {
    const h = harness();
    h.controller.speak({ text: 'hello', playKey: 'word', repeat: 3, rate: 0.8, voiceId: 'us', accent: 'US' });
    await flush();
    assert.equal(h.requests[0].rate, 0.8);
    assert.equal(h.requests[0].voiceId, 'us');
    assert.equal(h.requests[0].accent, 'US');
    assert.equal(h.plays.length, 1);
    h.audios[0].onended?.();
    await flush();
    assert.equal(h.timers.length, 1);
    const leftover = h.timers[0].fn;
    h.controller.stop();
    leftover();
    await flush();
    assert.equal(h.plays.length, 1);
    assert.equal(h.controller.snapshot().phase, 'idle');
    assert.equal(h.controller.snapshot().error, '');
    assert.ok(h.revoked.length >= 1);
});
test('rate, voice and repeat are applied once per utterance', async () => {
    const h = harness();
    h.setSynthesize(async (request) => ({
        mime: 'audio/wav',
        bytes: Uint8Array.of(7),
        voiceId: request.voiceId ?? 'uk',
        voiceLabel: 'Zira',
        language: 'en-US',
        requestedAccent: 'UK',
        actualAccent: 'US',
        substituted: true
    }));
    h.controller.speak({ text: 'meticulous', playKey: 'word', repeat: 3, rate: 1.2, voiceId: null, accent: 'UK' });
    await flush();
    assert.equal(h.requests.length, 1);
    assert.equal(h.requests[0].text, 'meticulous');
    assert.equal(h.requests[0].rate, 1.2);
    assert.equal(h.requests[0].voiceId, null);
    assert.match(h.controller.snapshot().notice, /British English is unavailable/);
    assert.match(h.controller.snapshot().notice, /Zira/);
    assert.equal(h.controller.snapshot().actualAccent, 'US');
    const fire = async () => {
        h.audios[0].onended?.();
        await flush();
        h.timers.at(-1)?.fn();
        await flush();
    };
    await fire();
    await fire();
    h.audios[0].onended?.();
    await flush();
    assert.equal(h.plays.length, 3);
    assert.equal(h.requests.length, 1);
    assert.equal(h.controller.snapshot().phase, 'idle');
    assert.equal(h.controller.snapshot().error, '');
});
test('synchronous playback failures are visible and release their URL', async () => {
    const h = harness();
    h.setPlay(() => { throw new Error('audio unavailable'); });
    h.controller.speak({ text: 'hello', playKey: 'word' });
    await flush();
    assert.equal(h.controller.snapshot().phase, 'error');
    assert.equal(h.revoked.length, 1);
});
test('text size is checked in UTF-8 bytes before synthesis', async () => {
    const h = harness();
    h.controller.speak({ text: '词'.repeat(700), playKey: 'word' });
    await flush();
    assert.equal(h.controller.snapshot().phase, 'error');
    assert.equal(h.requests.length, 0);
});
let failed = 0;
for (const [name, fn] of tests) {
    try {
        await fn();
        console.log(`ok ${name}`);
    }
    catch (error) {
        failed += 1;
        console.error(`fail ${name}`);
        console.error(error);
    }
}
if (failed)
    process.exit(1);
