using DearDiaryTodayCs;
using System.Numerics;
using System.Windows;
using System.Windows.Interop;

namespace TestApp;

public partial class MainWindow : Window
{
    readonly PeriodicTimer animationTimer = new(TimeSpan.FromSeconds(1.0 / 30));

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
        };
    }

    private void WindowLoaded(object sender, RoutedEventArgs e)
    {
        DearDiaryToday.StartDiary(new WindowInteropHelper(this).Handle);
    }

    private void SaveClicked(object sender, RoutedEventArgs e)
    {
        DearDiaryToday.ExportDiaryVideo($".diary/{DateTime.Now:yyyyMMddHHmmss}.mp4");
    }
}