# HAL1 闭源库反解工具（camera/tests/hal1_re）

本目录只放**可复跑的只读分析脚本**，不放任何厂商二进制。目标库来自
`../../../../proprietary_vendor_oppo_sdm660-common/proprietary/vendor/lib/`
（默认路径写在 `verify_evidence.py` 的 `DEFAULT_DIR`，可用 `--vendor-lib-dir` 覆盖）：

| 库 | 大小 | sha256 | BuildID |
| --- | --- | --- | --- |
| `liboppo_platform_hwi.so` | 49060 | `c7ec37df…a33b742b` | `5d97f73476c6e98e8d8ddfca08112552` |
| `libcamera_core_hwi.so` | 402656 | `3ef68495…17bafcd` | `7f4afeea7673da52407182b45e9f664d` |

分析结论写在 `OCam/docs/hal1-re-hwi.md`。本 README 只说明怎么复跑与工具限制。

## 依赖与已知限制

```sh
python3 -c 'import capstone, elftools'   # 需要 capstone 5.x + pyelftools
llvm-objdump --version                   # 可选，人工核对用
```

- **binutils `objdump` 不可用**：本机 `objdump -i` 无 ARM BFD，且两个库是
  **Thumb-2** 却被 strip 掉了 `$a`/`$t` mapping symbol，直接按 ARM 解会输出垃圾
  （例如把 `push {r4-r11,lr}` 解成 `svcmi`）。所有脚本用 capstone 强制 Thumb
  解码；人工核对请用 `llvm-objdump -d --triple=thumbv7a-linux-android`。
- 两个库 `.text` 尾部（platform 0x82b8+、core 0x3798c+ 区段）是
  **Thumb→PLT interworking veneer**（`bx pc; nop` + ARM `ldr ip,[pc]; add pc,ip,pc`）。
  `hal1re.py` 会扫出来并解析成导入符号名，否则调用链里全是匿名地址。
- `.gnu_debugdata` 存在（xz 压缩 mini debug ELF），但只含 libunwind 等少量符号；
  真正有用的是 `.dynsym`——两个库以默认可见性编译，C++ 符号带完整参数类型，
  `nm -D` 就能拿到 `param_platform_t`/`bokeh_hwi`/`platform_face_data_t` 等结构名。
- `.data.rel.ro` 里的函数指针在文件中是 R_ARM_ABS32 的重定位槽（文件值 0），
  函数名必须从重定位表读，不能直接读节内容。

## 脚本

| 脚本 | 作用 |
| --- | --- |
| `hal1re.py` | 通用工具：`info`/`syms`/`strs`/`dis`/`callers`/`xref`/`reloc`/`veneers`/`array`/`dump` |
| `dump_registry.py` | 打印两个库的 17 项算法注册表（mask → 算法，槽位 → 函数） |
| `callgraph.py` | Thumb-2 线性扫描建调用图：`--callees`/`--callers`/`--roots` |
| `extract_offsets.py` | 从各算法 hook 提取 `param_platform_t` / frame ctx 的字段访问 |
| `verify_evidence.py` | 断言文档中的全部关键结论（哈希/掩码/偏移/注册表/ZSL 缺失），失败即非 0 |
| `run_all.sh` | 一次性跑完并收集输出到临时目录 |

## 复跑

```sh
cd camera/tests/hal1_re

# 1) 二进制身份与总体信息
./hal1re.py info  <vendor>/lib/liboppo_platform_hwi.so
./hal1re.py info  <vendor>/lib/libcamera_core_hwi.so

# 2) mask 位定义 + 注册表槽位（文档 §3 的证据源）
./dump_registry.py <vendor>/lib/liboppo_platform_hwi.so --table platform
./dump_registry.py <vendor>/lib/libcamera_core_hwi.so   --table core

# 3) 入口契约 / 分发循环
./hal1re.py dis <vendor>/lib/liboppo_platform_hwi.so --sym _Z21oppo_platform_processPvS_y
./hal1re.py dis <vendor>/lib/libcamera_core_hwi.so  --sym _Z20oppo_wrapper_processPvS_x

# 4) 字段布局
./extract_offsets.py <vendor>/lib/liboppo_platform_hwi.so > /tmp/oppo_offsets.txt

# 5) 调用链
./callgraph.py <vendor>/lib/libcamera_core_hwi.so --callees _ZN10oppocamera8ocamcore11workRoutineEPv
./callgraph.py <vendor>/lib/libcamera_core_hwi.so --callers _Z13llhdr_processPvS_

# 6) 一键验证（CI 友好，退出码 0 = 全部通过）
./verify_evidence.py && echo "evidence OK"
```

二进制不在仓库内，任何路径都以命令行参数传入；脚本只读、不写厂商目录。

## 不在此目录验证的内容（文档中已标证据缺口）

- `param_platform_t +0x214` 的 face-info 数组只有**元素大小 0x174 × 10 项**这一层证据
  （`bokeh_setup_parameters_hwi` 逐项 memcpy），字段级布局需要真机 dump 或继续反解
  消费端（`find_biggest_face_width`/`VENUS_face_*`）才能确认，当前不写成结论。
- 模式（HDR/LLHDR/MFNR/SUPER/…）→ mask 的选择点**不在这两个库里**：两库都只接受
  调用方传入的 mask。HAL1 侧真正决定模式的是 mm-camera 侧的
  `libmmcamera2_sensor_modules.so`（其中含 `liboppo_platform_hwi.so` 字符串，说明由它
  dlopen 平台层），该库不在本任务反解范围，文档中列为缺口。
- ZSL：两个库内 symbol 与字符串均无 `zsl`/ring buffer 痕迹（`verify_evidence.py` 有断言），
  即 HAL1 的 ZSL 环/触发/snapshot 不在这两个算法库里。
- `liboppocamalgo_daemon.so` 的 binder 服务实现未反解；只从导入/导出关系确认
  `camAlgo*` 由它提供。
