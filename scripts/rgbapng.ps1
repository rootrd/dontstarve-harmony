# Convert a dumped RGBA8 frame (WxH, no header, name carries the size) into a PNG.
# Frames come off the device via DM_RAW / DM_RAWDIR - never through the log channel, which
# silently drops long and burst lines.
#
#   rgbapng.ps1 <file.rgba | directory of them> [-dst out.png] [-w W -h H]
#               [-x X -y Y -cw CW -ch CH] [-scale N]
#
# Needs -ReferencedAssemblies System.Drawing on the Add-Type, and the C# body must stay
# ASCII-only (Add-Type reads the script as ANSI).
param([string]$src, [string]$dst = '', [int]$w = 0, [int]$h = 0,
      [int]$x = 0, [int]$y = 0, [int]$cw = 0, [int]$ch = 0, [int]$scale = 1)
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Drawing;
using System.Drawing.Imaging;
using System.Drawing.Drawing2D;
public static class Rgba2Png {
  public static string One(string src, string dst, int w, int h, int x, int y, int cw, int ch, int scale) {
    byte[] b = File.ReadAllBytes(src);
    if (w <= 0 || h <= 0) {                       // fall back to WxH in the file name
      string nm = Path.GetFileName(src); int i = nm.LastIndexOf('_'); int k = nm.IndexOf('x', i);
      w = int.Parse(nm.Substring(i + 1, k - i - 1)); h = int.Parse(nm.Substring(k + 1, nm.Length - k - 6));
    }
    if (b.Length < w * h * 4) return "short " + b.Length;
    using (var full = new Bitmap(w, h, PixelFormat.Format32bppArgb)) {
      BitmapData d = full.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
      byte[] row = new byte[w * 4];
      for (int j = 0; j < h; j++) {
        int s = j * w * 4;
        for (int i2 = 0; i2 < w; i2++) {          // RGBA in memory -> BGRA for the bitmap
          row[i2*4] = b[s+i2*4+2]; row[i2*4+1] = b[s+i2*4+1]; row[i2*4+2] = b[s+i2*4]; row[i2*4+3] = 255;
        }
        Marshal.Copy(row, 0, IntPtr.Add(d.Scan0, j * d.Stride), w * 4);
      }
      full.UnlockBits(d);
      if (cw <= 0) cw = w - x; if (ch <= 0) ch = h - y; if (scale <= 0) scale = 1;
      using (var o = new Bitmap(cw * scale, ch * scale, PixelFormat.Format24bppRgb)) {
        using (Graphics g = Graphics.FromImage(o)) {
          g.InterpolationMode = InterpolationMode.NearestNeighbor;   // never smooth a diagnostic image
          g.PixelOffsetMode = PixelOffsetMode.Half;
          g.DrawImage(full, new Rectangle(0, 0, cw * scale, ch * scale), new Rectangle(x, y, cw, ch), GraphicsUnit.Pixel);
        }
        o.Save(dst, ImageFormat.Png);
      }
    }
    return "ok " + cw + "x" + ch + "@" + scale;
  }
}
"@ -ReferencedAssemblies System.Drawing

if (Test-Path -PathType Container $src) {
  $n = 0
  Get-ChildItem -Path (Join-Path $src '*.rgba') | Sort-Object Name | ForEach-Object {
    $p = [Rgba2Png]::One($_.FullName, ($_.FullName -replace '\.rgba$', '.png'), $w, $h, $x, $y, $cw, $ch, $scale)
    if ($p -notlike 'ok*') { Write-Output ("BAD " + $_.Name + " " + $p) } else { $n++ }
  }
  Write-Output "converted $n frames -> $src"
} else {
  if ($dst -eq '') { $dst = $src -replace '\.rgba$', '.png' }
  Write-Output ([Rgba2Png]::One($src, $dst, $w, $h, $x, $y, $cw, $ch, $scale))
}
