// Native acceptance helper. Callers operate only on their freshly launched emulator process.
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.IO;
public static class Stage10WindowProbe {
    public static bool GgMode;
    [DllImport("user32.dll")] static extern IntPtr GetMenu(IntPtr window);
    [DllImport("user32.dll")] static extern uint GetMenuState(IntPtr menu,uint id,uint flags);
    public static bool MenuEnabled(IntPtr window,uint id) { var state=GetMenuState(GetMenu(window),id,0);return state!=UInt32.MaxValue && (state&3)==0; }
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X,Y; }
    [StructLayout(LayoutKind.Sequential)] public struct BitmapInfo {
        public uint Size; public int Width,Height; public ushort Planes,Bits;
        public uint Compression,ImageSize; public int XMeter,YMeter; public uint Colors,Important;
    }
    public delegate bool EnumProc(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc proc, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr window, EnumProc proc, IntPtr data);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll")] static extern IntPtr GetWindowLongPtrW(IntPtr window, int index);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool PostMessageW(IntPtr window, uint message, IntPtr value, IntPtr data);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] static extern IntPtr SendText(IntPtr window, uint message, IntPtr value, string text);
    [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] static extern IntPtr ReadText(IntPtr window, uint message, IntPtr value, StringBuilder text);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int w, int h, uint flags);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr window);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr window, IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int width, int height);
    [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr bitmap);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr value);
    [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern bool BitBlt(IntPtr target,int x,int y,int w,int h,IntPtr source,int sx,int sy,uint operation);
    [DllImport("gdi32.dll")] static extern int GetDIBits(IntPtr dc,IntPtr bitmap,uint first,uint count,byte[] pixels,ref BitmapInfo info,uint usage);
    public static string Class(IntPtr window) { var t = new StringBuilder(256); GetClassName(window,t,t.Capacity); return t.ToString(); }
    public static string Title(IntPtr window) { var t = new StringBuilder(1024); GetWindowText(window,t,t.Capacity); return t.ToString(); }
    public static IntPtr Find(int pid, string type, IntPtr owner) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((w,d) => { uint p; GetWindowThreadProcessId(w,out p);
            if (p == pid && Class(w) == type && (owner == IntPtr.Zero || GetWindow(w,4) == owner)) found=w;
            return found == IntPtr.Zero;
        },IntPtr.Zero);
        return found;
    }
    public static string[] Children(IntPtr parent) {
        var rows=new List<string>();
        EnumChildWindows(parent,(w,d)=> { var p=GetParent(w); rows.Add(Class(w)+" id="+GetDlgCtrlID(w)+" visible="+IsWindowVisible(w)+" parent="+Class(p)+" parentId="+GetDlgCtrlID(p)+(Class(w)=="Button"?" text="+Title(w):"")); return true; },IntPtr.Zero);
        return rows.ToArray();
    }
    public static IntPtr Child(IntPtr parent, string type, int id) {
        IntPtr found=IntPtr.Zero;
        EnumChildWindows(parent,(w,d)=> { if(Class(w)==type && GetDlgCtrlID(w)==id) found=w; return found==IntPtr.Zero; },IntPtr.Zero);
        return found;
    }
    public static IntPtr ButtonMnemonic(IntPtr parent,string mnemonic) {
        IntPtr found=IntPtr.Zero;
        EnumChildWindows(parent,(w,d)=> {if(Class(w)=="Button" && IsWindowVisible(w) && Title(w).Contains(mnemonic)) found=w;return found==IntPtr.Zero;},IntPtr.Zero);
        return found;
    }
    public static void SetText(IntPtr window,string value) { if(SendText(window,12,IntPtr.Zero,value)==IntPtr.Zero) throw new Exception("WM_SETTEXT failed"); }
    public static string DialogText(IntPtr window) {
        var texts=new List<string>();
        EnumChildWindows(window,(w,d)=> { long style=GetWindowLongPtrW(w,-16).ToInt64()&31; if(Class(w)=="Static" && style!=3 && style!=14) { var text=new StringBuilder(4096); ReadText(w,13,new IntPtr(text.Capacity),text); if(text.Length>0) texts.Add(text.ToString()); } return true; },IntPtr.Zero);
        return String.Join(" / ",texts);
    }
    public static string[] Dialogs(int pid) {
        var rows=new List<string>();
        EnumWindows((w,d)=> {uint p;GetWindowThreadProcessId(w,out p);
            if(p==pid && Class(w)=="#32770") rows.Add(Title(w)+": "+DialogText(w)+" ["+String.Join(", ",Children(w))+"]");
            return true;
        },IntPtr.Zero);
        return rows.ToArray();
    }
    public static void Place(IntPtr window,int width,int height) {
        var previous=SetThreadDpiAwarenessContext(new IntPtr(-4));
        try { if(!SetWindowPos(window,new IntPtr(-1),200,100,width,height,0x50)) throw new Exception("SetWindowPos failed"); }
        finally { SetThreadDpiAwarenessContext(previous); }
    }
    public static int Capture(IntPtr window,string path) {
        var previous=SetThreadDpiAwarenessContext(new IntPtr(-4));
        IntPtr screen=IntPtr.Zero, memory=IntPtr.Zero, bitmap=IntPtr.Zero, old=IntPtr.Zero;
        try {
            Rect r; var origin=new Point();
            if(!GetClientRect(window,out r) || !ClientToScreen(window,ref origin)) throw new Exception("Client geometry failed");
            int w=r.Right,h=r.Bottom;
            screen=GetDC(IntPtr.Zero); memory=CreateCompatibleDC(screen); bitmap=CreateCompatibleBitmap(screen,w,h);
            if(screen==IntPtr.Zero || memory==IntPtr.Zero || bitmap==IntPtr.Zero) throw new Exception("Capture allocation failed");
            old=SelectObject(memory,bitmap);
            if(!BitBlt(memory,0,0,w,h,screen,origin.X,origin.Y,0x40cc0020)) throw new Exception("BitBlt failed");
            SelectObject(memory,old); old=IntPtr.Zero;
            var info=new BitmapInfo { Size=40,Width=w,Height=-h,Planes=1,Bits=32 };
            byte[] bytes=new byte[w*h*4];
            if(GetDIBits(screen,bitmap,0,(uint)h,bytes,ref info,0)!=h) throw new Exception("GetDIBits failed");
            using(var output=new BinaryWriter(File.Create(path))) {
                output.Write((ushort)0x4d42); output.Write(54+bytes.Length); output.Write(0); output.Write(54);
                output.Write(40); output.Write(w); output.Write(-h); output.Write((ushort)1); output.Write((ushort)32);
                output.Write(0); output.Write(bytes.Length); output.Write(0); output.Write(0); output.Write(0); output.Write(0); output.Write(bytes);
            }
            int sourceWidth=GgMode?160:256, sourceHeight=GgMode?144:192;
            int scale=Math.Min(w/sourceWidth,h/sourceHeight);
            if(scale==0) throw new Exception("Probe requires native size or larger");
            int x=(w-scale*sourceWidth)/2+scale/2, y=(h-scale*sourceHeight)/2+scale/2, at=(y*w+x)*4;
            return bytes[at+2]*65536+bytes[at+1]*256+bytes[at];
        } finally {
            if(old!=IntPtr.Zero) SelectObject(memory,old);
            if(bitmap!=IntPtr.Zero) DeleteObject(bitmap);
            if(memory!=IntPtr.Zero) DeleteDC(memory);
            if(screen!=IntPtr.Zero) ReleaseDC(IntPtr.Zero,screen);
            SetThreadDpiAwarenessContext(previous);
        }
    }
    public static int CheckerMismatches(string path,int color) {
        byte[] pixels=File.ReadAllBytes(path);
        int w=BitConverter.ToInt32(pixels,18), h=-BitConverter.ToInt32(pixels,22), start=BitConverter.ToInt32(pixels,10);
        int sourceWidth=GgMode?160:256,sourceHeight=GgMode?144:192;
        int scale=Math.Min(w/sourceWidth,h/sourceHeight), width=scale*sourceWidth,height=scale*sourceHeight;
        int left=(w-width)/2, top=(h-height)/2, errors=0;
        for(int y=0;y<height;y++) for(int x=0;x<width;x++) {
            int at=start+((top+y)*w+left+x)*4;
            int actual=pixels[at+2]*65536+pixels[at+1]*256+pixels[at];
            int expected;
            if(GgMode) {
                int sx=48+x/scale,sy=24+y/scale,tx=sx/8,ty=sy/8;
                int index=(tx+3*ty)%15+1+(((tx+ty)&1)!=0?16:0);
                if((tx==6||tx==25)&&(ty==3||ty==20)) index=((sx+sy)&1)!=0?15:1+(tx==25?1:0)+(ty==20?2:0);
                int raw=(color==0xeeccaa && index==1)?0xace:(index*73+0x123)&0xfff;
                expected=((raw&15)*17)*65536+(((raw>>4)&15)*17)*256+((raw>>8)&15)*17;
            } else expected=((x/scale+y/scale)%2)==0 ? color : 255;
            if(actual!=expected) errors++;
        }
        return errors;
    }
}
