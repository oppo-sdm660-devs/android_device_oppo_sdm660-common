# camera/tests：HAL 移植轮次的宿主行为测试

这些测试**不是** Soong 模块：`camera/Android.bp` 不引用本目录，构建 ROM 时不会编译它们。
它们用于在无 Android 源码树的开发机上验证 T06 改动中可独立测试的部分。

## 1. 前摄美颜适配器（fake DSO）

```sh
python3 run_front_beauty_tests.py        # 需要 g++ 支持 -m32
HOST_SANITIZE=0 python3 run_front_beauty_tests.py   # 无 libasan 32 位时
```

- `fake_arcsoft_beautyshot.cpp` 编译成 **`libarcsoft_beautyshot.so`**（伪造同名 DSO），
  被 `QCamera3FrontBeauty.cpp` 以 `dlopen` 加载；脚本用 `LD_LIBRARY_PATH` 指向构建目录。
- `front_beauty_host_test.cpp` 覆盖：`isAvailable`、`Init/UnInit/SetFeatureLevel/Process`
  调用序列与次数、7 个有证据 feature id（0xc9/0xec/0xca/0xcb/0xcd/0xcc/0xf0）、
  强度 0–100 映射、`format=0x605` 与 YV12 三平面布局、NV21↔YV12 转换往返、
  失败路径（Init/Process/UnInit 错误、descriptor 被篡改、参数非法、取消）不修改原图。
- 全部用 `-m32` 编译，使 ARM32 指针宽度与 44 字节 ASVLOFFSCREEN 布局与设备一致；
  默认启用 ASan/UBSan。

**边界**：fake DSO 只证明“适配器按有证据的序列调用、参数与图像布局正确、错误可降级”，
**不**证明真实 `libarcsoft_beautyshot.so` 的算法行为、face-info 结构或成像效果。
face-info 结构未定，适配器传 NULL，真机验证前必须保持
`persist.vendor.camera.oppo.front_beauty.enable=0`（默认）。

## 2. 自动 HDR/低光策略（纯 C++）

```sh
python3 run_auto_capture_tests.py
HOST_SANITIZE=0 python3 run_auto_capture_tests.py
```

验证 `QCamera3AutoCapturePolicy` 的 histogram 裁剪值计算、低光（曝光/ISO 暗度指数）、
逆光/动态范围（直方图）触发、motion 阻止、缺 stats 与非法配置分支。HAL3 侧
`updateAutoCaptureStats()`/`fillAutoCaptureStats()` 负责把最近预览帧的
exposure/ISO/直方图喂给该策略；真实触发仍待真机（预览 stats 是否随属性开启而到达）。

## 3. 原厂 platform 算法层（HAL1 重现，task-54）

```sh
python3 run_platform_hwi_tests.py
HOST_SANITIZE=0 python3 run_platform_hwi_tests.py
```

用 fake `liboppo_platform_hwi.so`（导出与反解一致的 mangled 入口并记录调用）验证：
`QCamera3OppoPlatformParams` 的 mask/偏移构建、`QCamera3OppoPlatformHwi` 的
gate→init→updateparameters→process 顺序、per-stillMode 算法位、param/frame-ctx 布局、
失败状态与 close 反初始化。真实库按反解 ABI 调用，仍需真机 A/B 验证效果。

## 4. Vendor 专属 ZSL 策略（纯 C++，task-59）

```sh
python3 run_zsl_policy_tests.py
HOST_SANITIZE=0 python3 run_zsl_policy_tests.py
```

验证 `QCamera3ZslPolicy` 的触发判定（standard `ANDROID_CONTROL_ENABLE_ZSL=1` +
vendor `zslEnable=1` 双条件）、互斥矩阵（native stillMode/video/RAW/paired/secure/
flash/AE/AWB/AF 锁定）、ring 深度与内存估算（16MP depth=2→4 buffers、128MiB 上限）、
年龄分类（默认 120ms，超龄 applied=0/fallbackReason=2）与 fallbackReason 映射。
HAL3 侧复用 stock HAL ZSL snapshot channel；真实取帧/时延/内存与结果键需真机验证。

## 5. 静态一致性检查

```sh
python3 hal_static_checks.py
```

检查本轮成组改动的耦合关系（4K/vstab 属性、C1–C6 标准入口拆分、模板默认值、
前摄美颜协议/消费者/Android.bp、fake 测试文件可加载），防止只改一侧。grep 级别，
不是编译替代。

## 6. 真机验证入口

见 `OCam/docs/hal-changes.md` §10 的远程构建/部署/真机命令；本目录不包含任何
依赖真机或 Android 源码树的测试。
