/* SPDX-License-Identifier: BSD-3-Clause */
package androidx.camera.extensions.impl;

import android.content.Context;
import java.util.concurrent.Executor;

public final class InitializerImpl {
    public static final int ERROR_UNKNOWN = 0;
    public static final int ERROR_INITIALIZE_VERSION_INCOMPATIBLE = 1;
    private InitializerImpl() {}
    public interface OnExtensionsInitializedCallback {
        void onSuccess();
        void onFailure(int error);
    }
    public interface OnExtensionsDeinitializedCallback {
        void onSuccess();
        void onFailure(int error);
    }
    public static void init(String version, Context context,
            OnExtensionsInitializedCallback callback, Executor executor) {
        Runnable event=()-> {
            if(new ExtensionVersionImpl().checkApiVersion(version)==null)
                callback.onFailure(ERROR_INITIALIZE_VERSION_INCOMPATIBLE);
            else callback.onSuccess();
        };
        if(executor==null)event.run();else executor.execute(event);
    }
    public static void deinit(OnExtensionsDeinitializedCallback callback, Executor executor) {
        if(executor==null)callback.onSuccess();else executor.execute(callback::onSuccess);
    }
}
