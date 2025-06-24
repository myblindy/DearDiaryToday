using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Windows.Win32.Foundation;

namespace DearDiaryTodayCs;
public static partial class DearDiaryToday
{
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate void ErrorCallback(HRESULT hr);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate void InitializeDiaryCompletion(bool hasDirtyDiary, IntPtr arg);

    [DllImport("deardiarytoday.dll", EntryPoint = "InitializeDiary", CallingConvention = CallingConvention.StdCall)]
    static extern void RawInitializeDiary(ErrorCallback errorFunc,
        InitializeDiaryCompletion initializeDiaryCompletion, IntPtr initializeDiaryCompletionArg);

    [DllImport("deardiarytoday.dll", EntryPoint = "StartDiary", CallingConvention = CallingConvention.StdCall)]
    static extern void RawStartDiary(HWND hWnd);

    static readonly ConcurrentDictionary<int, TaskCompletionSource<bool>> initializeDiaryTCS = [];
    static int nextInitializeDiaryCompletionId = 0;
    static readonly InitializeDiaryCompletion initializeDiaryCompletion = (hasDirtyDiary, arg) =>
    {
        initializeDiaryTCS[arg.ToInt32()].SetResult(hasDirtyDiary);
        initializeDiaryTCS.TryRemove(arg.ToInt32(), out _);
    };

    static readonly ErrorCallback errorCallback = hr =>
    {
        if (!hr.Succeeded)
            throw new InvalidOperationException($"DearDiaryToday error with HRESULT: {hr}");
    };

    /// <summary>
    /// Starts the diary recording. If any previous diaries are present, they are assumed to be left-overs of a crash
    /// and optionally can be saved to a video file before starting a new recording.
    /// </summary>
    /// <returns><see langword="true"/> if a dirty recording was saved.</returns>
    public static async Task<bool> StartDiary(IntPtr hWnd, Func<Task<string?>>? exportOnDirtyAction = null)
    {
        var tcs = new TaskCompletionSource<bool>();
        var id = Interlocked.Increment(ref nextInitializeDiaryCompletionId);
        initializeDiaryTCS[id] = tcs;

        RawInitializeDiary(errorCallback, initializeDiaryCompletion, new(id));

        var hasDirtyDiaryFiles = await tcs.Task.ConfigureAwait(false);

        if (hasDirtyDiaryFiles && exportOnDirtyAction is not null
            && await exportOnDirtyAction() is { } exportVideoFileName)
        {
            await ExportDiaryVideo(exportVideoFileName);
        }

        RawStartDiary(new(hWnd));

        return hasDirtyDiaryFiles;
    }

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate void ExportDiaryVideoCompletion(float percentDone, IntPtr arg);

    [DllImport("deardiarytoday.dll", EntryPoint = "ExportDiaryVideo", CallingConvention = CallingConvention.StdCall)]
    static extern void RawExportDiaryVideo([MarshalAs(UnmanagedType.LPWStr)] string outputFileName,
        ExportDiaryVideoCompletion completion, IntPtr completionArg);

    static readonly ConcurrentDictionary<int, (TaskCompletionSource<bool> tcs, Action<float>? progress)> exportDiaryVideoTCS = [];
    static int nextExportDiaryVideoCompletionId = 0;
    static readonly ExportDiaryVideoCompletion exportDiaryVideoCompletion = (percentDone, arg) =>
    {
        exportDiaryVideoTCS[arg.ToInt32()].progress?.Invoke(percentDone);
        if (percentDone < 0)
        {
            exportDiaryVideoTCS[arg.ToInt32()].tcs.SetResult(true);
            exportDiaryVideoTCS.TryRemove(arg.ToInt32(), out _);
        }
    };

    /// <summary>
    /// Saves the current diary recording to a video file, and resets the recording buffers.
    /// </summary>
    public static Task ExportDiaryVideo(string outputFileName, Action<float>? progress = null)
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
    static readonly ConcurrentDictionary<int, TaskCompletionSource<bool>> stopDiaryTCS = [];

    [DllImport("deardiarytoday.dll", EntryPoint = "StopDiary", CallingConvention = CallingConvention.StdCall)]
    static extern void RawStopDiary(StopDiaryCompletion completion, IntPtr completionArg);

    static readonly StopDiaryCompletion stopDiaryCompletion = arg =>
    {
        stopDiaryTCS[arg.ToInt32()].SetResult(true);
        stopDiaryTCS.TryRemove(arg.ToInt32(), out _);
    };

    /// <summary>
    /// Stops the diary recording in progress.
    /// </summary>
    public static Task StopDiary()
    {
        var tcs = new TaskCompletionSource<bool>();
        var id = Interlocked.Increment(ref nextStopDiaryCompletionId);
        stopDiaryTCS[id] = tcs;

        RawStopDiary(stopDiaryCompletion, new(id));
        return tcs.Task;
    }
}
