package com.nyako.ltpofix;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;

final class PointerMonitor {
    private Object listener;

    boolean install(final Runnable onTouch) {
        if (listener != null) return true;
        try {
            Class<?> sm = Class.forName("android.os.ServiceManager");
            Method getService = sm.getMethod("getService", String.class);
            Object binder = getService.invoke(null, "window");
            if (binder == null) return false;
            Class<?> ibinder = Class.forName("android.os.IBinder");
            Method qli = ibinder.getMethod("queryLocalInterface", String.class);
            Object wm = qli.invoke(binder, "android.view.IWindowManager");
            if (wm == null) return false;
            Class<?> iface = Class.forName("android.view.WindowManagerPolicyConstants$PointerEventListener");
            Object proxy = Proxy.newProxyInstance(
                iface.getClassLoader(),
                new Class<?>[] { iface },
                new InvocationHandler() {
                    @Override
                    public Object invoke(Object p, Method m, Object[] args) {
                        String name = m.getName();
                        if ("equals".equals(name)) return p == args[0];
                        if ("hashCode".equals(name)) return System.identityHashCode(p);
                        if ("toString".equals(name)) return "NyakoLtpoPointerMonitor";
                        if ("onPointerEvent".equals(name)) onTouch.run();
                        return null;
                    }
                });
            Method reg = wm.getClass().getMethod("registerPointerEventListener", iface, int.class);
            reg.setAccessible(true);
            reg.invoke(wm, proxy, 0);
            listener = proxy;
            return true;
        } catch (Throwable t) {
            return false;
        }
    }
}
