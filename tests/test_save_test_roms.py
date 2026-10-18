"""Execute the generated ARM diagnostics against independent backup devices.

This narrow ARM interpreter is a software check for the original diagnostic ROMs,
not a substitute for running them in the actual ReGBA JIT on the board.
"""

import importlib.util
from pathlib import Path
import struct
import unittest

SPEC = importlib.util.spec_from_file_location(
    "save_roms", Path(__file__).resolve().parents[1] / "scripts/make-save-test-roms.py")
ROMS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ROMS)
MASK = 0xFFFFFFFF


class Backup:
    def __init__(self, kind, image=None):
        self.kind = kind
        self.data = bytearray(image if image is not None else b"\xff" * ROMS.KINDS[kind][1])
        self.writes = 0
        self.flash_mode = "base"
        self.flash_unlock = 0
        self.bits = []
        self.read_bits = []

    def read(self, address):
        if self.kind != "eeprom":
            if self.flash_mode == "id":
                return (0xC2, 0x1C)[address & 1]
            return self.data[address & 0xFFFF]
        if self.read_bits:
            return self.read_bits.pop(0)
        return 1

    def write(self, address, value):
        value &= 255
        if self.kind == "sram":
            self.data[address] = value
            self.writes += 1
            return
        if self.kind == "flash":
            if self.flash_mode == "program":
                self.data[address] = value
                self.writes += 1
                self.flash_mode = "base"
                return
            if self.flash_unlock == 0 and address == 0x5555 and value == 0xAA:
                self.flash_unlock = 1
            elif self.flash_unlock == 1 and address == 0x2AAA and value == 0x55:
                self.flash_unlock = 2
            elif self.flash_unlock == 2 and address == 0x5555:
                self.flash_unlock = 0
                if value == 0x90:
                    self.flash_mode = "id"
                elif value == 0xF0:
                    self.flash_mode = "base"
                elif value == 0x80:
                    self.flash_mode = "erase"
                elif value == 0x10 and self.flash_mode == "erase":
                    self.data[:] = b"\xff" * len(self.data)
                    self.writes += 1
                    self.flash_mode = "base"
                elif value == 0xA0:
                    self.flash_mode = "program"
                else:
                    raise AssertionError(f"Bad FLASH command {value:#x}")
            else:
                raise AssertionError(f"Bad FLASH unlock {address:#x}={value:#x}")
            return
        self.bits.append(value & 1)
        if len(self.bits) < 8:
            return
        reading = self.bits[1] == 1
        if len(self.bits) != (9 if reading else 73):
            return
        bits, self.bits = self.bits, []
        assert bits[0] == 1 and bits[-1] == 0
        block = sum(bit << (5 - index) for index, bit in enumerate(bits[2:8]))
        offset = block * 8
        if reading:
            self.read_bits = [0] * 4 + [
                (byte >> bit) & 1 for byte in self.data[offset:offset + 8]
                for bit in range(7, -1, -1)]
        else:
            for index in range(8):
                self.data[offset + index] = sum(
                    bit << (7 - n) for n, bit in enumerate(bits[8 + index * 8:16 + index * 8]))
            self.writes += 8


class ArmMachine:
    def __init__(self, kind, image=None):
        self.kind = kind
        self.rom = ROMS.save_test_rom(kind)
        self.ram = bytearray(256 * 1024)
        self.iwram = bytearray(32768)
        self.vram = bytearray(128 * 1024)
        self.io = bytearray(1024)
        self.backup = Backup(kind, image)
        self.registers = [0] * 16
        self.registers[15] = 0x08000000
        self.keys = 0
        self.vcount_phase = 0
        self.n = self.z = self.c = self.v = False

    def read(self, address, size):
        if address == 0x04000006:
            value = 0 if self.vcount_phase % 2 == 0 else 160
            self.vcount_phase += 1
            return value
        if address == 0x04000130:
            return 0x3FF ^ self.keys
        if address >> 24 in (0x0D, 0x0E):
            return self.backup.read(address & 0xFFFF)
        area, index = self.area(address)
        return int.from_bytes(area[index:index + size], "little")

    def write(self, address, value, size):
        if address >> 24 in (0x0D, 0x0E):
            self.backup.write(address & 0xFFFF, value)
            return
        area, index = self.area(address)
        area[index:index + size] = (value & ((1 << (size * 8)) - 1)).to_bytes(size, "little")

    def area(self, address):
        region = address >> 24
        if region == 2:
            return self.ram, address & 0x3FFFF
        if region == 3:
            return self.iwram, address & 0x7FFF
        if region == 4:
            return self.io, address & 0x3FF
        if region == 6:
            return self.vram, address & 0x1FFFF
        if region == 8:
            return self.rom, address & 0x1FFFFFF
        raise AssertionError(f"Unsupported address {address:#x}")

    def reg(self, index):
        return (self.registers[15] + 8) & MASK if index == 15 else self.registers[index]

    def condition(self, condition):
        return {
            0: self.z, 1: not self.z, 3: not self.c, 5: not self.n,
            10: self.n == self.v, 11: self.n != self.v, 14: True,
        }[condition]

    def step(self):
        pc = self.registers[15]
        ins = self.read(pc, 4)
        next_pc = pc + 4
        if not self.condition(ins >> 28):
            self.registers[15] = next_pc
            return
        if ins & 0x0FFFFFF0 == 0x012FFF10:
            next_pc = self.reg(ins & 15)
        elif ins & 0x0E000000 == 0x0A000000:
            offset = ins & 0xFFFFFF
            if offset & 0x800000:
                offset -= 0x1000000
            if ins & 0x01000000:
                self.registers[14] = pc + 4
            next_pc = pc + 8 + offset * 4
        elif ins & 0x0E000000 == 0x08000000:
            regs = [r for r in range(16) if ins & (1 << r)]
            load = bool(ins & 0x00100000)
            address = self.registers[13] if load else self.registers[13] - 4 * len(regs)
            self.registers[13] += (4 if load else -4) * len(regs)
            for reg in regs:
                if load:
                    value = self.read(address, 4)
                    if reg == 15:
                        next_pc = value
                    else:
                        self.registers[reg] = value
                else:
                    self.write(address, self.reg(reg), 4)
                address += 4
        elif ins & 0x0C000000 == 0x04000000 or ins & 0x0E000090 == 0x00000090:
            half = ins & 0x0C000000 != 0x04000000
            size = 2 if half else (1 if ins & 0x00400000 else 4)
            offset = (((ins >> 4) & 0xF0) | (ins & 15)) if half else ins & 0xFFF
            base = (ins >> 16) & 15
            reg = (ins >> 12) & 15
            before = bool(ins & 0x01000000)
            address = self.reg(base)
            updated = address + (offset if ins & 0x00800000 else -offset)
            if before:
                address = updated
            if ins & 0x00100000:
                value = self.read(address, size)
                if reg == 15:
                    next_pc = value
                else:
                    self.registers[reg] = value
            else:
                self.write(address, self.reg(reg), size)
            if not before or ins & 0x00200000:
                self.registers[base] = updated & MASK
        elif ins & 0x0C000000 == 0:
            opcode = (ins >> 21) & 15
            src = self.reg((ins >> 16) & 15)
            if ins & 0x02000000:
                operand = ins & 255
                amount = ((ins >> 8) & 15) * 2
                operand = ((operand >> amount) | (operand << (32 - amount))) & MASK
            else:
                operand = self.reg(ins & 15)
                amount = (self.reg((ins >> 8) & 15) & 255) if ins & 16 else ((ins >> 7) & 31)
                shift = (ins >> 5) & 3
                if shift == 0:
                    operand = (operand << amount) & MASK
                elif shift == 1:
                    operand >>= amount if amount or ins & 16 else 32
                else:
                    raise AssertionError(f"Unsupported shift {ins:#x}")
            if opcode in (0, 8):
                result = src & operand
            elif opcode in (2, 10):
                result = (src - operand) & MASK
            elif opcode == 4:
                result = (src + operand) & MASK
            elif opcode == 12:
                result = src | operand
            elif opcode == 13:
                result = operand
            elif opcode == 14:
                result = src & ~operand & MASK
            elif opcode == 15:
                result = ~operand & MASK
            else:
                raise AssertionError(f"Unsupported ALU {ins:#x}")
            if ins & 0x00100000:
                self.n, self.z = bool(result & 0x80000000), result == 0
                if opcode in (2, 10):
                    self.c = src >= operand
                    self.v = bool(((src ^ operand) & (src ^ result)) & 0x80000000)
            if opcode not in (8, 10):
                self.registers[(ins >> 12) & 15] = result
        else:
            raise AssertionError(f"Unsupported ARM {ins:#x} at {pc:#x}")
        self.registers[15] = next_pc & MASK

    def mailbox(self):
        return dict(zip(ROMS.MAILBOX_FIELDS, struct.unpack_from("<16I", self.ram)))

    def frames(self, target):
        for _ in range(8_000_000):
            if self.mailbox()["frames"] >= target:
                return
            self.step()
        raise AssertionError(f"ROM stuck at {self.registers[15]:#x}, {self.mailbox()}")


class SaveRomTests(unittest.TestCase):
    def test_all_devices_read_only_boot_edges_overwrite_and_reload(self):
        for kind in ROMS.KINDS:
            with self.subTest(kind=kind):
                machine = ArmMachine(kind)
                machine.frames(2)
                self.assertEqual(machine.mailbox()["magic"], ROMS.MAILBOX_MAGIC)
                self.assertEqual(machine.mailbox()["state"], 0)
                self.assertEqual(machine.backup.writes, 0)
                machine.keys = 1
                machine.frames(4)
                self.assertEqual(machine.mailbox()["state"], 1)
                self.assertEqual(machine.mailbox()["write_events"], 1)
                self.assertEqual(machine.backup.data, ROMS.expected_save(kind, "A", 1))
                writes = machine.backup.writes
                machine.frames(8)
                self.assertEqual(machine.backup.writes, writes, "Holding A must not rewrite")
                machine.keys = 0
                machine.frames(10)
                machine.keys = 2
                machine.frames(12)
                self.assertEqual(machine.mailbox()["state"], 2)
                self.assertEqual(machine.mailbox()["write_events"], 2)
                self.assertEqual(machine.backup.data, ROMS.expected_save(kind, "B", 2))
                self.assertEqual(machine.mailbox()["boot_marker"], MASK)
                restored = ArmMachine(kind, bytes(machine.backup.data))
                restored.frames(2)
                self.assertEqual(restored.mailbox()["state"], 2)
                self.assertEqual(restored.mailbox()["count"], 2)
                self.assertEqual(restored.mailbox()["boot_marker"], ROMS.MARKERS["B"])
                self.assertEqual(restored.backup.writes, 0)
                self.assertEqual(restored.mailbox()["mismatch_offset"], MASK)
                restored.keys = 1
                restored.frames(4)
                self.assertEqual(restored.backup.data, ROMS.expected_save(kind, "A", 3))

    def test_full_capacity_read_detects_tail_corruption(self):
        for kind in ROMS.KINDS:
            with self.subTest(kind=kind):
                image = bytearray(ROMS.expected_save(kind, "A", 7))
                image[-1] ^= 1
                machine = ArmMachine(kind, image)
                machine.frames(2)
                self.assertEqual(machine.mailbox()["state"], 3)
                self.assertEqual(machine.mailbox()["mismatch_offset"], len(image) - 4)
                self.assertEqual(machine.backup.writes, 0)


if __name__ == "__main__":
    unittest.main()
