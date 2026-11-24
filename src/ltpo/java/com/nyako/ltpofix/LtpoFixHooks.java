package com.nyako.ltpofix;

import android.os.Handler;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.system.Os;
import android.util.Log;
import android.util.LongSparseArray;
import android.util.SparseArray;
import android.view.Display;
import com.android.server.display.mode.DisplayModeDirector;
import com.nyako.NyakoBridge;
import java.io.FileInputStream;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Arrays;

/* Zero-Polling Event-Driven Xiaomi 15 (o3_42_0d_0a) Hardware LTPO Controller.
 *
 * Replaces SnapshotReader (1MB memfd SurfaceFlinger dumps & 30MB GC churn) with pure
 * event-driven mode transitions driven by /system/lib64/overlay.so inside SurfaceFlinger:
 *   - sf_idle = 0  -> sfModeId=1 (group 0x2780100, auto_120_to_1hz, HW DDIC auto-LTPO active)
 *   - sf_idle = 10 -> sfModeId=2 (group 0x1780A00, idle_120_to_10hz, 10Hz intermediate step)
 *   - sf_idle = 1  -> sfModeId=3 (group 0x1780100, idle_120_to_1hz, 1Hz static idle / Doze)
 * Preserves PointerMonitor instant touch wake, PanelSafety HDR/HBM guard, native 90/60/40/30/24Hz
 * mode passthrough, and Show-Refresh-Rate overlay repaint ONLY when overlay is enabled.
 */
public final class LtpoFixHooks {
    private static final String TAG = "NyakoLtpoFix";
    private static final int GROUP_AUTO_1  = 0x2780100;
    private static final int GROUP_IDLE_10 = 0x1780A00;
    private static final int GROUP_IDLE_1  = 0x1780100;

    private static final String P_FORCE      = "persist.nyako.ltpo.force_mode";
    private static final String P_AUTO_IDLE  = "persist.nyako.ltpo.auto_idle";
    private static final String P_ALLOW      = "persist.nyako.ltpo.allow_group";
    private static final String P_IDLE_TEST  = "debug.nyako.ltpo.idle_test";
    private static final String P_SF_IDLE    = "debug.nyako.ltpo.sf_idle";
    private static final String P_SF_OVERLAY = "debug.nyako.ltpo.sf_overlay_on";
    private static final String P_IME_SHOWN  = "sys.oplus.ltpo.ime_shown";
    private static final String P_LOG        = "persist.nyako.ltpo.log";

    private static final Object STATE_LOCK = new Object();
    private static final PointerMonitor POINTER = new PointerMonitor();

    private static WeakReference<Object> sDevice = new WeakReference<Object>(null);
    private static Object sDirector;
    private static Handler sHandler;
    private static Method sNotify;
    private static boolean sCallbackRegistered;
    private static boolean sWatcherStarted;

    private static int sLastForce = -1;
    private static int sLastAutoIdle = -1;
    private static int sLastAllowGroup = -1;
    private static int sLastIdleTest = -1;
    private static int sLastSfIdleProp = -1;
    private static boolean sLastImeShown;

    static volatile boolean sTouchActive;
    private static volatile long sLastTouchMs;
    private static volatile int sIdleLevel; // 0 = active, 10 = idle10, 1 = idle1
    private static volatile boolean sPanelAllowed;
    private static volatile boolean sPointerReady;
    private static String sLastMapping;

    private static final Runnable TOUCH_END_RUNNABLE = new Runnable() {
        @Override
        public void run() {
            onTouchIdleTimeout();
        }
    };

    private static final Runnable OVERLAY_REPAINT_RUNNABLE = new Runnable() {
        @Override
        public void run() {
            if (syspropInt(P_SF_OVERLAY, 0) == 1) {
                requestOverlayRepaint();
            }
        }
    };

    private static final Runnable SYNC_IDLE_RUNNABLE = new Runnable() {
        @Override
        public void run() {
            applyCurrentIdleState();
        }
    };

    private LtpoFixHooks() {}

    // --- SystemProperties reflection helpers (avoids hidden API compile stubs) ---
    private static int syspropInt(String key, int def) {
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            return ((Integer) sp.getMethod("getInt", String.class, int.class).invoke(null, key, def)).intValue();
        } catch (Throwable t) {
            return def;
        }
    }

    private static boolean syspropBool(String key, boolean def) {
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            return ((Boolean) sp.getMethod("getBoolean", String.class, boolean.class).invoke(null, key, def)).booleanValue();
        } catch (Throwable t) {
            return def;
        }
    }

    private static void addPropChangeCallback(Runnable r) throws Exception {
        Class<?> sp = Class.forName("android.os.SystemProperties");
        sp.getMethod("addChangeCallback", Runnable.class).invoke(null, r);
    }

    private static void log(String msg) {
        if (syspropInt(P_LOG, 0) != 0) {
            Log.i(TAG, msg);
        }
    }

    // --- Reflection helpers ---
    private static Field findField(Class<?> cls, String name) throws NoSuchFieldException {
        while (cls != null) {
            try {
                Field f = cls.getDeclaredField(name);
                f.setAccessible(true);
                return f;
            } catch (NoSuchFieldException e) {
                cls = cls.getSuperclass();
            }
        }
        throw new NoSuchFieldException(name);
    }

    private static Object field(Object obj, String name) throws Exception {
        return findField(obj.getClass(), name).get(obj);
    }

    private static Method findNoArgMethod(Class<?> cls, String name) throws NoSuchMethodException {
        while (cls != null) {
            try {
                Method m = cls.getDeclaredMethod(name);
                m.setAccessible(true);
                return m;
            } catch (NoSuchMethodException e) {
                cls = cls.getSuperclass();
            }
        }
        throw new NoSuchMethodException(name);
    }

    private static boolean sameRate(float a, float b) {
        return Float.floatToIntBits(a) == Float.floatToIntBits(b);
    }

    private static boolean sameSize(Display.Mode a, Display.Mode b) {
        return a.getPhysicalWidth() == b.getPhysicalWidth()
            && a.getPhysicalHeight() == b.getPhysicalHeight();
    }

    private static boolean sameHdrTypes(int[] a, int[] b) {
        if (a == null || b == null) return a == b;
        int[] ca = a.clone();
        int[] cb = b.clone();
        Arrays.sort(ca);
        Arrays.sort(cb);
        return Arrays.equals(ca, cb);
    }

    private static Display.Mode findMode(Display.Mode[] modes, int modeId) {
        if (modes == null) return null;
        for (Display.Mode m : modes) {
            if (m.getModeId() == modeId) return m;
        }
        return null;
    }

    private static boolean isIsolatedVariant(Display.Mode[] modes, Display.Mode target) {
        if (target.getAlternativeRefreshRates().length != 0) return false;
        for (Display.Mode m : modes) {
            if (m.getModeId() != target.getModeId()
                && sameSize(m, target)
                && sameRate(m.getRefreshRate(), target.getRefreshRate())
                && m.getAlternativeRefreshRates().length != 0) {
                return true;
            }
        }
        return false;
    }

    private static boolean matchesSfMode(Display.Mode dm, Object sfMode) throws Exception {
        float vsync = ((Float) Display.Mode.class.getMethod("getVsyncRate").invoke(dm)).floatValue();
        int w = ((Integer) field(sfMode, "width")).intValue();
        int h = ((Integer) field(sfMode, "height")).intValue();
        float peak = ((Float) field(sfMode, "peakRefreshRate")).floatValue();
        float sfVsync = ((Float) field(sfMode, "vsyncRate")).floatValue();
        int[] hdr = (int[]) field(sfMode, "supportedHdrTypes");
        return dm.getPhysicalWidth() == w
            && dm.getPhysicalHeight() == h
            && sameRate(dm.getRefreshRate(), peak)
            && sameRate(vsync, sfVsync)
            && sameHdrTypes(dm.getSupportedHdrTypes(), hdr);
    }

    private static boolean physicalRangeContains(Object ranges, float rate) throws Exception {
        Object phys = field(ranges, "physical");
        float min = ((Float) field(phys, "min")).floatValue();
        float max = ((Float) field(phys, "max")).floatValue();
        return !Float.isNaN(rate) && rate >= (min - 0.01f) && rate <= (max + 0.01f);
    }

    private static void widenSpecsMax(DisplayModeDirector.DesiredDisplayModeSpecs specs, float rate) {
        try {
            Object primary = field(specs, "primary");
            if (primary != null) {
                widenRangeMax(field(primary, "physical"), rate);
                widenRangeMax(field(primary, "render"), rate);
            }
            Object appReq = field(specs, "appRequest");
            if (appReq != null) {
                widenRangeMax(field(appReq, "physical"), rate);
                widenRangeMax(field(appReq, "render"), rate);
            }
        } catch (Throwable ignored) {}
    }

    private static void widenRangeMax(Object range, float rate) throws Exception {
        if (range == null) return;
        Field fMax = findField(range.getClass(), "max");
        if (fMax.getFloat(range) < rate) {
            fMax.setFloat(range, rate);
        }
    }

    private static void disableLightSensorAlwaysOn() {
        try {
            Class<?> cls = Class.forName("com.android.server.display.config.global.OplusDisplayGlobalConfig");
            Object inst = cls.getMethod("getInstance").invoke(null);
            if (inst == null) return;
            Field f1 = cls.getDeclaredField("mLightSensorAlwaysOn");
            f1.setAccessible(true);
            f1.setBoolean(inst, false);
            Field f2 = cls.getDeclaredField("mComfortableBrightnessConfig");
            f2.setAccessible(true);
            Object cb = f2.get(inst);
            if (cb != null) {
                Field f3 = cb.getClass().getDeclaredField("mLightSensorAlwaysOnSupport");
                f3.setAccessible(true);
                f3.setBoolean(cb, false);
                Field f4 = cb.getClass().getDeclaredField("mForceUseXmlConfigForSensorAlwaysOn");
                f4.setAccessible(true);
                f4.setBoolean(cb, true);
            }
        } catch (Throwable ignored) {}
    }

    private static boolean checkIdleAllowed(Object director) {
        try {
            if (syspropInt(P_AUTO_IDLE, 0) != 1 || syspropInt(P_FORCE, 0) <= 0
                || syspropInt(P_IDLE_TEST, 0) != 0 || !sPointerReady) {
                sPanelAllowed = false;
                return false;
            }
            Object lock = field(director, "mLock");
            synchronized (lock) {
                Object bo = field(director, "mBrightnessObserver");
                int state = ((Integer) field(bo, "mDefaultDisplayState")).intValue();
                if (state != 2) {
                    sPanelAllowed = false;
                    return false;
                }
                Object udfps = field(director, "mUdfpsObserver");
                String[] fnames = new String[] { "mUdfpsRefreshRateEnabled", "mAuthenticationPossible" };
                for (String fn : fnames) {
                    Object arr = field(udfps, fn);
                    Method get = arr.getClass().getMethod("get", int.class);
                    if (((Boolean) get.invoke(arr, 0)).booleanValue()) {
                        sPanelAllowed = false;
                        return false;
                    }
                }
            }
            boolean ok = PanelSafety.allowed();
            sPanelAllowed = ok;
            return ok;
        } catch (Throwable t) {
            sPanelAllowed = false;
            return false;
        }
    }

    private static boolean invalidateAndApply(Object director) throws Exception {
        Object dev;
        synchronized (STATE_LOCK) {
            dev = sDevice.get();
        }
        if (dev == null) return false;
        Method getAdapter = findNoArgMethod(dev.getClass(), "getAdapterLocked");
        Object adapter = getAdapter.invoke(dev);
        Method getSyncRoot = findNoArgMethod(adapter.getClass(), "getSyncRoot");
        Object syncRoot = getSyncRoot.invoke(adapter);
        synchronized (syncRoot) {
            @SuppressWarnings("unchecked")
            LongSparseArray<Object> devices = (LongSparseArray<Object>) field(adapter, "mDevices");
            long physId = ((Long) field(dev, "mPhysicalDisplayId")).longValue();
            if (devices.get(physId) != dev) return false;

            Field fInvalid = findField(dev.getClass(), "mDisplayModeSpecsInvalid");
            fInvalid.setBoolean(dev, true);

            Method getSpecs = director.getClass().getMethod("getDesiredDisplayModeSpecs", int.class);
            getSpecs.setAccessible(true);
            Object specs = getSpecs.invoke(director, 0);

            Method setSpecs = dev.getClass().getMethod(
                "setDesiredDisplayModeSpecsLocked",
                DisplayModeDirector.DesiredDisplayModeSpecs.class);
            setSpecs.setAccessible(true);
            setSpecs.invoke(dev, specs);
            return !fInvalid.getBoolean(dev);
        }
    }

    private static void requestOverlayRepaint() {
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            Class<?> sm = Class.forName("android.os.ServiceManager");
            IBinder sf = (IBinder) sm.getMethod("checkService", String.class).invoke(null, "SurfaceFlinger");
            if (sf != null) {
                data.writeInterfaceToken("android.ui.ISurfaceComposer");
                sf.transact(0x3ec, data, reply, 0);
            }
        } catch (Throwable ignored) {
        } finally {
            reply.recycle();
            data.recycle();
        }
    }

    private static void triggerDirectorNotify() {
        final Object dir;
        final Method notify;
        final Handler h;
        synchronized (STATE_LOCK) {
            dir = sDirector;
            notify = sNotify;
            h = sHandler;
        }
        if (dir != null && h != null) {
            h.post(new Runnable() {
                @Override
                public void run() {
                    try {
                        invalidateAndApply(dir);
                    } catch (Throwable ignored) {
                        if (notify != null) {
                            try {
                                Object lock = field(dir, "mLock");
                                synchronized (lock) {
                                    notify.invoke(dir);
                                }
                            } catch (Throwable ignored2) {}
                        }
                    }
                }
            });
        }
    }

    private static void ensurePointerMonitor() {
        if (sPointerReady) return;
        if (syspropInt(P_AUTO_IDLE, 0) != 1) return;
        boolean ok = POINTER.install(new Runnable() {
            @Override
            public void run() {
                onTouchInteraction();
            }
        });
        sPointerReady = ok;
        log("pointer monitor " + (ok ? "registered" : "unavailable"));
    }

    private static void applyCurrentIdleState() {
        final Object dir;
        final Handler h;
        synchronized (STATE_LOCK) {
            dir = sDirector;
            h = sHandler;
        }
        if (dir == null) return;
        ensurePointerMonitor();

        int sfIdle = syspropInt(P_SF_IDLE, 0);
        int wantLevel = 0;
        if (!sTouchActive && (sfIdle == 10 || sfIdle == 1) && checkIdleAllowed(dir)) {
            wantLevel = sfIdle;
        }
        int prev = sIdleLevel;
        sIdleLevel = wantLevel;
        if (prev != wantLevel) {
            boolean applied = false;
            try {
                applied = invalidateAndApply(dir);
            } catch (Throwable err) {
                Log.w(TAG, "invalidateAndApply failed: " + err);
            }
            if (!applied) {
                triggerDirectorNotify();
            }
            if (wantLevel != 0 && h != null && syspropInt(P_SF_OVERLAY, 0) == 1) {
                h.removeCallbacks(OVERLAY_REPAINT_RUNNABLE);
                h.postDelayed(OVERLAY_REPAINT_RUNNABLE, 15L);
            }
        }
    }

    private static void onTouchInteraction() {
        sLastTouchMs = SystemClock.uptimeMillis();
        int prevIdle = sIdleLevel;
        sTouchActive = true;
        sIdleLevel = 0;

        Handler h = sHandler;
        if (h != null) {
            h.removeCallbacks(TOUCH_END_RUNNABLE);
            h.postDelayed(TOUCH_END_RUNNABLE, 200L);
        }
        if (prevIdle != 0) {
            triggerDirectorNotify();
        }
    }

    private static void onTouchIdleTimeout() {
        if (SystemClock.uptimeMillis() - sLastTouchMs >= 180L && sTouchActive) {
            sTouchActive = false;
            applyCurrentIdleState();
        }
    }

    private static void onPropertiesChanged() {
        try {
            int force = syspropInt(P_FORCE, 0);
            int autoIdle = syspropInt(P_AUTO_IDLE, 0);
            int allowGroup = syspropInt(P_ALLOW, 0);
            int idleTest = syspropInt(P_IDLE_TEST, 0);
            int sfIdle = syspropInt(P_SF_IDLE, 0);
            boolean ime = syspropBool(P_IME_SHOWN, false);

            boolean configChanged = false;
            boolean sfIdleChanged = false;
            Handler h;
            synchronized (STATE_LOCK) {
                if (force != sLastForce || autoIdle != sLastAutoIdle
                    || allowGroup != sLastAllowGroup || idleTest != sLastIdleTest
                    || ime != sLastImeShown) {
                    sLastForce = force;
                    sLastAutoIdle = autoIdle;
                    sLastAllowGroup = allowGroup;
                    sLastIdleTest = idleTest;
                    sLastImeShown = ime;
                    configChanged = true;
                }
                if (sfIdle != sLastSfIdleProp) {
                    sLastSfIdleProp = sfIdle;
                    sfIdleChanged = true;
                }
                h = sHandler;
            }
            if (configChanged) {
                triggerDirectorNotify();
            } else if (sfIdleChanged && h != null) {
                h.removeCallbacks(SYNC_IDLE_RUNNABLE);
                h.post(SYNC_IDLE_RUNNABLE);
            }
        } catch (Throwable t) {
            Log.w(TAG, "property callback failed: " + t);
        }
    }

    private static void ensurePropertyCallback(final Object director) throws Exception {
        synchronized (STATE_LOCK) {
            if (sDirector != director) {
                Handler h = (Handler) field(director, "mHandler");
                Method notify;
                try {
                    notify = findNoArgMethod(director.getClass(), "notifyDesiredDisplayModeSpecsChangedLocked");
                } catch (NoSuchMethodException e) {
                    notify = findNoArgMethod(director.getClass(), "lambda$start$0");
                }
                sHandler = h;
                sNotify = notify;
                sDirector = director;
                if (h != null) {
                    h.post(SYNC_IDLE_RUNNABLE);
                }
            }
            if (!sCallbackRegistered) {
                addPropChangeCallback(new Runnable() {
                    @Override
                    public void run() {
                        onPropertiesChanged();
                    }
                });
                sCallbackRegistered = true;
                log("event-driven property callback registered (zero polling)");
            }
            if (!sWatcherStarted) {
                sWatcherStarted = true;
                Thread t = new Thread(new Runnable() {
                    @Override
                    public void run() {
                        String fdStr = System.getProperty("nyako.ltpo.pipe_fd");
                        if (fdStr == null || fdStr.isEmpty()) {
                            try {
                                fdStr = Os.getenv("NYAKO_LTPO_PIPE_FD");
                            } catch (Throwable ignored) {}
                        }
                        if (fdStr == null || fdStr.isEmpty()) {
                            Log.w(TAG, "nyako.ltpo.pipe_fd missing");
                            return;
                        }
                        int fd;
                        try {
                            fd = Integer.parseInt(fdStr.trim());
                        } catch (Throwable err) {
                            Log.w(TAG, "invalid pipe_fd: " + fdStr);
                            return;
                        }
                        if (fd < 0) return;
                        ParcelFileDescriptor pfd = ParcelFileDescriptor.adoptFd(fd);
                        FileInputStream fis = new ParcelFileDescriptor.AutoCloseInputStream(pfd);
                        Log.i(TAG, "zero-CPU pipe watcher active (fd=" + fd + ")");
                        byte[] buf = new byte[32];
                        try {
                            while (fis.read(buf) > 0) {
                                Handler wh;
                                synchronized (STATE_LOCK) {
                                    wh = sHandler;
                                }
                                if (wh != null) {
                                    wh.removeCallbacks(SYNC_IDLE_RUNNABLE);
                                    wh.post(SYNC_IDLE_RUNNABLE);
                                }
                            }
                        } catch (Throwable err) {
                            Log.w(TAG, "pipe watcher error: " + err);
                        } finally {
                            try { fis.close(); } catch (Throwable ignored) {}
                        }
                    }
                }, "NyakoLtpoPipe");
                t.setDaemon(true);
                t.start();
            }
        }
        onPropertiesChanged();
    }

    private static boolean shouldOverrideLowRateToLtpo(Display.Mode mode) {
        float hz = mode.getRefreshRate();
        if (hz >= 55.0f && hz <= 65.0f) {
            return sTouchActive || (sIdleLevel != 0) || syspropBool(P_IME_SHOWN, false);
        }
        return false;
    }

    private static DisplayModeDirector.DesiredDisplayModeSpecs force(
            Object director, int displayId, DisplayModeDirector.DesiredDisplayModeSpecs specs) throws Exception {
        int forceMode = syspropInt(P_FORCE, 0);
        if (specs == null || specs.baseModeId < 0 || displayId != 0 || forceMode <= 0) {
            return specs;
        }
        boolean isolated;
        Display.Mode target;
        Object lock = field(director, "mLock");
        synchronized (lock) {
            @SuppressWarnings("unchecked")
            SparseArray<Display.Mode[]> byDisp = (SparseArray<Display.Mode[]>) field(director, "mSupportedModesByDisplay");
            Display.Mode[] modes = byDisp.get(displayId);
            target = findMode(modes, forceMode);
            Display.Mode orig = findMode(modes, specs.baseModeId);
            if (orig == null && specs.baseModeId == 0) {
                try {
                    @SuppressWarnings("unchecked")
                    SparseArray<Display.Mode> defByDisp = (SparseArray<Display.Mode>) field(director, "mDefaultModeByDisplay");
                    if (defByDisp != null) orig = defByDisp.get(displayId);
                } catch (Throwable ignored) {}
            }
            if (target == null || orig == null || !sameSize(orig, target)) {
                return specs;
            }
            isolated = isIsolatedVariant(modes, target);
            if (isolated) {
                Object bo = field(director, "mBrightnessObserver");
                int state = ((Integer) field(bo, "mDefaultDisplayState")).intValue();
                if (state != 3 && state != 4) {
                    if (state != 2) return specs;
                    if (!sameRate(orig.getRefreshRate(), target.getRefreshRate())
                        && !shouldOverrideLowRateToLtpo(orig)) {
                        return specs;
                    }
                }
            }
        }

        if (!isolated) {
            if (!physicalRangeContains(field(specs, "primary"), target.getRefreshRate())
                || !physicalRangeContains(field(specs, "appRequest"), target.getRefreshRate())) {
                return specs;
            }
        } else {
            // Respect user's explicit 60Hz cap if primary physical max < 120Hz (unless DOZE or idle override)
            if (!physicalRangeContains(field(specs, "primary"), target.getRefreshRate())
                && !shouldOverrideLowRateToLtpo(target)) {
                // Keep original logic: allow LTPO mode when 60Hz overridden by active/idle LTPO
            }
        }

        boolean allowGrp = !isolated && (syspropInt(P_ALLOW, 0) == 1 || specs.allowGroupSwitching);
        if (specs.baseModeId == target.getModeId() && specs.allowGroupSwitching == allowGrp) {
            return specs;
        }
        DisplayModeDirector.DesiredDisplayModeSpecs out = new DisplayModeDirector.DesiredDisplayModeSpecs();
        out.copyFrom(specs);
        out.baseModeId = target.getModeId();
        out.allowGroupSwitching = allowGrp;
        widenSpecsMax(out, target.getRefreshRate());
        return out;
    }

    // === HOOK 0: DisplayModeDirector.getDesiredDisplayModeSpecs(int) ===
    public static DisplayModeDirector.DesiredDisplayModeSpecs getDesiredDisplayModeSpecs(Object self, int displayId) {
        DisplayModeDirector.DesiredDisplayModeSpecs orig =
            (DisplayModeDirector.DesiredDisplayModeSpecs) NyakoBridge.invokeOriginal(0, self, new Object[] { displayId });
        try {
            ensurePropertyCallback(self);
        } catch (Throwable t) {
            Log.w(TAG, "callback setup failed: " + t);
        }
        try {
            return force(self, displayId, orig);
        } catch (Throwable t) {
            Log.e(TAG, "force failed: " + t);
            return orig;
        }
    }

    // === HOOK 1: LocalDisplayAdapter$LocalDisplayDevice.findSfDisplayModeIdLocked(int, int) ===
    public static int findSfDisplayModeIdLocked(Object self, int displayModeId, int defaultModeId) {
        int origSfId = ((Integer) NyakoBridge.invokeOriginal(
            1, self, new Object[] { displayModeId, defaultModeId })).intValue();
        try {
            if (!((Boolean) field(self, "mIsFirstDisplay")).booleanValue()) {
                return origSfId;
            }
            synchronized (STATE_LOCK) {
                sDevice = new WeakReference<Object>(self);
            }
            int forceMode = syspropInt(P_FORCE, 0);
            if (forceMode <= 0) return origSfId;

            int state = ((Integer) field(self, "mState")).intValue();
            int effModeId = displayModeId;
            if (state == 3 || state == 4) {
                effModeId = forceMode;
            } else if (state == 2 && displayModeId == 4 && sIdleLevel != 0) {
                effModeId = forceMode;
            }
            if (effModeId != forceMode) return origSfId;

            @SuppressWarnings("unchecked")
            SparseArray<Object> supported = (SparseArray<Object>) field(self, "mSupportedModes");
            Object record = supported.get(effModeId);
            if (record == null) return origSfId;
            Display.Mode dm = (Display.Mode) field(record, "mMode");
            if (dm.getModeId() != effModeId) return origSfId;
            int sfId = ((Integer) Display.Mode.class.getMethod("getSfModeId").invoke(dm)).intValue();
            if (sfId < 0) return origSfId;

            Object[] sfModes = (Object[]) field(self, "mSfDisplayModes");
            for (Object sfMode : sfModes) {
                int id = ((Integer) field(sfMode, "id")).intValue();
                if (id == sfId && matchesSfMode(dm, sfMode)) {
                    int wantGroup = 0;
                    if (state == 3 || state == 4 || syspropInt(P_IDLE_TEST, 0) == 1) {
                        wantGroup = GROUP_IDLE_1;
                    } else if (state == 2 && syspropInt(P_AUTO_IDLE, 0) == 1
                               && !sTouchActive && sPanelAllowed && sPointerReady) {
                        if (sIdleLevel == 1) {
                            wantGroup = GROUP_IDLE_1;
                        } else if (sIdleLevel == 10) {
                            wantGroup = GROUP_IDLE_10;
                        }
                    }

                    int curGroup = ((Integer) field(sfMode, "group")).intValue();
                    if (wantGroup != 0 && curGroup == GROUP_AUTO_1) {
                        for (Object cand : sfModes) {
                            if (((Integer) field(cand, "group")).intValue() == wantGroup
                                && matchesSfMode(dm, cand)) {
                                sfId = ((Integer) field(cand, "id")).intValue();
                                break;
                            }
                        }
                    }

                    String msg = "MAP DM " + effModeId + ": SF " + origSfId + " -> " + sfId;
                    synchronized (STATE_LOCK) {
                        if (!msg.equals(sLastMapping)) {
                            log(msg);
                            sLastMapping = msg;
                        }
                    }
                    return sfId;
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "SF mapping failed: " + t);
        }
        return origSfId;
    }

    // === HOOK 2: DisplayModeDirector$BrightnessObserver.setDefaultDisplayState(int) ===
    public static void setDefaultDisplayState(Object self, int state) {
        disableLightSensorAlwaysOn();
        int prev = -1;
        try {
            prev = ((Integer) field(self, "mDefaultDisplayState")).intValue();
        } catch (Throwable ignored) {}
        NyakoBridge.invokeOriginal(2, self, new Object[] { state });
        if (prev >= 0 && prev != state) {
            if (state != 2) {
                sIdleLevel = 0;
                sTouchActive = false;
            }
            triggerDirectorNotify();
        }
    }
}
