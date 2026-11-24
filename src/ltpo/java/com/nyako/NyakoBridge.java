package com.nyako;

public final class NyakoBridge {
    private NyakoBridge() {}
    public static native Object hookDispatch0();
    public static native Object hookDispatch1(Object a0);
    public static native Object hookDispatch2(Object a0, Object a1);
    public static native Object hookDispatch3(Object a0, Object a1, Object a2);
    public static native Object hookDispatch4(Object a0, Object a1, Object a2, Object a3);
    public static native Object invokeOriginal(int slot, Object self, Object[] args);
}
