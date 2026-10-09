/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

public final class ExtensionVersionImpl {
    public ExtensionVersionImpl() {}
    public String checkApiVersion(String version) {
        return version != null && version.startsWith("1.") ? "1.3.0" : null;
    }
    // Falling back to the basic provider preserves existing modes if a ROM
    // omitted the native BLOB writer or its app namespace cannot load it.
    public boolean isAdvancedExtenderImplemented() { return JpegBlobWriter.available(); }
}
