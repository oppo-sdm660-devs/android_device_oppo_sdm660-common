#!/usr/bin/env python3
"""Run the production HWI metadata handoff and channel cleanup with ASan."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
HAL3 = HERE.parent / "QCamera2" / "HAL3"
hwi = (HAL3 / "QCamera3HWI.cpp").read_text()
channel = (HAL3 / "QCamera3Channel.cpp").read_text()


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


handler = function(hwi, "void QCamera3HardwareInterface::handleMetadataWithLock(")
selection_start = handler.index("            // Find channel requiring metadata")
selection_end = handler.index("            if (previewJpegFrame) {\n                clearPreviewJpegRequest", selection_start)
handoff_start = handler.index("            if (metadataConsumer) {", selection_end)
handoff_end = handler.index("            if (!internalPproc) {", handoff_start)
cleanup_start = handler.index("    if(!meta_freed && free_and_bufdone_meta_buf)")
cleanup_end = handler.index('\n\n    LOGD("mPendingLiveRequest', cleanup_start)
pic_callback = function(channel, "void QCamera3PicChannel::streamCbRoutine(")
source_error_start = pic_callback.index("    const bool zeroSource")
source_error_end = pic_callback.index("    int32_t frameNum =", source_error_start)
assert selection_end < handler.index("saveExifParams(resultMetadata)", selection_end) < handoff_start
assert handler.index("translateFromHalMetadata(resultMetadata", selection_end) < handoff_start
assert handler.index("updateFpsInPreviewBuffer(metadata,") < handoff_start

out = Path(tempfile.mkdtemp(prefix="ocam-metadata-ownership-"))
for name, source in {
    "selection.inc": handler[selection_start:selection_end],
    "handoff.inc": handler[handoff_start:handoff_end],
    "cleanup.inc": handler[cleanup_start:cleanup_end],
    "channel.inc": function(channel, "int32_t QCamera3ProcessingChannel::queueReprocMetadata(")
        + "\n" + function(channel, "int32_t QCamera3ProcessingChannel::metadataBufDone("),
    "source_error.inc": pic_callback[source_error_start:source_error_end],
    "return_error.inc": function(channel, "int QCamera3PicChannel::returnBufferError("),
}.items():
    (out / name).write_text(source)
producer_start = hwi.index("                requestedStream.need_metadata = requestedStream.meteringOnly != 1 &&")
producer_end = hwi.index("\n                }", producer_start) + len("\n                }")
(out / "producer.inc").write_text(hwi[producer_start:producer_end])

flags = ["-std=gnu++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
subprocess.run([os.environ.get("CXX", "g++"), *flags, "-I", str(out),
                str(HERE / "metadata_ownership_test.cpp"), "-o", str(out / "test")], check=True)
subprocess.run([str(out / "test")], check=True)
print(f"artifacts: {out}")
