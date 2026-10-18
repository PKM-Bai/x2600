"""Prepare the pinned headless core; all derived sources stay in the build tree."""
import argparse
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import re
import runpy

PIN = '7f379052dc14366e622a96c7cf7fb6b5a7e918a3'


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('Pinned-source patch no longer matches: ' + old[:80])
    return text.replace(old, new, 1)


def test_rom():
    # Original diagnostic, no commercial ROM/header artwork. Skip BIOS boot.
    rom = bytearray([0xFF] * 32768)
    def word(off, value): struct.pack_into('<I', rom, off, value)
    word(0, 0xEA00002E)  # ARM B 0x080000C0
    rom[0xA0:0xAC] = b'X2600 TEST  '
    rom[0xAC:0xB2] = b'X26T00'
    rom[0xB2] = 0x96
    rom[0xBD] = (-sum(rom[0xA0:0xBD]) - 0x19) & 255
    pos, literals = 0xC0, []
    def emit(value):
        nonlocal pos
        word(pos, value)
        pos += 4
    def literal(reg, value):
        literals.append((pos, reg, value))
        emit(0)
    emit(0xE3A08402)  # mov r8,#0x02000000 mailbox
    emit(0xE3A0002A)
    emit(0xE5880000)
    literal(0, 0x08000181)
    emit(0xE12FFF10)
    # Thumb: mov r1,#55; ldr r0,[pc,#4]; bx r0; nop; .word ARM continuation
    struct.pack_into('<HHHHI', rom, 0x180, 0x2137, 0x4801, 0x4700, 0x46C0, 0x08000200)
    pos = 0x200
    emit(0xE5881004)
    emit(0xE3A09403)  # mov r9,#0x03000000
    literal(0, 0xE3A00042)  # mov r0,#66
    emit(0xE5890000)
    literal(0, 0xE12FFF1E)  # bx lr
    emit(0xE5890004)
    emit(0xE28FE000)  # adr lr, after bx
    emit(0xE12FFF19)
    emit(0xE5880008)
    literal(0, 0xE3A0004D)  # modify translated RAM code: mov r0,#77
    emit(0xE5890000)
    emit(0xE28FE000)
    emit(0xE12FFF19)
    emit(0xE588000C)
    emit(0xE3A0002A)
    emit(0xE3A01007)
    emit(0xEF060000)  # BIOS/HLE Div
    emit(0xE5880010)
    emit(0xE3A00404)  # DISPCNT
    literal(1, 0x0403)  # mode3, BG2
    emit(0xE1C010B0)
    emit(0xE3A00406)  # VRAM
    literal(1, 0x3DEF)
    literal(2, 240 * 160)
    loop = pos
    emit(0xE0C010B2)  # strh r1,[r0],#2
    emit(0xE2522001)
    emit(0x1A000000 | (((loop - (pos + 8)) // 4) & 0xFFFFFF))
    literal(0, 0x600D)
    emit(0xE5880014)
    literal(0, 0x03007FFC)
    literal(1, 0x08000400)
    emit(0xE5801000)  # BIOS IRQ dispatcher handler pointer
    literal(0, 0x04000004)
    emit(0xE3A01008)
    emit(0xE1C010B0)  # DISPSTAT VBlank IRQ enable
    literal(0, 0x04000200)
    emit(0xE3A01001)
    emit(0xE1C010B0)  # IE = VBlank
    emit(0xE1C010B8)  # IME = 1
    emit(0xEAFFFFFE)
    for off, reg, value in literals:
        delta = pos - (off + 8)
        assert 0 <= delta <= 4095
        word(off, 0xE59F0000 | reg << 12 | delta)
        word(pos, value)
        pos += 4
    assert pos < 0x400
    # BIOS preserves r0/r1 for us; acknowledge IF and count delivered IRQs.
    for i, instruction in enumerate([
            0xE3A00402, 0xE5901018, 0xE2811001, 0xE5801018,
            0xE3A00404, 0xE2800C02, 0xE3A01001, 0xE1C010B2, 0xE12FFF1E]):
        word(0x400 + i * 4, instruction)
    return bytes(rom)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--eeprom-dma', action='store_true',
                        help='Use the DMA3 EEPROM diagnostic for the mGBA backend')
    parser.add_argument('--flash-poll', action='store_true',
                        help='Poll Flash completion in the mGBA diagnostic ROM')
    args = parser.parse_args()
    pin = subprocess.check_output(['git', '-C', str(args.source), 'rev-parse', 'HEAD'], text=True).strip()
    if pin != PIN:
        raise RuntimeError(f'ReGBA must be pinned at {PIN}, found {pin}')
    if subprocess.check_output(['git', '-C', str(args.source), 'status', '--porcelain'], text=True).strip():
        raise RuntimeError('ReGBA checkout has local changes; preserve them and use a clean pinned source')
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    src = args.source / 'source'
    for path in list(src.glob('*.h')) + [src / (name + '.c') for name in
            ['video', 'input', 'bios', 'sound', 'stats', 'memory', 'cpu_common', 'cpu_asm', 'sha1']]:
        shutil.copyfile(path, out / path.name)
    (out / 'mips').mkdir(exist_ok=True)
    for path in (src / 'mips').iterdir():
        if path.is_file(): shutil.copyfile(path, out / 'mips' / path.name)
    # ZIP decoding is excluded in v1; the core's ZIP entry returns ENOTSUP.
    zip_h = (out / 'zip.h').read_text(encoding='latin1')
    (out / 'zip.h').write_text(zip_h.replace('#include <zlib.h>', '/* Raw .gba only: no zlib dependency. */'))
    common_path = out / 'common.h'
    common = common_path.read_text(encoding='latin1')
    common = replace_once(common, '#include <dirent.h>', '/* Directory listing belongs to the FatFs task. */')
    # A translator frame is about 10.5KiB on this o32 compiler. Keep direct
    # linking within a bounded depth; deeper exits use the existing indirect
    # branch stubs and compile their target when execution reaches it. The
    # emitter and external-exit collector MUST use the same predicate.
    common += '''
/* X2600: bound recursive static linking on the FreeRTOS task stack. */
extern uint32_t translation_recursion_level;
extern uint32_t translation_recursion_peak;
#define REGBA_MAX_TRANSLATION_DEPTH 8u
#define REGBA_CAN_LINK_STATIC_TARGET(address) \\
  (translation_recursion_level < REGBA_MAX_TRANSLATION_DEPTH && \\
   ((address) < 0x00004000u || \\
    ((address) >= 0x08000000u && (address) < 0x0E000000u)))
'''
    common_path.write_text(common, encoding='latin1')
    emitter_path = out / 'mips/emit.h'
    emitter = emitter_path.read_text(encoding='latin1')
    lines = emitter.splitlines(keepends=True)
    first = next(i for i, line in enumerate(lines)
                 if 'if ((new_pc >= block_start_pc && new_pc < block_end_pc)' in line)
    old_condition = ''.join(lines[first:first + 3])
    assert emitter.count(old_condition) == 2
    emitter = emitter.replace(old_condition,
        '      if ((new_pc >= block_start_pc && new_pc < block_end_pc) \\\n'
        '       || REGBA_CAN_LINK_STATIC_TARGET(new_pc)) { \\\n')
    emitter_path.write_text(emitter, encoding='latin1')
    cpu_path = out / 'cpu_asm.c'
    cpu = cpu_path.read_text(encoding='latin1')
    lines = cpu.splitlines(keepends=True)
    first = next(i for i, line in enumerate(lines)
                 if 'if (branch_target < 0x00004000 /* BIOS */' in line)
    cpu = replace_once(cpu, ''.join(lines[first:first + 2]),
        '      if (REGBA_CAN_LINK_STATIC_TARGET(branch_target)) \\\n')
    cpu = replace_once(cpu, 'uint32_t translation_recursion_level = 0;',
        'uint32_t translation_recursion_level = 0;\nuint32_t translation_recursion_peak = 0;')
    # One increment has different indentation in the ROM hash lookup path.
    cpu, increments = re.subn(r'(?m)^(\s*)translation_recursion_level\+\+;[^\n]*\n',
        lambda m: m[0] + m[1] +
        'if (translation_recursion_level > translation_recursion_peak) \\\n' + m[1] +
        '  translation_recursion_peak = translation_recursion_level; \\\n', cpu)
    assert increments == 3, increments
    cpu_path.write_text(cpu, encoding='latin1')
    memory_h = out / 'memory.h'
    memory_h.write_text(replace_once(memory_h.read_text(encoding='latin1'),
        'struct BIOS_DATA bios;', 'extern struct BIOS_DATA bios;'), encoding='latin1')
    memory = (out / 'memory.c').read_text(encoding='latin1')
    memory = '#include "battery_save.h"\n' + memory
    memory = replace_once(memory, '    if(backup_type == BACKUP_SRAM)\n      gamepak_backup[0x5555] = value;',
        '    if(backup_type == BACKUP_SRAM) {\n      gamepak_backup[0x5555] = value;\n      backup_update = write_backup_delay;\n    }')
    memory = replace_once(memory, 'static int32_t load_gamepak_raw(const char* name);',
                          'static ssize_t load_gamepak_raw(const char* name);')
    old = '''\t\t\t// Read in just enough for the header
\t\t\tgamepak_file_large = gamepak_file;
\t\t\tgamepak_ram_buffer_size = ReGBA_AllocateOnDemandBuffer((void**) &gamepak_rom);
\t\t\tFILE_READ(gamepak_file, gamepak_rom, 0x100);'''
    memory = replace_once(memory, old, '''\t\t\t/* This port loads one complete ROM; do not page through SPI. */
\t\t\tFILE_CLOSE(gamepak_file);
\t\t\treturn -1;''')
    memory = replace_once(memory, 'gamepak_ram_buffer_size = gamepak_size;',
                          'gamepak_ram_buffer_size = (gamepak_size + 0x7FFF) & ~0x7FFF;')
    memory = replace_once(memory, '\t\tupdate_backup_force();',
                          '\t\t/* Stage 0-4: explicit save command; no implicit SD writes. */')
    zip_start = memory.index('\t\tif ((magicbit[0] == 0x50)')
    zip_end = memory.index('\t\telse if (magicbit[3] == 0xEA)', zip_start)
    memory = memory[:zip_start] + '''\t\tif ((magicbit[0] == 0x50) && (magicbit[1] == 0x4B)) {
\t\t\terrno = ENOTSUP;
\t\t\treturn -1; /* No ZIP allocation or temporary files in this port. */
\t\t}
''' + memory[zip_end:]
    # The core task snapshots guest RAM; card I/O belongs to the save worker.
    # A malformed/unreadable save aborts loading instead of silently replacing it.
    backup_begin = memory.index('uint32_t load_backup()\n{')
    backup_end = memory.index('\nvoid update_backup()', backup_begin)
    memory = memory[:backup_begin] + '''uint32_t load_backup()
{
  char path[MAX_PATH + 1];
  size_t size = 0;
  backup_type = BACKUP_NONE;
  memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
  if (!ReGBA_GetBackupFilename(path, CurrentGamePath)) return 0;
  int result = battery_save_load(path, gamepak_backup, sizeof(gamepak_backup), &size);
  if (result == -ENOENT) return 0;
  if (result < 0) {
    errno = -result;
    ReGBA_Trace("SAVE load failed path=%s result=%d", path, result);
    return UINT32_MAX;
  }
  switch (size) {
    case 0x200: backup_type = BACKUP_EEPROM; eeprom_size = EEPROM_512_BYTE; break;
    case 0x2000: backup_type = BACKUP_EEPROM; eeprom_size = EEPROM_8_KBYTE; break;
    case 0x8000: backup_type = BACKUP_SRAM; sram_size = SRAM_SIZE_32KB; break;
    case 0x10000:
      /* SRAM and Flash share this size. Ordinary reads/writes work as SRAM;
       * the Flash unlock command promotes the type before any Flash action. */
      backup_type = BACKUP_SRAM; flash_size = FLASH_SIZE_64KB;
      sram_size = SRAM_SIZE_64KB; break;
    case 0x20000: backup_type = BACKUP_FLASH; flash_size = FLASH_SIZE_128KB; break;
  }
  ReGBA_Trace("SAVE loaded path=%s bytes=%lu", path, (unsigned long)size);
  return 1;
}

uint32_t save_backup()
{
  char path[MAX_PATH + 1];
  size_t size;
  if (!ReGBA_GetBackupFilename(path, CurrentGamePath)) { errno = ENOTSUP; return 0; }
  switch (backup_type) {
    case BACKUP_SRAM: size = sram_size == SRAM_SIZE_32KB ? 0x8000 : 0x10000; break;
    case BACKUP_FLASH: size = flash_size == FLASH_SIZE_64KB ? 0x10000 : 0x20000; break;
    case BACKUP_EEPROM: size = eeprom_size == EEPROM_512_BYTE ? 0x200 : 0x2000; break;
    default: errno = ENODATA; return 0;
  }
  int result = battery_save_submit(path, gamepak_backup, size);
  if (result < 0) errno = -result;
  return result == 0;
}

/* Core task only, paused with the previous save worker settled. Unlike the
 * initial loader, a failed manual read must leave the live guest untouched. */
int regba_reload_backup(void)
{
  char path[MAX_PATH + 1];
  size_t size = 0;
  if (!IsGameLoaded || !gamepak_rom) return -ENOENT;
  if (battery_save_busy()) return -EBUSY;
  if (!ReGBA_GetBackupFilename(path, CurrentGamePath)) return -ENOTSUP;
  uint8_t *image = malloc(sizeof(gamepak_backup));
  if (!image) return -ENOMEM;
  int result = battery_save_load(path, image, sizeof(gamepak_backup), &size);
  if (result < 0) {
    free(image);
    return result == -EILSEQ ? -EINVAL : result;
  }
  if (size != 0x200 && size != 0x2000 && size != 0x8000 &&
      size != 0x10000 && size != 0x20000) {
    free(image);
    return -EINVAL;
  }

  /* All fallible save-file work is complete. Match boot-time inference,
   * including the existing 64KiB SRAM/Flash ambiguity and config overrides. */
  sram_size = SRAM_SIZE_32KB;
  flash_size = FLASH_SIZE_64KB;
  eeprom_size = EEPROM_512_BYTE;
  memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
  memcpy(gamepak_backup, image, size);
  free(image);
  switch (size) {
    case 0x200: backup_type = BACKUP_EEPROM; break;
    case 0x2000: backup_type = BACKUP_EEPROM; eeprom_size = EEPROM_8_KBYTE; break;
    case 0x8000: backup_type = BACKUP_SRAM; break;
    case 0x10000: backup_type = BACKUP_SRAM; sram_size = SRAM_SIZE_64KB; break;
    case 0x20000: backup_type = BACKUP_FLASH; flash_size = FLASH_SIZE_128KB; break;
  }
  load_game_config(gamepak_title, gamepak_code, gamepak_maker);
  init_rewind();
  backup_update = write_backup_delay + 1;
  reset_gba();
  reg[CHANGED_PC_STATUS] = 1;
  return 0;
}
''' + memory[backup_end:]
    # load_game_config used to erase the type inferred from an existing .sav.
    memory = replace_once(memory, '\tbackup_type = BACKUP_NONE;\n',
                          '\t/* Preserve the type inferred by load_backup; explicit config may override it. */\n')
    # A matched user entry must stop lookup before the bundled fallback. The
    # upstream false return here applied a block, then incorrectly fell back.
    memory = replace_once(memory, '''\t\t\t\t\tif(!strcasecmp(current_variable, "game_name"))
\t\t\t\t\t{
\t\t\t\t\t\tfclose(config_file);
\t\t\t\t\t\treturn 0;
\t\t\t\t\t}''', '''\t\t\t\t\tif(!strcasecmp(current_variable, "game_name"))
\t\t\t\t\t{
\t\t\t\t\t\tfclose(config_file);
\t\t\t\t\t\treturn true;
\t\t\t\t\t}''')
    # Keep the last value character for ordinary LF files as well as CRLF.
    memory = replace_once(memory, '''  line_ptr_new = current_value + strlen(current_value) - 1;
  if(*line_ptr_new == '\\n')
  {
    line_ptr_new--;
    *line_ptr_new = 0;
  }

  if(*line_ptr_new == '\\r')
    *line_ptr_new = 0;''', '''  current_value[strcspn(current_value, "\\r\\n")] = 0;''')
    memory = replace_once(memory, '\t\tload_backup();', '''        backup_update = write_backup_delay + 1;
        if (load_backup() == UINT32_MAX) {
          int saved_errno = errno;
          ReGBA_UnmapEntireROM(gamepak_rom);
          gamepak_rom = NULL;
          gamepak_ram_buffer_size = gamepak_ram_pages = gamepak_size = 0;
          IsGameLoaded = false;
          errno = saved_errno;
          return -1;
        }''')
    (out / 'memory.c').write_text(memory, encoding='latin1')
    # The Linux-era stub did not reserve o32's 16-byte argument home area.
    # Move all private stack slots above that area and keep 8-byte alignment.
    stub_path = out / 'mips/stub.S'
    stub = stub_path.read_text(encoding='latin1')
    stub, frame_count = re.subn(r'(addiu\s+\$sp,\s*\$sp,\s*)(-?)(4|8|12)(\s|$)',
                  lambda m: m[1] + m[2] + '32' + m[4], stub, flags=re.M)
    stub, slot_count = re.subn(r'(?m)^(\s*(?:sw|lw)\s+[^,]+,\s*)([048]?)\(\$sp\)',
                  lambda m: m[1] + str(int(m[2] or 0) + 16) + '($sp)', stub)
    assert frame_count == 44 and slot_count == 50, (frame_count, slot_count)
    stub = replace_once(stub, '  sw $30, REG_R14($16)\n.endm',
        '  sw $30, REG_R14($16)\n  la $28, _gp  # bare-metal newlib uses the application GP\n.endm')
    stub = replace_once(stub, '\nlookup_pc:\n',
        '\nlookup_pc:\n  la $28, _gp  # irq_alert arrives after restoring guest GP\n')
    stub_path.write_text(stub, encoding='latin1')
    original = (src / 'opendingux/main.c').read_text(encoding='latin1')
    engine = original[:original.index('int main(int argc, char *argv[])')]
    engine = replace_once(engine, 'TIMER_TYPE timer[4];', '/* timer[4] is owned by memory.c. */')
    engine += original[original.index('u32 update_gba()'):original.index('u64 last_screen_timestamp')]
    engine = replace_once(engine, '          update_backup();',
                           '          /* Explicit battery-save milestone; no per-frame SD write. */')
    engine = replace_once(engine, '  return execute_cycles;', '  regba_frame_boundary();\n  return execute_cycles;')
    (out / 'engine.c').write_text(engine, encoding='latin1')
    rom = test_rom()
    (out / 'x2600-selftest.gba').write_bytes(rom)
    # Upstream's open replacement BIOS is GPL source-built, not a Nintendo dump.
    bios = (args.source / 'bios/gba_bios.bin').read_bytes()
    assert len(bios) == 16384
    save_tests = runpy.run_path(str(Path(__file__).with_name('make-save-test-roms.py')))
    game_config = (args.source / 'game_config.txt').read_bytes()
    resources = [('selftest_rom', rom), ('openbios', bios),
                 ('bundled_game_config', game_config)]
    resources += [(f'save_{kind}_rom', save_tests['save_test_rom'](
                      kind, eeprom_dma=args.eeprom_dma, flash_poll=args.flash_poll))
                  for kind in save_tests['KINDS']]
    with (out / 'test_resources.h').open('w') as f:
        for name, data in resources:
            f.write(f'static const unsigned char {name}[{len(data)}] = {{\n')
            for i in range(0, len(data), 24):
                f.write(','.join(f'0x{x:02x}' for x in data[i:i+24]) + ',\n')
            f.write('};\n')
    (out / 'SOURCE.txt').write_text(f'ReGBA {PIN}\nSelftest SHA256 {hashlib.sha256(rom).hexdigest()}\n'
        'Derived: engine loop only; raw full-ROM loader/OOM/page rounding; validated battery load and asynchronous native-save snapshots; no zlib/dirent.\n'
        f'Bundled compatibility config SHA256 {hashlib.sha256(game_config).hexdigest()}; user TF entries take priority.\n'
        'MIPS: o32 aligned argument home areas; guest/application GP switching; extern BIOS/timer ownership for fno-common garbage collection.\n'
        'JIT: static linking bounded to depth 8 with matching emitter/exit predicates; deeper branches use existing indirect dispatch.\n'
        'Cache publish uses project core/cache_publish.S, preserving the JIT limited-clobber ABI.\n')
    print(f'Prepared ReGBA {PIN}; diagnostic ROM {len(rom)} bytes')


if __name__ == '__main__':
    main()
