"""在已经 RAM 启动的掌机固件上重复检查 GUI 和原生存档，全程静音。

options：中文游戏启动、音量、跳帧、倍速、保存与缓存 ROM 读档。
reload：仅用内置诊断验证 SRAM/Flash/EEPROM 保存 A、未保存 B、读取 A。
large：32MiB 游戏及既有 128KiB Flash 存档的缓存读档。
switch：小 ROM、龙珠、32MiB ROM、小 ROM、退出，并包含 large 检查。

要求 GUI 已显示 --directory 对应的游戏库。测试不复制或修改 ROM、不复位、
不操作 NOR、不播放测试音；商业游戏只运行启动场景并保存其当前原生状态，
不声称验证了具体剧情进度。每次退出都尝试停止模拟和音频并保持静音。
--save-dir 默认 /GBA/SAVES，mGBA 独立构建可指定 /GBA/SAVES/mGBA。
"""

import argparse
import importlib.util
import json
import math
from datetime import datetime
from pathlib import Path
import re
import sys
import time

import serial


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "engine_gui_save_support", Path(__file__).with_name("test-save-roundtrip.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
require = m.require

A, B, DOWN, RIGHT, LEFT, L = 1, 2, 0x80, 0x10, 0x20, 0x200


class Console(m.Console):
    """共用原始 UART、UTF-8 解码、完整状态和当前七项菜单导航。"""

    def __init__(self, uart, output, report):
        super().__init__(uart, output)
        self.report = report
        self.observed = []
        self.strict = True
        self.operation_timeout = 30
        self.settings_normalized = False

    def lines(self, seconds):
        for line in super().lines(seconds):
            self.observed.append(line)
            yield line

    def wait_since(self, pattern, cursor, timeout):
        for line in self.observed[cursor:]:
            match = re.search(pattern, line)
            if match:
                return match
        return self.wait(pattern, timeout)

    def status(self, step="status"):
        self.send("status")
        state = {}
        prefixes = (
            ("ui enabled=", "ui"), ("core running=", "core"),
            ("backup dirty=", "backup"), ("playback volume=", "playback"),
            ("game_stats ", "game_stats"), ("save_test magic=", "save_test"),
            ("SAVE queued=", "save"), ("BTPAD initialized=", "btpad"),
            ("btpad_rx ", "rx"), ("lcd active=", "lcd"),
            ("audio running=", "audio"),
        )
        hexadecimal = ("crc", "crc32", "keys", "serial_keys", "board_keys", "bt_keys",
                       "magic", "marker", "expected_marker", "mismatch", "boot_marker")
        for line in self.lines(8):
            if line.startswith("time_us="):
                state = {"clock": self.parse_fields(line)}
                continue
            if "clock" not in state:
                continue
            for prefix, group in prefixes:
                if line.startswith(prefix):
                    state[group] = self.parse_fields(line, hexadecimal)
                    if " path=" in line:
                        state[group]["path"] = line.split(" path=", 1)[1]
                    if group == "game_stats":
                        for field in ("cpu_pct", "fps"):
                            if field in state[group]:
                                state[group][field] = float(state[group][field])
                    break
            if line.startswith("audio_timing "):
                state["audio_timing"] = self.parse_fields(line)
                require(all(key in state for key in
                            ("ui", "core", "backup", "playback", "game_stats", "save", "lcd", "audio")),
                        "状态未完整返回: " + repr(state))
                state["host_monotonic"] = time.monotonic()
                self.report["records"].append({"step": step, "state": state})
                if self.strict:
                    self.check_status(state)
                print(json.dumps({"step": step, "ui": state["ui"],
                                  "save_test": state.get("save_test"),
                                  "heap": state["clock"].get("heap_free"),
                                  "playback": state["playback"]}, ensure_ascii=False), flush=True)
                return state
        raise RuntimeError("状态未返回 audio_timing")

    def check_status(self, state):
        require(state["audio"].get("muted") == 1, "音频必须始终静音")
        require(state["lcd"].get("active") == 1 and state["lcd"].get("underrun") == 0,
                "显示未开启或发生欠载")
        require(state["save"].get("errors") == 0, "保存任务出现错误")
        for counter in ("queue_under", "FIFO_under", "errors", "timeouts"):
            require(state["audio"].get(counter) == 0, "音频错误: " + counter)
        for counter in ("checksum_errors", "length_errors", "uart_errors"):
            if "rx" in state:
                require(state["rx"].get(counter) == 0, "蓝牙接收错误: " + counter)
        require(state["core"].get("board_keys") == 0 and state["core"].get("bt_keys") == 0,
                "自动检查期间请松开板载和蓝牙按键")
        if self.report.get("mode") == "reload":
            require(state["playback"].get("speed") == 1 and
                    state["playback"].get("frameskip") == 0,
                    "三类读档诊断需要 speed=1 / frameskip=0")

    def until(self, predicate, timeout, description):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            last = self.status(description)
            if predicate(last):
                return last
            self.idle(min(0.06, max(0, deadline - time.monotonic())))
        raise RuntimeError("等待超时: " + description + "; last=" + repr(last))

    def pulse(self, key):
        self.send("keys " + format(key, "03x"))
        self.idle(0.08)
        self.send("keys 000")
        self.idle(0.10)

    def down(self, count):
        for _ in range(count):
            self.pulse(DOWN)

    def pause(self, step="pause", timeout=None):
        # 客体写入到菜单之间不增加状态采样或固定长等待，保留 2 秒窗口。
        self.send("menu")
        self.idle(0.10)
        state = self.until(is_paused, timeout or self.operation_timeout, step)
        require(state["audio"].get("running") == 0, "菜单未停止音频")
        return state

    def settings(self, step):
        self.pause(step + "-pause")
        self.down(3)
        self.pulse(A)
        state = self.status(step)
        page(state, 4)
        if not self.settings_normalized:
            # 设置页记住上次选中行；同一 RAM 固件重复 options 也从音量行开始。
            # 右键对跳帧/速度必有变化，音量 100 的不变结果说明正处在音量行。
            self.pulse(RIGHT)
            changed = self.status(step + "-locate-row")
            fields = ("volume", "frameskip", "speed")
            differences = [i for i, field in enumerate(fields)
                           if state["playback"][field] != changed["playback"][field]]
            require(len(differences) <= 1, "探测设置行时多项设置同时变化")
            row = differences[0] if differences else 0
            if differences:
                self.pulse(LEFT)
                restored = self.status(step + "-restore-probe")
                require(all(restored["playback"][field] == state["playback"][field] for field in fields),
                        "设置行探测未恢复原值")
            for _ in range(row):
                self.pulse(0x040)
            self.settings_normalized = True
        return state

    def resume_settings(self):
        self.pulse(B)  # 设置回暂停，再恢复；设置选择行保持现有行为。
        self.pulse(B)

    def listing(self, directory):
        cursor = len(self.observed)
        self.send("ls " + directory)
        self.status("inventory")  # 同步消费完整 ls，再返回。
        entries = []
        for line in self.observed[cursor:]:
            match = re.fullmatch(r"([d-])\s+(\d+)\s+(.+)", line)
            if match and match[3] not in (".", "..") and not match[3].startswith("."):
                if match[1] == "d" or match[3].lower().endswith(".gba"):
                    entries.append({"name": match[3], "directory": match[1] == "d",
                                    "bytes": int(match[2])})
        require(entries and all("\ufffd" not in e["name"] for e in entries),
                "没有可导航游戏，或 UTF-8 名称损坏")
        return entries

    def crc(self, path, timeout=30):
        cursor = len(self.observed)
        self.send("crc " + path)
        match = self.wait_since(r"CRC32=([0-9a-fA-F]+) bytes=(\d+) " + re.escape(path) + r"$",
                                cursor, timeout)
        return {"path": path, "bytes": int(match[2]), "crc32": int(match[1], 16)}


def page(state, expected):
    require(state["ui"].get("page") == expected and state["ui"].get("showing") == 1,
            "GUI 页面不符: " + repr(state["ui"]))


def idle_save(state):
    return m.idle_backup(state)


def is_paused(state):
    return (state["ui"].get("enabled") == 1 and state["ui"].get("showing") == 1 and
            state["ui"].get("page") == 1 and state["ui"].get("loaded") == 1 and
            state["core"].get("running") == 0)


def is_library(state):
    return (state["ui"].get("showing") == 1 and state["ui"].get("page") == 0 and
            state["ui"].get("loaded") == 0 and state["core"].get("running") == 0 and idle_save(state))


def applied(console, step, **values):
    state = console.status(step)
    page(state, 4)
    for field, value in values.items():
        m.require(state["playback"].get(field) == value,
                  f"{step}: expected {field}={value}: {state['playback']}")
    if "volume" in values:
        m.require(state["audio"].get("volume") == values["volume"] and
                  state["audio"].get("muted") == 1,
                  f"{step}: volume/mute readback mismatch")
    return state


def running(console, step, speed, skip, seconds=4):
    console.idle(seconds)
    state = console.status(step)
    m.require(state["core"].get("running") == 1 and state["ui"].get("showing") == 0 and
              state["ui"].get("loaded") == 1, step + ": game is not running")
    m.require(state["playback"].get("speed") == speed and state["playback"].get("frameskip") == skip,
              step + ": wrong playback settings")
    m.require(state["game_stats"].get("valid") == 1 and state["game_stats"].get("running") == 1,
              step + ": no completed FPS measurement")
    m.require(state["core"].get("audio_requested") == 1, step + ": sound preference was lost")
    if speed == 1:
        m.require(58 <= state["game_stats"]["fps"] <= 61, step + ": 1x FPS outside58..61")
        m.require(state["audio"].get("running") == 1 and state["audio"].get("muted") == 1 and
                  state["audio"].get("DMA_done", 0) > 0,
                  step + ": normal audio/DMA did not recover")
        if state["audio"].get("nonzero", 0) == 0:
            # Dragon Ball's opening can generate valid silent PCM for several
            # seconds. Give the game time, rather than treating silence as a
            # broken producer; every observation still enforces zero errors.
            console.pulse(0x008)  # One Start press, only while still silent.
            deadline = time.monotonic() + 14
            attempt = 0
            while state["audio"].get("nonzero", 0) == 0 and time.monotonic() < deadline:
                previous_dma = state["audio"]["DMA_done"]
                console.idle(min(2, max(0, deadline - time.monotonic())))
                attempt += 1
                state = console.status(f"{step}-audio-onset-{attempt}")
                m.require(state["core"].get("running") == 1 and state["audio"].get("running") == 1 and
                          state["audio"].get("muted") == 1 and state["audio"].get("DMA_done", 0) > previous_dma,
                          step + ": DMA/core stalled while waiting for game audio")
        m.require(state["audio"].get("nonzero", 0) > 0,
                  step + ": game produced no nonzero PCM after the opening grace period")
    else:
        m.require(state["audio"].get("running") == 0 and state["playback"].get("fast_audio_silent") == 1,
                  step + ": fast-forward audio was not stopped")
    return state


def run_options(console, args, report):
    inventory = console.listing(args.directory)
    report["inventory"] = inventory
    indices = [i for i, entry in enumerate(inventory)
               if not entry["directory"] and entry["name"] == args.rom_name]
    require(len(indices) == 1, "目标游戏未唯一出现在游戏库: " + args.rom_name)
    report["selected_index"] = indices[0]
    path = args.directory.rstrip("/") + "/" + args.rom_name
    report["rom"] = path
    report["load_seconds"] = launch_entry(console, args, inventory, indices[0])
    baseline = running(console, "normal-1x", 1, 0)
    require(baseline["ui"].get("path") == path, "启动了错误的游戏库项目")

    console.settings("settings-volume")  # First entry selects volume row0.
    console.pulse(RIGHT)
    applied(console, "volume100-clamp", volume=100)
    for expected in range(90, -1, -10):
        console.pulse(LEFT)
        applied(console, f"volume{expected}", volume=expected)
    for expected in range(10, 101, 10):
        console.pulse(RIGHT)
        applied(console, f"volume{expected}-restore", volume=expected)
    console.pulse(DOWN)  # Skip row1.
    for expected in (1, 2, 3, 0, 1):
        console.pulse(A)
        applied(console, f"skip{expected}", frameskip=expected)
    console.resume_settings()
    skipped = running(console, "skip1-running", 1, 1)
    emulated, drawn = skipped["playback"]["emulated"], skipped["playback"]["drawn"]
    m.require(drawn > 0 and 1.8 <= emulated / drawn <= 2.2,
              f"skip1 expected about2 simulated frames/drawn frame: {skipped['playback']}")

    console.settings("settings-speed2")  # The skip row1 remains selected.
    console.pulse(LEFT)
    applied(console, "skip0-restored", frameskip=0)
    console.pulse(DOWN)  # Speed row2.
    console.pulse(A)
    applied(console, "speed2-selected", speed=2)
    console.resume_settings()
    twice = running(console, "speed2-running", 2, 0)
    m.require(115 <= twice["game_stats"]["fps"] <= 125, "2x FPS outside115..125")

    console.settings("settings-speed4")  # Speed row2 remains selected.
    console.pulse(A)
    applied(console, "speed4-selected", speed=4)
    console.resume_settings()
    four = running(console, "speed4-running", 4, 0)
    fps4 = four["game_stats"]["fps"]
    report["four_x_target_met"] = 225 <= fps4 <= 245
    m.require(fps4 > baseline["game_stats"]["fps"] * 1.05, "4x did not accelerate beyond1x")
    if not report["four_x_target_met"]:
        report["notes"].append(f"4x selected, measured {fps4}FPS; target225..245 was not reached.")
    console.settings("settings-normal")
    console.pulse(A)
    applied(console, "speed1-selected", speed=1)
    console.resume_settings()
    running(console, "normal-audio-restored", 1, 0)

    before_save = console.pause("before-save")
    console.pulse(DOWN)
    console.pulse(A)
    deadline = time.monotonic() + 30
    while True:
        saved = console.status("save-completion")
        if saved["ui"].get("page") == 3 and saved["save"].get("completed", 0) > before_save["save"]["completed"]:
            break
        m.require(time.monotonic() < deadline, "save completion timeout")
        console.idle(0.2)
    m.require(saved["ui"].get("result") == 0 and saved["save"].get("last_result") == 0 and idle_save(saved),
              "manual GUI save did not complete cleanly")
    save_path = m.native_save_path(args, args.rom_name.rsplit(".", 1)[0] + ".sav")
    m.require(saved["save"].get("path") == save_path, "save name lost its complete Chinese basename")
    cursor = len(console.observed)
    console.send("crc " + save_path)
    crc = console.wait_since(r"CRC32=([0-9a-fA-F]+) bytes=(\d+) " + re.escape(save_path) + r"$", cursor, 30)
    m.require(int(crc[1], 16) == saved["save"]["crc32"] and int(crc[2]) == saved["save"]["bytes"],
              "full Chinese save file CRC/size mismatch")
    report["save_crc"] = {"path": save_path, "crc32": int(crc[1], 16), "bytes": int(crc[2])}
    console.pulse(B)  # Save notice -> pause.
    console.pulse(DOWN)
    console.pulse(DOWN)
    console.pulse(A)
    before_reload = console.status("reload-confirmation")
    page(before_reload, 3)
    m.require(before_reload["core"].get("running") == 0, "reload confirmation unexpectedly resumed")
    cursor = len(console.observed)
    begun = time.monotonic()
    console.pulse(A)
    reloaded = console.wait_since(r"SAVE reloaded cached_rom=1 path=(.+)$", cursor, 15)
    report["reload_seconds"] = time.monotonic() - begun
    m.require(reloaded[1] == path and report["reload_seconds"] < 8,
              "cached reload path/timing differs from expected native-save read")
    after_reload = running(console, "cached-save-reloaded", 1, 0)
    m.require(not any("load result=" in line for line in console.observed[cursor:]),
              "reload unexpectedly entered the full ROM loader")
    m.require(after_reload["clock"]["heap_free"] >= before_reload["clock"]["heap_free"],
              "cached save reload reduced free heap")
    report["notes"].append("Cached native-save reload/CRC verified; no claim of in-game progress restoration.")
    console.pause("before-exit")
    for _ in range(6):
        console.pulse(DOWN)
    console.pulse(A)
    console.idle(0.5)
    final = console.status("exit-library")
    page(final, 0)
    m.require(final["ui"].get("loaded") == 0 and final["core"].get("running") == 0 and
              final["audio"].get("running") == 0 and idle_save(final), "exit did not leave unloaded/idle library")


class TimingWindowMiss(RuntimeError):
    """The host reached the menu after the existing 2 s autosave deadline."""


def same_save_counts(before, after, description, *, timing=False):
    unchanged = all(after["save"].get(key) == before["save"].get(key)
                    for key in ("queued", "completed"))
    if not unchanged and timing:
        raise TimingWindowMiss(
            description + ": autosave occurred before GUI pause; host timing window missed, "
            "not a cached-ROM reload result; before=" + repr(before["save"]) +
            "; after=" + repr(after["save"]))
    require(unchanged, description + " unexpectedly submitted/completed a save: before=" +
            repr(before["save"]) + "; after=" + repr(after["save"]))


def guest_ready(state, kind):
    kind_id, capacity = m._roms.KINDS[kind]
    box = state.get("save_test", {})
    return (state["core"].get("running") == 1 and
            box.get("magic") == m._roms.MAILBOX_MAGIC and box.get("kind") == kind_id and
            box.get("bytes") == capacity and box.get("checks", 0) >= 1 and
            box.get("frames", 0) >= 1 and box.get("keys") == 0)


def wait_guest(console, kind, choice, count, writes, timeout):
    marker = m._roms.MARKERS[choice]

    def finished(state):
        box = state.get("save_test", {})
        return (guest_ready(state, kind) and box.get("marker") == marker and
                box.get("count") == count and box.get("writes") == writes and
                box.get("state") == (1 if choice == "A" else 2))

    state = console.until(finished, timeout, kind + " guest " + choice + " write/read-back")
    m.assert_guest(state, kind, choice, count, writes)
    return state


def exit_diagnostic(console, args):
    state = console.status()
    if state["ui"].get("loaded"):
        require(re.fullmatch(r"/__save_(?:sram|flash|eeprom)\.gba", state["ui"].get("path", "")),
                "refusing automatic exit/save of a user game: " + repr(state["ui"]))
        console.pause("diagnostic-pause", args.operation_timeout)  # menu resets pause selection to zero
        console.down(6)
        console.pulse(0x001)
    return console.until(is_library, args.operation_timeout, "empty library and synchronized save worker")


def test_reload_kind(console, kind, args, report):
    report["stage"] = "load"
    before = console.status()
    require(is_library(before), "diagnostic must start at an empty library")
    path = "/__save_" + kind + ".gba"
    report["rom"] = path
    console.send("load " + path)
    loaded = console.wait(r"load result=(-?\d+)", args.load_timeout)
    require(int(loaded[1]) == 0, kind + " diagnostic load failed")
    console.send("keys 000")
    console.pause("diagnostic-pause", args.operation_timeout)
    console.pulse(0x002)  # GUI Resume; deliberately do not use CLI run
    boot = console.until(lambda s: guest_ready(s, kind), args.operation_timeout,
                         kind + " diagnostic read-only boot")
    report["boot"] = boot
    box = boot["save_test"]
    require(box.get("writes") == 0 and box.get("state") in (0, 1, 2) and
            box.get("mismatch") == 0xFFFFFFFF and m.idle_backup(boot),
            kind + " diagnostic backup is invalid or changed on boot: " + repr(box))
    if box["state"] == 0:
        require(box.get("marker") == 0xFFFFFFFF and box.get("count") == 0xFFFFFFFF,
                kind + " blank backup marker/count mismatch")
        baseline_count = 0
    else:
        initial_choice = "A" if box["state"] == 1 else "B"
        require(box.get("boot_marker") == m._roms.MARKERS[initial_choice] and
                box.get("boot_count") == box.get("count") and box.get("count", 0) >= 1,
                kind + " persisted backup boot metadata mismatch")
        baseline_count = box["boot_count"]
    require(baseline_count < 0xFFFFFFFD, "diagnostic counter has insufficient room")
    report["baseline_count"] = baseline_count
    same_save_counts(before, boot, kind + " load/read-only boot")

    report["stage"] = "save-A"
    m.press(console, "A")
    written_a = wait_guest(console, kind, "A", baseline_count + 1, 1, args.operation_timeout)
    report["written_A"] = written_a
    paused_a = console.pause("diagnostic-pause", args.operation_timeout)
    report["paused_A"] = paused_a
    same_save_counts(boot, paused_a, kind + " A pause", timing=True)
    require(paused_a["backup"].get("dirty") == 1 and
            paused_a["backup"].get("in_flight") == 0 and paused_a["save"].get("busy") == 0,
            kind + " guest A is not an unsaved, stable snapshot: " + repr(paused_a["backup"]))
    console.down(1)
    console.pulse(0x001)

    def saved_a(state):
        return (state["ui"].get("showing") == 1 and state["ui"].get("page") == 3 and
                state["ui"].get("result") == 0 and m.idle_backup(state) and
                state["save"].get("completed") == boot["save"]["completed"] + 1)

    saved = console.until(saved_a, args.operation_timeout, kind + " GUI save success notice")
    report["saved_A"] = saved
    size, crc = m.expected_crc(kind, "A", baseline_count + 1)
    require(saved["save"].get("queued") == boot["save"]["queued"] + 1 and
            saved["save"].get("last_result") == 0 and saved["save"].get("bytes") == size and
            saved["save"].get("crc32") == crc and
            saved["save"].get("path") == m.native_save_path(args, "__save_" + kind + ".sav"),
            kind + " GUI saved image differs from guest A: " + repr(saved["save"]))
    report["card_A_after_save"] = m.check_card(console, kind, "A", baseline_count + 1, args)
    console.pulse(0x002)  # notice -> pause, resets selection to zero
    require(is_paused(console.status()), kind + " save notice did not return to pause")
    console.pulse(0x002)  # pause -> gameplay
    resumed = console.until(lambda s: guest_ready(s, kind), args.operation_timeout, kind + " resumed A")
    m.assert_guest(resumed, kind, "A", baseline_count + 1, 1)

    report["stage"] = "unsaved-B"
    started_b = time.monotonic()
    m.press(console, "B")
    written_b = wait_guest(console, kind, "B", baseline_count + 2, 2, args.operation_timeout)
    # Do not log an extra status or full-file CRC between write and pause.
    paused_b = console.pause("diagnostic-pause", args.operation_timeout)
    report["write_B_to_pause_seconds"] = time.monotonic() - started_b
    report["written_B"] = written_b
    report["paused_B"] = paused_b
    same_save_counts(saved, paused_b, kind + " B pause", timing=True)
    m.assert_guest(paused_b, kind, "B", baseline_count + 2, 2)
    require(paused_b["backup"].get("dirty") == 1 and
            paused_b["backup"].get("in_flight") == 0 and
            paused_b["backup"].get("requested") == 0 and paused_b["save"].get("busy") == 0,
            kind + " B did not remain unsaved on entering the menu: " + repr(paused_b["backup"]))
    report["card_A_while_B_paused"] = m.check_card(console, kind, "A", baseline_count + 1, args)

    report["stage"] = "GUI-read-confirmation"
    console.down(2)
    console.pulse(0x001)
    confirm = console.status()
    report["confirmation"] = confirm
    require(confirm["ui"].get("page") == 3 and confirm["ui"].get("showing") == 1 and
            confirm["core"].get("running") == 0 and confirm["backup"].get("dirty") == 1,
            kind + " GUI read confirmation did not retain paused unsaved B")
    same_save_counts(saved, confirm, kind + " read confirmation")
    m.assert_guest(confirm, kind, "B", baseline_count + 2, 2)
    # pulse() already released the first A. Capture the reload event before
    # idle() can consume it; release the second A even if waiting fails.
    console.send("keys 001")
    try:
        event = console.wait(r"SAVE reloaded cached_rom=1 path=" + re.escape(path) + r"$",
                             args.operation_timeout)
        report["reload_event"] = event[0]
    finally:
        console.send("keys 000")
    report["stage"] = "boot-A-after-reload"
    reloaded = wait_guest(console, kind, "A", baseline_count + 1, 0, args.operation_timeout)
    report["reloaded_A"] = reloaded
    require(reloaded["ui"].get("showing") == 0 and reloaded["core"].get("running") == 1 and
            reloaded["save_test"].get("boot_marker") == m._roms.MARKERS["A"] and
            reloaded["save_test"].get("boot_count") == baseline_count + 1 and m.idle_backup(reloaded),
            kind + " cached-ROM restart did not boot persisted A with writes=0")
    same_save_counts(saved, reloaded, kind + " cached-ROM read/restart")
    report["card_A_after_reload"] = m.check_card(console, kind, "A", baseline_count + 1, args)
    report["stage"] = "normal-exit"
    final = exit_diagnostic(console, args)
    same_save_counts(saved, final, kind + " read-only reboot/normal exit")
    report["final"] = final
    report["stage"] = "complete"
    report["pass"] = True
    print("PASS " + kind + ": saved A/" + str(baseline_count + 1) +
          " -> unsaved B/" + str(baseline_count + 2) +
          " -> GUI cached-ROM boot A, writes=0, full-file CRC unchanged", flush=True)


def launch_entry(console, args, inventory, index):
    # L 每次退 8 项，重复足够次数回到第一项，支持多页列表。
    for _ in range((len(inventory) + 7) // 8):
        console.pulse(L)
    console.down(index)
    cursor = len(console.observed)
    started = time.monotonic()
    console.pulse(A)
    loaded = console.wait_since(r"load result=(-?\d+) errno=", cursor, args.load_timeout)
    require(int(loaded[1]) == 0, "GUI ROM 加载失败")
    elapsed = time.monotonic() - started
    console.idle(2)
    state = console.status("launched-" + inventory[index]["name"])
    path = args.directory.rstrip("/") + "/" + inventory[index]["name"]
    require(state["ui"].get("path") == path and state["ui"].get("loaded") == 1 and
            state["ui"].get("showing") == 0 and state["core"].get("running") == 1,
            "GUI 启动路径或运行状态不符: " + repr(state["ui"]))
    require(state["game_stats"].get("valid") == 1 and 58 <= state["game_stats"]["fps"] <= 61,
            "正常速度 FPS 不在 58..61")
    require(state["audio"].get("running") == 1 and state["audio"].get("DMA_done", 0) > 0,
            "静音游戏的音频 DMA 未运行")
    # 此处允许合法静音的开场；options 和独立长测另验证非零 PCM。
    return elapsed


def exit_game(console, args, step="exit-library"):
    console.pause("before-" + step, args.operation_timeout)
    console.down(6)
    console.pulse(A)
    return console.until(is_library, args.operation_timeout, step)


def check_large_reload(console, args, report):
    before = console.pause("large-paused", args.operation_timeout)
    path = before["ui"]["path"]
    save_path = m.native_save_path(args, path.rsplit("/", 1)[1].rsplit(".", 1)[0] + ".sav")
    original = console.crc(save_path, args.operation_timeout)
    require(original["bytes"] == 131072, "大 ROM 检查要求既有 128KiB Flash 原生存档")
    console.down(2)
    console.pulse(A)
    confirmation = console.status("large-reload-confirmation")
    page(confirmation, 3)
    require(confirmation["core"].get("running") == 0, "读档确认页不应运行游戏")
    cursor = len(console.observed)
    started = time.monotonic()
    console.pulse(A)
    console.wait_since(r"SAVE reloaded cached_rom=1 path=" + re.escape(path) + r"$",
                       cursor, args.operation_timeout)
    report["reload_seconds"] = time.monotonic() - started
    require(report["reload_seconds"] < 8, "缓存读档时间超过原有 8 秒门限")
    require(console.crc(save_path, args.operation_timeout) == original, "缓存读档改变了存档文件")
    console.idle(2)
    after = console.status("large-reloaded")
    require(after["core"].get("running") == 1 and after["ui"].get("path") == path and
            after["audio"].get("running") == 1, "大 ROM 读档后没有恢复游戏和静音 DMA")
    require(after["clock"]["heap_free"] >= before["clock"]["heap_free"],
            "128KiB 暂存区未释放")
    require(not any("load result=" in line for line in console.observed[cursor:]),
            "缓存读档意外重读了整个 ROM")
    report.update({"path": path, "save": original, "free_heap": after["clock"]["heap_free"]})


def run_large(console, args, report):
    initial = console.status("large-initial")
    inventory = console.listing(args.directory)
    report["inventory"] = inventory
    large = next((i for i, e in enumerate(inventory)
                  if not e["directory"] and e["bytes"] == 33554432), None)
    require(large is not None, "游戏库没有 32MiB ROM")
    report["load_seconds"] = launch_entry(console, args, inventory, large)
    check_large_reload(console, args, report)
    final = exit_game(console, args, "large-exit")
    report["heap_retained_bytes"] = initial["clock"]["heap_free"] - final["clock"]["heap_free"]
    require(report["heap_retained_bytes"] <= 512, "大 ROM 退出后存在额外堆占用")


def run_switch(console, args, report):
    initial = console.status("switch-initial")
    inventory = console.listing(args.directory)
    report["inventory"] = inventory
    small = next((i for i, e in enumerate(inventory)
                  if not e["directory"] and 0 < e["bytes"] < 12 * 1024 * 1024), None)
    dragon = next((i for i, e in enumerate(inventory)
                   if not e["directory"] and e["name"] == args.rom_name), None)
    large = next((i for i, e in enumerate(inventory)
                  if not e["directory"] and e["bytes"] == 33554432), None)
    require(None not in (small, dragon, large), "切换检查需要小 ROM、指定龙珠 ROM 和 32MiB ROM")
    sequence = report["sequence"] = []
    for number, index in enumerate((small, dragon, large, small)):
        if number:
            console.pause("switch-pause", args.operation_timeout)
            console.down(4)
            console.pulse(A)
            state = console.status("switch-library")
            page(state, 0)
            require(state["ui"].get("loaded") == 1 and idle_save(state),
                    "切换游戏库时保存同步或旧 ROM 状态不符")
        seconds = launch_entry(console, args, inventory, index)
        sequence.append({"name": inventory[index]["name"], "bytes": inventory[index]["bytes"],
                         "load_seconds": seconds})
        if number == 1:
            running(console, "switch-dragon-nonzero-pcm", 1, 0)
        elif number == 2:
            report["large"] = {}
            check_large_reload(console, args, report["large"])
    final = exit_game(console, args, "switch-exit")
    report["heap_retained_bytes"] = initial["clock"]["heap_free"] - final["clock"]["heap_free"]
    require(report["heap_retained_bytes"] <= 512, "四次 GUI 启动后 ROM 堆未回收")


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("options", "reload", "large", "switch"), required=True)
    parser.add_argument("--port", default="COM52")
    parser.add_argument("--directory", default="/gba_game", help="GUI 当前显示的游戏目录")
    parser.add_argument("--rom-name", default="龙珠大冒险.gba")
    parser.add_argument("--save-dir", type=m.save_directory, default="/GBA/SAVES",
                        help="原生存档目录；mGBA 独立构建使用 /GBA/SAVES/mGBA")
    parser.add_argument("--kind", choices=tuple(m._roms.KINDS), action="append",
                        help="reload 模式可重复指定，默认三类全部检查")
    parser.add_argument("--load-timeout", type=float, default=180)
    parser.add_argument("--operation-timeout", type=float, default=30)
    parser.add_argument("--log", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    for field in ("load_timeout", "operation_timeout"):
        if not math.isfinite(getattr(args, field)) or getattr(args, field) <= 0:
            parser.error("超时必须为有限正数")
    if any("\r" in s or "\n" in s for s in (args.directory, args.rom_name)):
        parser.error("目录与文件名必须为单行 UTF-8 文本")
    path = args.directory.rstrip("/") + "/" + args.rom_name
    if not args.directory.startswith("/") or len(path.encode("utf-8")) >= 512:
        parser.error("需要绝对 TF 路径，组合 ROM 路径必须小于 512 个 UTF-8 字节")
    try:
        m.native_save_path(args, args.rom_name.rsplit(".", 1)[0] + ".sav")
        for kind in args.kind or tuple(m._roms.KINDS):
            m.native_save_path(args, "__save_" + kind + ".sav")
    except argparse.ArgumentTypeError as error:
        parser.error(str(error))
    prefix = ROOT / "build" / ("engine-gui-" + args.mode + "-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    log_path = args.log or prefix.with_suffix(".log")
    json_path = args.json or log_path.with_suffix(".json")
    require(log_path.resolve() != json_path.resolve(), "日志和 JSON 需要不同路径")
    require(not log_path.exists() and not json_path.exists(), "请选择新日志路径，保留旧验收证据")
    log_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.parent.mkdir(parents=True, exist_ok=True)
    report = {"pass": False, "mode": args.mode, "port": args.port, "records": [], "notes": [],
              "save_dir": args.save_dir,
              "started": datetime.now().astimezone().isoformat(), "log": str(log_path),
              "scope": "静音 GUI、原生存档文件及缓存重启；不验证具体剧情点恢复"}
    uart = serial.Serial(port=None, baudrate=115200, timeout=0.03, write_timeout=3)
    uart.dtr = uart.rts = False
    uart.port = args.port
    try:
        with log_path.open("xb", buffering=0) as output:
            console = Console(uart, output, report)
            console.operation_timeout = args.operation_timeout
            try:
                uart.open()
                console.send("audio silent")
                console.send("keys 000")
                console.idle(0.2)
                initial = console.status("initial-muted")
                report["initial"] = initial
                require(is_library(initial), "请先正常退出游戏，停在空载 GUI 游戏库")
                require(initial["playback"].get("speed") == 1 and
                        initial["playback"].get("frameskip") == 0,
                        "前一轮必须恢复 speed=1 / frameskip=0")
                if args.mode == "options":
                    require(initial["playback"].get("volume") == 100, "设置检查从默认音量 100 开始")
                    run_options(console, args, report)
                elif args.mode == "reload":
                    report["kinds"] = []
                    for kind in args.kind or tuple(m._roms.KINDS):
                        item = {"kind": kind, "pass": False}
                        report["kinds"].append(item)
                        test_reload_kind(console, kind, args, item)
                elif args.mode == "large":
                    run_large(console, args, report)
                else:
                    run_switch(console, args, report)
                require(is_library(console.status("completed-library")), "检查结束后未正常卸载到游戏库")
                report["pass"] = True
            except (Exception, KeyboardInterrupt) as error:
                report.update({"error": str(error), "error_type": type(error).__name__,
                               "timing_window_missed": isinstance(error, TimingWindowMiss)})
            finally:
                if uart.is_open:
                    console.strict = False
                    console.ignore_failures = True
                    try:
                        console.send("audio silent")
                        console.send("keys 000")
                        console.send("stop")
                        console.send("audio off")
                        console.idle(0.3)
                        final = console.until(
                            lambda s: s["core"].get("running") == 0 and
                            s["audio"].get("running") == 0 and s["audio"].get("muted") == 1 and idle_save(s),
                            args.operation_timeout, "final-stopped-muted-flushed")
                        report["final"] = final
                        if report["pass"]:
                            console.check_status(final)
                    except (Exception, KeyboardInterrupt) as error:
                        report["pass"] = False
                        report["cleanup_error"] = str(error)
    finally:
        uart.close()
        report["finished"] = datetime.now().astimezone().isoformat()
        json_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(("GUI PASS " if report["pass"] else "GUI FAIL ") + str(json_path), flush=True)
    for field in ("error", "cleanup_error"):
        if field in report:
            print(report[field], flush=True)
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
