param([Parameter(Mandatory=$true)][string]$Exe, [Parameter(Mandatory=$true)][string]$Version)
$ErrorActionPreference = 'Stop'
$info = [Diagnostics.FileVersionInfo]::GetVersionInfo($Exe)
$expected = [version]("$Version.0")
$file = [version]::new($info.FileMajorPart,$info.FileMinorPart,$info.FileBuildPart,$info.FilePrivatePart)
$product = [version]::new($info.ProductMajorPart,$info.ProductMinorPart,$info.ProductBuildPart,$info.ProductPrivatePart)
if ($file -ne $expected -or $product -ne $expected -or $info.FileVersion -ne "$expected" -or
    $info.ProductVersion -ne "$expected" -or $info.ProductName -cne 'acesms' -or
    $info.OriginalFilename -cne 'acesms.exe') {
    throw "Unexpected version resource in ${Exe}: file=$file product=$product name=$($info.ProductName)"
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class AcesmsResources {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr module);
    [DllImport("kernel32.dll", EntryPoint = "FindResourceW")] static extern IntPtr FindResource(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32.dll")] static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll")] static extern IntPtr LockResource(IntPtr memory);
    [DllImport("kernel32.dll")] static extern int SizeofResource(IntPtr module, IntPtr resource);

    public static string ApplicationManifest(string file) {
        IntPtr module = LoadLibraryEx(file, IntPtr.Zero, 0x22);
        if (module == IntPtr.Zero) throw new Exception("Cannot load EXE resources");
        try {
            IntPtr resource = FindResource(module, new IntPtr(1), new IntPtr(24));
            if (resource == IntPtr.Zero) throw new Exception("Application manifest missing");
            IntPtr data = LockResource(LoadResource(module, resource));
            int size = SizeofResource(module, resource);
            if (data == IntPtr.Zero || size <= 0) throw new Exception("Invalid application manifest resource");
            byte[] bytes = new byte[size];
            Marshal.Copy(data, bytes, 0, size);
            return System.Text.Encoding.UTF8.GetString(bytes).TrimStart('\uFEFF');
        } finally { FreeLibrary(module); }
    }

    public static string IconSizes(string file) {
        IntPtr module = LoadLibraryEx(file, IntPtr.Zero, 0x22);
        if (module == IntPtr.Zero) throw new Exception("Cannot load EXE resources");
        try {
            IntPtr group = FindResource(module, new IntPtr(100), new IntPtr(14));
            if (group == IntPtr.Zero) throw new Exception("Application icon group missing");
            IntPtr data = LockResource(LoadResource(module, group));
            int size = SizeofResource(module, group);
            if (data == IntPtr.Zero || size < 6 || Marshal.ReadInt16(data, 2) != 1)
                throw new Exception("Invalid icon group");
            int count = Marshal.ReadInt16(data, 4);
            if (size != 6 + 14 * count) throw new Exception("Invalid icon group size");
            var sizes = new int[count];
            for (int i = 0; i < count; ++i) {
                int entry = 6 + 14 * i;
                int width = Marshal.ReadByte(data, entry);
                int height = Marshal.ReadByte(data, entry + 1);
                int id = Marshal.ReadInt16(data, entry + 12);
                IntPtr icon = FindResource(module, new IntPtr(id), new IntPtr(3));
                if (width != height || icon == IntPtr.Zero || SizeofResource(module, icon) != Marshal.ReadInt32(data, entry + 8))
                    throw new Exception("Square icon image missing or invalid");
                sizes[i] = width == 0 ? 256 : width;
            }
            Array.Sort(sizes);
            return string.Join(",", sizes);
        } finally { FreeLibrary(module); }
    }
}
'@

$icons = [AcesmsResources]::IconSizes($Exe)
if ($icons -ne '16,24,32,48,64,256') { throw "Unexpected application icon sizes: $icons" }
[xml]$manifest = [AcesmsResources]::ApplicationManifest($Exe)
$dpi = $manifest.SelectSingleNode("//*[local-name()='dpiAwareness' and namespace-uri()='http://schemas.microsoft.com/SMI/2016/WindowsSettings']")
if ($null -eq $dpi -or $dpi.InnerText -cne 'PerMonitorV2') { throw "PerMonitorV2 missing from the application manifest in $Exe" }
"Windows resources PASS: version=$expected, icon sizes=$icons, DPI=$($dpi.InnerText)"
