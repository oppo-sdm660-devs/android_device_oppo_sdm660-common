#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
hal = root / 'QCamera2/HAL3'
with tempfile.TemporaryDirectory(prefix='ocam-filter-adapter-') as tmp:
    lib = pathlib.Path(tmp) / 'fixture.so'
    test = pathlib.Path(tmp) / 'test'
    subprocess.run(['g++', '-std=c++14', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                    str(root / 'tests/stock_filter_fixture.cpp'), '-o', str(lib)], check=True)
    subprocess.run(['g++', '-std=c++14', '-Wall', '-Wextra', '-Werror', '-pthread',
                    f'-DQCAMERA3_STOCK_FILTER_LIBRARY="{lib}"', '-I', str(hal),
                    str(root / 'tests/stock_filter_adapter_test.cpp'),
                    str(hal / 'QCamera3StockFilter.cpp'), str(hal / 'QCamera3HdrAlgo.cpp'),
                    '-ldl', '-o', str(test)], check=True)
    subprocess.run([str(test), str(lib)], check=True)
print('PASS: stock CPU ABI argument/layout, padded RGBA/NV21, failure and cancellation atomicity')
