# note

Windows 性能监视器使用 [System Informer](https://github.com/winsiderss/systeminformer)（原 Process Hacker），源码在 `SystemInformer/`，对应上游提交 `b409d5f3bb870d0a94c3e4c7acc6252a29478574`。

进程列表里每一行都有该程序的 CPU 和内存。双击某一行，打开属性窗口的 Performance 页，可以看到这个程序自己的 CPU、专用内存（Private bytes）和工作集曲线。按 CPU 或内存列排序就能找出占用最高的程序。这些数字来自 Windows 的 `NtQuerySystemInformation`，不需要安装内核驱动。

在装有 Visual Studio 2022 的 Windows 上打包：

```powershell
./SystemInformer/packaging/package-exe.ps1
```

脚本会编 Release，并把 64 位程序打成 `SystemInformer/packaging/output/SystemInformer-win64.zip`。解压后运行 `SystemInformer.exe`。
