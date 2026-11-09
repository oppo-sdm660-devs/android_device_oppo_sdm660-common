# visidon_compat: vendor 32-bit libjnigraphics stub (task-156)

`libVDSelfiePanorama.so` (32-bit, JNI) has `DT_NEEDED libjnigraphics.so`, but a
vendor HAL process cannot resolve the system `libjnigraphics.so` (it is not
LLNDK/VNDK; stock vendor namespace only links the LLNDK whitelist, which does
include libEGL/libGLESv2 but not libjnigraphics). The Visidon backend's
frame -> NV21/JPEG path does not call `AndroidBitmap_*` (Bitmap entry points are
not used), so this stub satisfies the dynamic linker.

- Module: `libjnigraphics_visidon_stub` (`vendor: true`, `compile_multilib: 32`),
  installed as `/vendor/lib/libjnigraphics.so` via `stem` + `-soname`.
- All three `AndroidBitmap_*` symbols return `-2` (bad parameter) and log once.
- Integration (hal-dev task-157): add the module to the camera HAL
  `required` list; do **not** link it (`shared_libs`) because the vendor engine
  is `dlopen`'ed.
- After integration verify: `ls -l /vendor/lib/libjnigraphics.so` (ELF32) and no
  `dlopen failed` in HAL logcat.
