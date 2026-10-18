"""Derive SSI polling completion fixes without editing the vendor SDK."""

import argparse
import hashlib
import json
from pathlib import Path
import re


SOURCE_NAME = "x2600_hal_ssi.c"
FUNCTION = re.compile(r"(?ms)^HAL_StatusTypeDef HAL_SPI_TransmitReceive\(.*?^}")
TX_POLL = """        if ((hspi->TxXferCount > 0) && !(__HAL_SPI_TX_FIFO_IS_FULL(hspi))) {
"""
TX_WINDOW = """        /* Bound all queued/in-flight/unread characters, not only TxFIFO.
         * The X2600 has 128 RX entries. A 64-character window stays safe
         * even when another task runs until every queued TX has shifted.
         */
        if ((hspi->TxXferCount > 0) &&
                (((uint32_t)hspi->RxXferCount - hspi->TxXferCount) < 64U) &&
                !(__HAL_SPI_TX_FIFO_IS_FULL(hspi))) {
"""
POLL_TIMEOUT = """        if ((((HAL_GetTick() - tickstart) >=  Timeout) && ((Timeout != HAL_MAX_DELAY))) || (Timeout == 0U)) {
"""
PENDING_TIMEOUT = """        /* Completion wins over a finite wall-clock deadline: polling may
         * resume after the last RX byte and hardware END are already ready.
         * Preserve the SDK's immediate Timeout == 0 behavior.
         */
        if ((Timeout == 0U) ||
                (((hspi->TxXferCount != 0U) || (hspi->RxXferCount != 0U)) &&
                 (Timeout != HAL_MAX_DELAY) &&
                 ((HAL_GetTick() - tickstart) >= Timeout))) {
"""
EARLY_FINISH = """
            /* 传输结束 */
            if (hspi->TxXferCount == 0) {
                __HAL_SPI_FINISH_TRANSFER(hspi);
            }
"""
FINAL_UNDERRUN_CLEAR = """    /* The final empty FIFO can leave TUNDR set while UNFIN held the
     * transfer open. PM 31.7.2.4 defines writing zero to clear this hold flag.
     * Clear only TX underrun; preserve any RX overrun for the check below.
     */
    __HAL_SPI_CLEAR_TX_ERROR(hspi);

"""
END_WAIT = """    /* Keep UNFIN set until all expected RX characters have arrived. PM
     * 31.7.2.3 permits waiting with TxFIFO empty; no extra data is transmitted.
     */
    __HAL_SPI_FINISH_TRANSFER(hspi);

""" + FINAL_UNDERRUN_CLEAR + """    /* X2600 PM 31.7.2.4: BUSY may be zero during an interval while
     * END is still zero. Software RX/TX counts do not prove the clock/frame
     * tail has completed. Wait before DISREV or either FIFO is flushed.
     * Share the original transaction deadline; timeout uses existing cleanup.
     */
    while ((__HAL_SPI_GET_STATUS(hspi) & SSISR_END_Msk) == 0U) {
        if ((((HAL_GetTick() - tickstart) >= Timeout) && (Timeout != HAL_MAX_DELAY)) ||
                (Timeout == 0U)) {
            errorcode = HAL_TIMEOUT;
            goto error;
        }
    }

"""


def derive_ssi(text):
    matches = list(FUNCTION.finditer(text))
    if len(matches) != 1:
        raise RuntimeError(f"SSI patch requires one polling TransmitReceive, found {len(matches)}")
    match = matches[0]
    original = match[0]
    marker = "    if (__HAL_SPI_IS_RX_ERROR(hspi)) {\n"
    error_marker = "error :\n    __HAL_SPI_DISABLE_RECEIVE(hspi);\n"
    expected_tail = """        }
    }

    if (__HAL_SPI_IS_RX_ERROR(hspi)) {
"""
    if (original.count(expected_tail) != 1 or original.count(marker) != 1 or
            original.count(POLL_TIMEOUT) != 1 or original.count(EARLY_FINISH) != 1 or
            original.count(error_marker) != 1 or original.count(TX_POLL) != 1 or
            original.count("    uint32_t tickstart;\n") != 1):
        raise RuntimeError("SSI polling loop/normal-cleanup patch no longer matches pinned source")
    if "SSISR_END_Msk" in original:
        raise RuntimeError("SSI polling source already has an END check; review the SDK change")
    updated = original.replace(TX_POLL, TX_WINDOW, 1)
    updated = updated.replace(POLL_TIMEOUT, PENDING_TIMEOUT, 1)
    updated = updated.replace(EARLY_FINISH, "", 1)
    updated = updated.replace(marker, END_WAIT + marker, 1)
    updated = updated.replace("    uint32_t tickstart;\n",
                              "    extern void regba_ssi_timeout_snapshot(SPI_HandleTypeDef *hspi);\n"
                              "    extern uint32_t regba_ssi_poll_tick(void);\n"
                              "    uint32_t tickstart;\n", 1)
    updated = updated.replace(error_marker,
                              "error :\n"
                              "    if (errorcode == HAL_TIMEOUT) regba_ssi_timeout_snapshot(hspi);\n"
                              "    __HAL_SPI_DISABLE_RECEIVE(hspi);\n", 1)
    if updated.count("HAL_GetTick()") != 3:
        raise RuntimeError("SSI task-clock patch requires exactly three polling tick reads")
    updated = updated.replace("HAL_GetTick()", "regba_ssi_poll_tick()")
    return text[:match.start()] + updated + text[match.end():]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sdk, output = args.sdk.resolve(), args.output.resolve()
    if output == sdk or sdk in output.parents:
        parser.error("--output must stay outside the vendor SDK")
    source = sdk / "drivers/drivers-x2600/src" / SOURCE_NAME
    raw = source.read_bytes()
    text = raw.decode("utf-8").replace("\r\n", "\n")
    generated = derive_ssi(text).encode("utf-8")
    manifest = {
        "schema": 1,
        "sdk": str(sdk),
        "sources": {SOURCE_NAME: {
            "input_sha256": hashlib.sha256(raw).hexdigest(),
            "output_sha256": hashlib.sha256(generated).hexdigest(),
            "changes": [
                "bound queued/in-flight/unread TX characters to 64 for long polling transfers",
                "use 32-bit task millisecond tick only for polling transfer deadlines",
                "only time out unfinished RX/TX; retain Timeout=0 and original deadline",
                "retain UNFIN until all expected RX bytes arrive before END wait",
                "clear final TX underrun after FINISH before waiting END; preserve RX overrun",
                "snapshot polling timeout before RX disable/error clear/FIFO cleanup",
                "poll END before normal RX disable/FIFO cleanup; retain original Timeout",
            ],
        }},
    }
    output.mkdir(parents=True, exist_ok=True)
    (output / SOURCE_NAME).write_bytes(generated)
    (output / "SOURCE.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared SSI polling completion fixes in {output}")


if __name__ == "__main__":
    main()
