using DearDiaryTodayCs;
using System.Windows;
using System.Windows.Interop;

namespace TestApp;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
    }

    private void WindowLoaded(object sender, RoutedEventArgs e)
    {
        DearDiaryToday.StartDiary(new WindowInteropHelper(this).Handle);
    }
}