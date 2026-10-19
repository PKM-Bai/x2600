"""Derive a read-only FatFs/SD-SPI copy from the validated repository sources."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--ssi', required=True, type=Path,
                        help='Polling-corrected SSI output from prepare-ssi.py')
    args = parser.parse_args()
    output = args.output.resolve()
    platform = ROOT / 'projects/halley7m-regba/platform'
    output.mkdir(parents=True, exist_ok=True)
    fatfs = output / 'fatfs'
    fatfs.mkdir(exist_ok=True)
    manifest = {'schema': 1, 'readonly': True, 'sources': {}}
    for name in ('ff.c', 'ff.h', 'integer.h', 'unicode.c', 'diskio.h'):
        source = platform / 'fatfs' / name
        data = source.read_bytes()
        generated = data
        if name == 'diskio.h':
            text = data.decode('utf-8')
            text, count = re.subn(r'#define _USE_WRITE\s+1\b', '#define _USE_WRITE\t0', text)
            if count != 1:
                raise RuntimeError('Expected one FatFs disk-write configuration')
            generated = text.encode('utf-8')
        (fatfs / name).write_bytes(generated)
        manifest['sources'][name] = {
            'input_sha256': hashlib.sha256(data).hexdigest(),
            'output_sha256': hashlib.sha256(generated).hexdigest(),
        }
    config = Path(__file__).with_name('ffconf.h').read_bytes()
    (fatfs / 'ffconf.h').write_bytes(config)
    source = platform / 'sdspi.c'
    raw = source.read_bytes()
    text = raw.decode('utf-8').replace('\r\n', '\n')
    for function in ('sdspi_write', 'sdspi_sync'):
        text, count = re.subn(r'(?ms)^int ' + function + r'\(.*?^}\n', '', text)
        if count != 1:
            raise RuntimeError(f'Expected one complete {function} implementation')
    if 'command(24,' in text or 'int sdspi_write(' in text:
        raise RuntimeError('Read-only SD-SPI derivation retained its write path')
    generated = text.encode('utf-8')
    (output / 'sdspi_readonly.c').write_bytes(generated)
    manifest['sources']['sdspi.c'] = {
        'input_sha256': hashlib.sha256(raw).hexdigest(),
        'output_sha256': hashlib.sha256(generated).hexdigest(),
        'changes': ['remove SD write and write-sync APIs; retain validated read/init path'],
    }
    manifest['sources']['ffconf.h'] = {'output_sha256': hashlib.sha256(config).hexdigest()}
    # The SD transport always uses polling. The SDK still initializes PDMA
    # unconditionally in HAL_SPI_Init(NULL DMA_Handle), which is unnecessary
    # here and adds a controller ownership/link dependency to the other core.
    ssi_raw = args.ssi.read_bytes()
    ssi = ssi_raw.decode('utf-8').replace('\r\n', '\n')
    ssi, count = re.subn(
        r'(?ms)^    HAL_DMA_Init\(hspi->DMA_Handle\);\n.*?(?=^    __HAL_Lock_Init\(&hspi->Lock\);)',
        '', ssi)
    if count != 1:
        raise RuntimeError('Expected one SSI DMA initialization block')
    ssi, count = re.subn(
        r'(?ms)^    if \(hspi->SPI_DMA_Tx_Config != NULL\) \{.*?^    HAL_DMA_DeInit\(hspi->DMA_Handle\);\n',
        '', ssi)
    if count != 1:
        raise RuntimeError('Expected one SSI DMA deinitialization block')
    ssi, count = re.subn(r'(?ms)^HAL_StatusTypeDef HAL_SPI_DMA_(?:Transmit|Receive|TransmitReceive)\(.*?^}\n', '', ssi)
    if count != 3 or re.search(r'\bHAL_DMA_\w+\s*\(', ssi):
        raise RuntimeError('Polling-only SSI still references PDMA')
    init = 'HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *hspi)\n{\n'
    if ssi.count(init) != 1:
        raise RuntimeError('Expected one SSI initialization entry')
    ssi = ssi.replace(init, init +
        '    /* Player storage owns a polling-only SSI instance. */\n'
        '    if (hspi && (hspi->DMA_Handle || hspi->SPI_DMA_Tx_Config || hspi->SPI_DMA_Rx_Config))\n'
        '        return HAL_ERROR;\n', 1)
    ssi_output = ssi.encode('utf-8')
    (output / 'x2600_hal_ssi_polling.c').write_bytes(ssi_output)
    manifest['sources']['x2600_hal_ssi_polling.c'] = {
        'prepared_input_sha256': hashlib.sha256(ssi_raw).hexdigest(),
        'output_sha256': hashlib.sha256(ssi_output).hexdigest(),
        'changes': ['retain prepare-ssi.py polling corrections',
                    'remove DMA lifecycle and DMA entry points', 'reject DMA-configured handles'],
    }
    (output / 'SOURCE.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(f'Prepared read-only player storage in {output}')


if __name__ == '__main__':
    main()
