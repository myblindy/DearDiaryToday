using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
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

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate void ExportDiaryVideoCompletion(float percentDone, IntPtr arg);

    [DllImport("deardiarytoday.dll", EntryPoint = "ExportDiaryVideo", CallingConvention = CallingConvention.StdCall)]
    static extern void RawExportDiaryVideo([MarshalAs(UnmanagedType.LPWStr)] string outputFileName,
        ExportDiaryVideoCompletion completion, IntPtr completionArg);

    static readonly Dictionary<int, (TaskCompletionSource<bool> tcs, Action<float> progress)> exportDiaryVideoTCS = [];
    static int nextExportDiaryVideoCompletionId = 0;
    static readonly ExportDiaryVideoCompletion exportDiaryVideoCompletion = (percentDone, arg) =>
    {
        if (percentDone < 0)
        {
            exportDiaryVideoTCS[arg.ToInt32()].tcs.SetResult(true);
            exportDiaryVideoTCS.Remove(arg.ToInt32());
        }
        else
            exportDiaryVideoTCS[arg.ToInt32()].progress(percentDone);
    };
    public static Task ExportDiaryVideo(string outputFileName, Action<float> progress)
    {
        var tcs = new TaskCompletionSource<bool>();
        var id = Interlocked.Increment(ref nextExportDiaryVideoCompletionId);
        exportDiaryVideoTCS[id] = (tcs, progress);

        new Thread(() =>
        {
            RawExportDiaryVideo(outputFileName, exportDiaryVideoCompletion, new(id));
        }).Start();
        return tcs.Task;
    }

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate void StopDiaryCompletion(IntPtr arg);

    static int nextStopDiaryCompletionId = 0;
    static readonly Dictionary<int, TaskCompletionSource<bool>> stopDiaryTCS = [];

    [DllImport("deardiarytoday.dll", EntryPoint = "StopDiary", CallingConvention = CallingConvention.StdCall)]
    static extern void RawStopDiary(StopDiaryCompletion completion, IntPtr completionArg);

    static readonly StopDiaryCompletion stopDiaryCompletion = arg =>
    {
        stopDiaryTCS[arg.ToInt32()].SetResult(true);
        stopDiaryTCS.Remove(arg.ToInt32());
    };
    public static Task StopDiary()
    {
        var tcs = new TaskCompletionSource<bool>();
        var id = Interlocked.Increment(ref nextStopDiaryCompletionId);
        stopDiaryTCS[id] = tcs;

        RawStopDiary(stopDiaryCompletion, new(id));
        return tcs.Task;
    }
}
