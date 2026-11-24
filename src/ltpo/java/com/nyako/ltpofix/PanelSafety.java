package com.nyako.ltpofix;

import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class PanelSafety {
    private static final String ROOT = "/sys/class/mi_display/disp-DSI-0/";
    private static final Pattern HBM = Pattern.compile(
        "(?m)^\\s*(hbm|hbm_fod|fod_calibration_hbm|local_hbm|lcd_hbm|peak_hdr_mode)\\[\\d+\\]:\\s*(\\d+)\\s*$");
    private static final Pattern HBM_NAME = Pattern.compile(
        "^\\s*(hbm|hbm_fod|fod_calibration_hbm|local_hbm|lcd_hbm|peak_hdr_mode)(?=\\[|\\s|:).*");

    private static String sysprop(String key) {
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            return (String) sp.getMethod("get", String.class, String.class).invoke(null, key, "");
        } catch (Throwable t) {
            return "";
        }
    }

    private static String readFile(String name) throws Exception {
        FileInputStream fis = new FileInputStream(ROOT + name);
        try {
            byte[] buf = new byte[4096];
            int total = 0;
            int n;
            while (total < buf.length && (n = fis.read(buf, total, buf.length - total)) > 0) {
                total += n;
            }
            return new String(buf, 0, total, StandardCharsets.UTF_8);
        } finally {
            fis.close();
        }
    }

    private static boolean falseOrUnset(String v) {
        return v == null || v.isEmpty() || "0".equals(v) || "false".equalsIgnoreCase(v)
            || "n".equalsIgnoreCase(v) || "no".equalsIgnoreCase(v) || "off".equalsIgnoreCase(v);
    }

    static boolean allowed() {
        try {
            int br = Integer.parseInt(readFile("brightness_clone").trim());
            int maxBr = Integer.parseInt(readFile("max_brightness_clone").trim());
            if (maxBr <= 0 || br <= 0 || br > maxBr) return false;
            if (!"1,1:0".equals(sysprop("ro.vendor.display.primary_idle_refresh_rate"))) return false;
            if (!"0".equals(sysprop("persist.vendor.disable_idle_fps.threshold"))) return false;
            if (!falseOrUnset(sysprop("persist.vendor.mi_sf.force_skip_idle_fps"))) return false;
            if (!falseOrUnset(sysprop("persist.vendor.disable_idle_fps"))) return false;
            return hbmInactive(readFile("disp_param"));
        } catch (Throwable t) {
            return false;
        }
    }

    private static boolean hbmInactive(String s) {
        if (s == null) return false;
        HashSet<String> seen = new HashSet<String>();
        String[] lines = s.split("\\r?\\n");
        for (String line : lines) {
            if (!HBM_NAME.matcher(line).matches()) continue;
            Matcher m = HBM.matcher(line);
            if (!m.matches()) return false;
            if (!"0".equals(m.group(2))) return false;
            seen.add(m.group(1));
        }
        return !seen.isEmpty();
    }
}
