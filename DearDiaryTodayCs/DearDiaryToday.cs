using System;
using System.Runtime.InteropServices;
using Windows.Win32.Foundation;

namespace DearDiaryTodayCs;
public static partial class DearDiaryToday
{
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate void ErrorCallback(HRESULT hr);

    [DllImport("deardiarytoday.dll", EntryPoint = "StartDiary", CallingConvention = CallingConvention.StdCall)]
    static extern void RawStartDiary(HWND hWnd, ErrorCallback errorFunc);

    static readonly ErrorCallback errorCallback = hr =>
    {
        if (!hr.Succeeded)
            throw new InvalidOperationException($"DearDiaryToday error with HRESULT: {hr}");
    };
    public static void StartDiary(IntPtr hWnd)
    {
        RawStartDiary(new(hWnd), errorCallback);
    }
}
