# DearDiaryToday

[![NuGet version (MB.DearDiaryToday.x64)](https://img.shields.io/nuget/v/MB.DearDiaryToday.x64.svg?style=flat-square)](https://www.nuget.org/packages/MB.DearDiaryToday.x64/)

This library helps debug issues as they're happening by keeping a video log of the last certain amount of time. It can save the recorded data to a video file on demand, or after a crash. It can work with any kind of window as long as you can get its HWND, be it WinForms, WPF, WinUI3, Win32 etc.

The API used internally is [Windows.Graphics.Capture](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture?view=winrt-26100) to record, and [Media Foundation](https://learn.microsoft.com/en-us/windows/win32/medfound/microsoft-media-foundation-sdk) to export the video. As such, the minimum requirements are as follows:

|OS Requirement | Feature|
|-|-|
| Win10 | Base functionality|
| Win11 | Ability to hide the yellow capture border around the window |

The API is designed to be very simple. To start the process, simply call `StartDiary`:

```C#
await DearDiaryToday.StartDiary(hWnd, async () => crashVideoFileName);
```

The first parameter is the handle of the window to monitor, and the second optional parameter is an async `Task` that returns a file name for the crash video data, if any. Since it's a `Task`, you can take your time to show a save dialog, or to query configuration files to determine where to save the video file. The function will return true if a crash file was detected and saved.

To save a video file at run-time, call the `ExportDiaryVideo` function:

```C#
await DearDiaryToday.ExportDiaryVideo(outputFileName, progress => /* ... */);
```

The first parameter is the video file name to save, and the second is a callback that receives a progress percentage between 0.0 and 1.0. Once the export is finished, the progress callback will be called with a -1, though of course the `Task` itself will also complete, so you can simply `await` it instead.

Since crash data is important, it's equally important to shut down cleanly, since any left over files will be treated as crash data and saved during `StartDiary`. As such, you need to call `StopDiary` when the application is shutting down, and `await` it to completion:

```C#
await DearDiaryToday.StopDiary();
```

You can see it implemented for a WPF window (with a bunch of WPF-specific boilerplate) in the demo project here: https://github.com/myblindy/DearDiaryToday/blob/master/TestApp/MainWindow.xaml.cs.