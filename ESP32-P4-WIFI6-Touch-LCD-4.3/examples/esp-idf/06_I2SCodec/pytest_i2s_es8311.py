# SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0
import pytest
from pytest_embedded import Dut


@pytest.mark.esp32p4
@pytest.mark.generic
def test_i2s_es8311_example_generic(dut: Dut) -> None:
    dut.expect('i2s codec example start')
    dut.expect('-----------------------------')
    dut.expect('Board audio codec initialization succeeded')
    dut.expect('Music playback started')
