#!/usr/bin/env python3
"""가짜 OBS 플러그인 — 인그레스 검증용 (엔진별 레이트 송신).

OBS/C++ 플러그인 없이, prototype 의 한국어 샘플 WAV(16k mono s16)를 100ms 청크로
/ingress 에 실시간 페이스로 업로드한다. 서버 단독 end-to-end 검증에 사용.

엔진에 따라 입력 레이트가 다르다(Gemini 16k / OpenAI 24k). 실제 플러그인은 OBS 48k 원본을
엔진 네이티브 레이트로 다운샘플하지만, 여기선 16k 샘플밖에 없어 OpenAI(24k)용은 16k→24k
선형 리샘플한다(프로토콜·경로 스모크 테스트용; 품질 A/B 는 실제 플러그인으로).

  python fake_plugin.py                          # gemini(16k), localhost
  python fake_plugin.py --engine openai          # openai(24k)
  python fake_plugin.py ws://host:8000 --engine openai
"""
import argparse
import array
import asyncio
import os
import sys
import wave

import websockets
from dotenv import load_dotenv

load_dotenv()

HERE = os.path.dirname(os.path.abspath(__file__))
WAV = os.path.join(HERE, "..", "prototype", "samples", "sermon_ko_16k.wav")
SERVICE_KEY = os.environ.get("SERVICE_KEY", "changeme")

ENGINE_RATE = {"gemini": 16000, "openai": 24000}  # 엔진별 입력 레이트


def resample_s16(pcm: bytes, src_rate: int, dst_rate: int) -> bytes:
    """순수 파이썬 선형 리샘플(s16 mono, little-endian). 테스트 하니스 전용."""
    if src_rate == dst_rate:
        return pcm
    a = array.array("h")
    a.frombytes(pcm)
    if sys.byteorder == "big":
        a.byteswap()
    n = len(a)
    if n == 0:
        return pcm
    ratio = dst_rate / src_rate
    m = int(n * ratio)
    out = array.array("h", bytes(2 * m))
    for i in range(m):
        pos = i / ratio
        j = int(pos)
        frac = pos - j
        s0 = a[j] if j < n else a[n - 1]
        s1 = a[j + 1] if j + 1 < n else s0
        out[i] = int(s0 + (s1 - s0) * frac)
    if sys.byteorder == "big":
        out.byteswap()
    return out.tobytes()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base", nargs="?", default="ws://localhost:8000", help="서버 베이스 URL")
    ap.add_argument("--engine", choices=["gemini", "openai"], default="gemini")
    args = ap.parse_args()

    target_rate = ENGINE_RATE[args.engine]
    chunk = target_rate // 5  # 100ms @ rate, s16 mono = rate/1000*100*2 바이트
    url = f"{args.base}/ingress?key={SERVICE_KEY}&engine={args.engine}"

    with wave.open(WAV, "rb") as w:
        if (w.getnchannels(), w.getsampwidth(), w.getframerate()) != (1, 2, 16000):
            sys.exit("샘플 WAV 포맷 오류: 16kHz mono s16 가 아닙니다. prototype/make_sample.sh 실행 필요.")
        pcm = w.readframes(w.getnframes())

    if target_rate != 16000:
        pcm = resample_s16(pcm, 16000, target_rate)
        print(f"리샘플 16k→{target_rate//1000}k ({args.engine})")

    print(f"연결: {url}")
    async with websockets.connect(url) as ws:
        print(f"ingress 연결됨 → {len(pcm)} bytes 송신 시작 ({len(pcm)/2/target_rate:.1f}s, {target_rate//1000}kHz)")
        for i in range(0, len(pcm), chunk):
            await ws.send(pcm[i:i + chunk])
            await asyncio.sleep(0.1)  # 실시간 페이스
        print("송신 완료. 잔여 번역 대기 위해 5s 유지...")
        await asyncio.sleep(5)
    print("종료(서비스 IDLE 전환).")


if __name__ == "__main__":
    asyncio.run(main())
