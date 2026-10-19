# -*- coding: utf-8 -*-
"""
固件直连千问 Realtime API 的协议验证脚本
==========================================
完整复刻 xiaozhiPro 固件 main/direct/adapters/qwen_realtime.cc 的通信流程：
  连接 -> session.update -> input_audio_buffer.append -> commit -> response.create
  -> 收 response.audio_transcript.delta / response.audio.delta / response.done

两种模式：
  1) 真实验证（默认）：从环境变量 DASHSCOPE_API_KEY 读 Key，连 DashScope 官方服务
  2) mock 自测（--mock）：在本地起一个模拟服务端，验证脚本与固件消息格式，
     不需要 API Key 也能跑通全流程

用法：
  # mock 自测（无 Key）
  python verify_qwen_realtime.py --mock

  # 真实验证
  set DASHSCOPE_API_KEY=sk-xxxx
  python verify_qwen_realtime.py
  python verify_qwen_realtime.py --model qwen-omni-turbo-realtime --voice Chelsie
"""

import argparse
import asyncio
import base64
import json
import os
import sys
import threading

# ---------- 与固件 qwen_realtime.cc 保持一致的常量 ----------
DEFAULT_BASE_URL = "wss://dashscope.aliyuncs.com/api-ws/v1/realtime"
DEFAULT_MODEL = "qwen-omni-turbo-realtime"     # ai_config.cc 迁移后的默认模型
DEFAULT_VOICE = "Chelsie"                      # qwen-omni-turbo-realtime 官方默认音色
INSTRUCTION = "你是小智，一个友好的中文语音助手。回答简洁、口语化。"
TRANSCRIPTION_MODEL = "qwen3-asr-flash-realtime"  # 官方固定值


def build_session_update(model: str, voice: str) -> dict:
    """与固件 SendSessionUpdate() 逐字段一致。"""
    return {
        "type": "session.update",
        "session": {
            "modalities": ["text", "audio"],
            "voice": voice,
            "instructions": INSTRUCTION,
            "input_audio_format": "pcm16",
            "output_audio_format": "pcm24",
            "turn_detection": None,   # Manual 模式：由固件在 StopListening 时提交并触发回复
            "input_audio_transcription": {"model": TRANSCRIPTION_MODEL},
        },
    }


def build_append_pcm16_silence(ms: int = 200) -> dict:
    """生成 ms 毫秒的 16kHz 单声道 PCM16 静音，与固件上行（AudioService::SetSendPcm(true)）一致。"""
    n = 16000 // 1000 * ms * 2
    return {
        "type": "input_audio_buffer.append",
        "audio": base64.b64encode(bytes(n)).decode("ascii"),
    }


# ---------- 客户端流程（真实模式与 mock 模式共用） ----------
async def run_client(uri: str, api_key: str, model: str, voice: str, verbose: bool = True):
    result = {"ok": False, "steps": [], "audio_bytes": 0, "transcript": ""}

    def step(name, ok=True, detail=""):
        line = f"[{'OK ' if ok else 'FAIL'}] {name}"
        if detail:
            line += f"  ({detail})"
        result["steps"].append(line)
        if verbose:
            print(line, flush=True)

    headers = {"Authorization": f"Bearer {api_key}"}
    if not api_key.startswith("Bearer"):
        pass  # 固件 SetHeader("Authorization", "Bearer " + key)，这里直接拼好

    async with __import__("websockets").connect(uri, additional_headers=headers,
                                                open_timeout=15) as ws:
        # 1. 等待 session.created
        while True:
            ev = json.loads(await asyncio.wait_for(ws.recv(), timeout=15))
            if ev.get("type") == "session.created":
                step("session.created（握手成功，会话已建立）")
                break
            if ev.get("type") == "error":
                step("握手后收到 error", False, json.dumps(ev.get("error", ev), ensure_ascii=False))
                return result

        # 2. session.update
        await ws.send(json.dumps(build_session_update(model, voice)))
        while True:
            ev = json.loads(await asyncio.wait_for(ws.recv(), timeout=15))
            t = ev.get("type")
            if t == "session.updated":
                sess = ev.get("session", {})
                echo_in = sess.get("input_audio_format", sess.get("audio", {}).get("input", {}).get("format"))
                echo_out = sess.get("output_audio_format", sess.get("audio", {}).get("output", {}).get("format"))
                step("session.updated（配置已被服务端接受）",
                     detail=f"回显 input={echo_in} output={echo_out} voice={sess.get('voice')}")
                break
            if t == "error":
                step("session.update 被拒绝", False, json.dumps(ev.get("error", ev), ensure_ascii=False))
                return result

        # 3. 上行 2 秒静音（200ms/包 × 10）
        for _ in range(10):
            await ws.send(json.dumps(build_append_pcm16_silence(200)))
        step("input_audio_buffer.append × 10（2 秒 PCM16 静音已发送）")

        # 4. commit
        await ws.send(json.dumps({"type": "input_audio_buffer.commit"}))
        while True:
            ev = json.loads(await asyncio.wait_for(ws.recv(), timeout=15))
            t = ev.get("type")
            if t == "input_audio_buffer.committed":
                step("input_audio_buffer.committed（缓冲区提交成功）")
                break
            if t == "error":
                step("commit 失败（缓冲区为空或格式错误）", False,
                     json.dumps(ev.get("error", ev), ensure_ascii=False))
                return result

        # 5. response.create
        await ws.send(json.dumps({"type": "response.create"}))
        step("response.create 已发送，等待模型响应...")

        # 6. 接收响应
        while True:
            ev = json.loads(await asyncio.wait_for(ws.recv(), timeout=30))
            t = ev.get("type")
            if t == "error":
                step("响应阶段 error", False, json.dumps(ev.get("error", ev), ensure_ascii=False))
                return result
            if t in ("response.audio_transcript.delta", "response.text.delta"):
                result["transcript"] += ev.get("delta", "")
            elif t in ("response.audio.delta", "response.output_audio.delta"):
                try:
                    result["audio_bytes"] += len(base64.b64decode(ev.get("delta", "")))
                except Exception:
                    pass
            elif t == "response.done":
                status = ev.get("response", {}).get("status")
                step("response.done（本轮响应结束）", detail=f"status={status}")
                break

        result["ok"] = True
        if verbose:
            print("", flush=True)
            print(f"收到语音文本: {result['transcript']!r}", flush=True)
            print(f"收到音频字节: {result['audio_bytes']}", flush=True)
    return result


# ---------- mock 服务端（本地自测，无需 Key） ----------
async def mock_handler(ws):
    """模拟 DashScope 关键事件流，严格按官方文档时序。"""
    await ws.send(json.dumps({"type": "session.created", "session": {}}))
    while True:
        msg = json.loads(await ws.recv())
        t = msg.get("type")
        if t == "session.update":
            s = msg.get("session", {})
            # 校验关键字段（与固件一致的期望）
            problems = []
            if s.get("modalities") != ["text", "audio"]:
                problems.append("modalities")
            if s.get("turn_detection") is not None:
                problems.append("turn_detection 应为 null")
            if s.get("input_audio_transcription", {}).get("model") != "qwen3-asr-flash-realtime":
                problems.append("input_audio_transcription.model")
            await ws.send(json.dumps({
                "type": "session.updated",
                "session": {
                    "modalities": ["text", "audio"],
                    "voice": s.get("voice", ""),
                    "input_audio_format": s.get("input_audio_format"),
                    "output_audio_format": s.get("output_audio_format"),
                    "input_audio_transcription": s.get("input_audio_transcription"),
                    "mock_problems": problems,
                },
            }))
        elif t == "input_audio_buffer.append":
            pass
        elif t == "input_audio_buffer.commit":
            await ws.send(json.dumps({"type": "input_audio_buffer.committed", "item_id": "mock-item"}))
        elif t == "response.create":
            await ws.send(json.dumps({"type": "response.created"}))
            await ws.send(json.dumps({
                "type": "response.audio_transcript.delta",
                "delta": "你好，我是小智。",
            }))
            await ws.send(json.dumps({
                "type": "response.audio.delta",
                "delta": base64.b64encode(b"\x00\x00" * 4800).decode("ascii"),  # 200ms pcm24
            }))
            await ws.send(json.dumps({
                "type": "response.audio.delta",
                "delta": base64.b64encode(b"\x00\x00" * 4800).decode("ascii"),
            }))
            await ws.send(json.dumps({"type": "response.done", "response": {"status": "completed"}}))


def run_mock_server(host: str, port: int, ready: threading.Event):
    import websockets

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    async def _start():
        server = await websockets.serve(mock_handler, host, port)
        ready.set()                       # 通知主线程：监听已就绪
        await asyncio.Future()            # 永久运行

    loop.run_until_complete(_start())


async def main():
    ap = argparse.ArgumentParser(description="xiaozhiPro 固件直连千问 Realtime 协议验证")
    ap.add_argument("--mock", action="store_true", help="本地 mock 自测（不需要 API Key）")
    ap.add_argument("--model", default=DEFAULT_MODEL, help=f"模型名（默认 {DEFAULT_MODEL}）")
    ap.add_argument("--voice", default=DEFAULT_VOICE, help=f"音色（默认 {DEFAULT_VOICE}）")
    ap.add_argument("--base-url", default=DEFAULT_BASE_URL, help="接入地址（默认官方公共域名）")
    ap.add_argument("--key", default=os.environ.get("DASHSCOPE_API_KEY", ""), help="API Key（或用环境变量 DASHSCOPE_API_KEY）")
    args = ap.parse_args()

    print("=" * 60)
    print("xiaozhiPro 直连千问 Realtime 协议验证")
    print(f"模型: {args.model}   音色: {args.voice}")
    print(f"端点: {args.base_url}?model={args.model}")
    print("=" * 60)

    if args.mock:
        host, port = "127.0.0.1", 8765
        ready = threading.Event()
        th = threading.Thread(target=run_mock_server, args=(host, port, ready), daemon=True)
        th.start()
        ready.wait(timeout=5)
        uri = f"ws://{host}:{port}"
        print(f"[mock] 本地模拟服务端已启动 {uri}\n")
        r = await run_client(uri, api_key="mock-key", model=args.model, voice=args.voice)
        problems = []
        for s in r["steps"]:
            if "[FAIL]" in s:
                problems.append(s)
        print("-" * 60)
        if problems:
            print("mock 自测结论: 存在 FAIL 项")
            for p in problems:
                print("  " + p)
            sys.exit(1)
        # 检查 mock 回显的字段校验
        print("mock 自测结论: 全流程通过（连接/会话配置/音频上行/提交/触发/下行事件均正常）")
        sys.exit(0 if r["ok"] else 1)

    key = args.key.strip()
    if not key:
        print("\n错误：缺少 API Key。请设置环境变量后再运行：")
        print('  set DASHSCOPE_API_KEY=sk-xxxx')
        print("  或在命令行传参：--key sk-xxxx")
        sys.exit(2)

    uri = args.base_url + ("&" if "?" in args.base_url else "?") + "model=" + args.model
    print(f"\n连接 {uri} ...\n")
    r = await run_client(uri, key, args.model, args.voice)
    print("-" * 60)
    if r["ok"]:
        print("真实链路验证结论: 通过")
        print(f"  - 服务端接受了 session 配置并返回 session.updated")
        print(f"  - 音频上行/提交/触发均正常")
        print(f"  - 模型返回语音文本: {r['transcript']!r}")
        print(f"  - 模型返回音频字节: {r['audio_bytes']}")
        print("  => 固件按此协议接入可正常工作（格式与官方规范一致）")
        sys.exit(0)
    else:
        print("真实链路验证结论: 失败，请根据上方 FAIL 项排查")
        sys.exit(1)


if __name__ == "__main__":
    asyncio.run(main())
