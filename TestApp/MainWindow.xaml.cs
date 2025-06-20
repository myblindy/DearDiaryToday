using DearDiaryTodayCs;
using System.Diagnostics;
using System.Numerics;
using System.Windows;
using System.Windows.Interop;

namespace TestApp;

public partial class MainWindow : Window
{
    readonly PeriodicTimer animationTimer = new(TimeSpan.FromSeconds(1.0 / 30));
    readonly PeriodicTimer counterTimer = new(TimeSpan.FromSeconds(1.0));

    public MainWindow()
    {
        InitializeComponent();

        Loaded += (s, e) =>
        {
            async Task BounceAsync()
            {
                var angle = Random.Shared.NextSingle() * 2 * MathF.PI;
                var v = new Vector2(MathF.Cos(angle), MathF.Sin(angle));

                while (true)
                {
                    BouncerRotateTransform.Angle += 5;
                    Bouncer.Margin = new(
                        Math.Clamp(Bouncer.Margin.Left + v.X * 4, 0, ActualWidth - Bouncer.ActualWidth),
                        Math.Clamp(Bouncer.Margin.Top + v.Y * 4, 0, ActualHeight - Bouncer.ActualHeight),
                        0, 0);

                    if (Bouncer.Margin.Left == 0 || Bouncer.Margin.Left + Bouncer.ActualWidth == ActualWidth) v.X = -v.X;
                    if (Bouncer.Margin.Top == 0 || Bouncer.Margin.Top + Bouncer.ActualHeight == ActualHeight) v.Y = -v.Y;

                    await animationTimer.WaitForNextTickAsync();
                }
            }
            _ = BounceAsync();

            async Task CountAsync()
            {
                var count = 0;
                while (true)
                {
                    Counter.Text = $"{++count}";
                    await counterTimer.WaitForNextTickAsync();
                }
            }
            _ = CountAsync();
        };
    }

    private async void WindowLoaded(object sender, RoutedEventArgs e)
    {
        var crashVideoFileName = $".diary\\crash-{DateTime.Now:yyyyMMddHHmmss}.mp4";
        if (await DearDiaryToday.StartDiary(new WindowInteropHelper(this).Handle, async () => crashVideoFileName))
            Process.Start(new ProcessStartInfo(crashVideoFileName) { UseShellExecute = true });
    }

    private async void SaveClicked(object sender, RoutedEventArgs e)
    {
        var outputFileName = $".diary\\{DateTime.Now:yyyyMMddHHmmss}.mp4";
        await DearDiaryToday.ExportDiaryVideo(outputFileName, progress => Dispatcher.BeginInvoke(() =>
        {
            SaveProgress.Value = progress;
            SaveProgress.Visibility = progress is >= 0 and <= 1 ? Visibility.Visible : Visibility.Collapsed;
        }));
        Process.Start(new ProcessStartInfo(outputFileName) { UseShellExecute = true });
    }

    bool OkToClose;
    private async void WindowClosing(object sender, System.ComponentModel.CancelEventArgs e)
    {
        if (!OkToClose)
        {
            e.Cancel = true;
            await DearDiaryToday.StopDiary();
            OkToClose = true;
            Close();
        }
    }
}