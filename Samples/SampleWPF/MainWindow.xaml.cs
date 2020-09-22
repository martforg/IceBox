﻿using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Navigation;
using System.Windows.Shapes;

namespace SampleWPF
{
    class WindowHost : HwndHost
    {
        [DllImport("IBEngine.dll")]
        private static extern IntPtr IB_createWindow(IntPtr parentWindowHandle, string name, int width, int height);

        [DllImport("IBEngine.dll")]
        private static extern void IB_destroyWindow(IntPtr hwnd);

        protected override HandleRef BuildWindowCore(HandleRef hwndParent)
        {
            IntPtr windowHandle = IB_createWindow(hwndParent.Handle, "TestWPFWindow", 500, 150);
            return new HandleRef(this, windowHandle);
        }

        protected override void DestroyWindowCore(HandleRef hwnd)
        {
            IB_destroyWindow(hwnd.Handle);
        }
    }

    /// <summary>
    /// Interaction logic for MainWindow.xaml
    /// </summary>
    public partial class MainWindow : Window
    {
        public MainWindow()
        {
            InitializeComponent();
            WindowReserve.Children.Add(new WindowHost());
        }
    }
}