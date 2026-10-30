Add-Type -AssemblyName System.Drawing

function Create-ApfsIcon {
    param([string]$OutputPath)

    $sizes = @(16, 24, 32, 48, 64, 128, 256)
    $images = @()

    foreach ($sz in $sizes) {
        $bmp = New-Object System.Drawing.Bitmap($sz, $sz, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.Clear([System.Drawing.Color]::Transparent)

        $scale = $sz / 256.0

        # Draw Drive Body (rounded rect)
        $margin = [math]::Max(1.0, 14.0 * $scale)
        $dw = $sz - ($margin * 2.0)
        $dh = $sz - ($margin * 2.0)
        $radius = [math]::Max(2.0, 36.0 * $scale)

        $driveRect = New-Object System.Drawing.RectangleF($margin, $margin, $dw, $dh)

        # Drop shadow / bottom rim
        $shadowY = $margin + [math]::Max(1.0, 4.0 * $scale)
        $shadowRect = [System.Drawing.RectangleF]::new([float]$margin, [float]$shadowY, [float]$dw, [float]$dh)
        $pathShadow = New-Object System.Drawing.Drawing2D.GraphicsPath
        $d = $radius * 2.0
        $pathShadow.AddArc($shadowRect.X, $shadowRect.Y, $d, $d, 180, 90)
        $pathShadow.AddArc($shadowRect.Right - $d, $shadowRect.Y, $d, $d, 270, 90)
        $pathShadow.AddArc($shadowRect.Right - $d, $shadowRect.Bottom - $d, $d, $d, 0, 90)
        $pathShadow.AddArc($shadowRect.X, $shadowRect.Bottom - $d, $d, $d, 90, 90)
        $pathShadow.CloseFigure()
        $shadowBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(100, 20, 25, 30))
        $g.FillPath($shadowBrush, $pathShadow)
        $shadowBrush.Dispose()
        $pathShadow.Dispose()

        # Drive main body path
        $pathBody = New-Object System.Drawing.Drawing2D.GraphicsPath
        $pathBody.AddArc($driveRect.X, $driveRect.Y, $d, $d, 180, 90)
        $pathBody.AddArc($driveRect.Right - $d, $driveRect.Y, $d, $d, 270, 90)
        $pathBody.AddArc($driveRect.Right - $d, $driveRect.Bottom - $d, $d, $d, 0, 90)
        $pathBody.AddArc($driveRect.X, $driveRect.Bottom - $d, $d, $d, 90, 90)
        $pathBody.CloseFigure()

        # Aluminum gradient
        $gradBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
            [System.Drawing.PointF]::new($driveRect.X, $driveRect.Y),
            [System.Drawing.PointF]::new($driveRect.Right, $driveRect.Bottom),
            [System.Drawing.Color]::FromArgb(255, 238, 242, 246),
            [System.Drawing.Color]::FromArgb(255, 175, 186, 196)
        )
        $g.FillPath($gradBrush, $pathBody)
        $gradBrush.Dispose()

        # Inner border / chamfer
        $borderPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 110, 125, 140), [math]::Max(1.0, 2.5 * $scale))
        $g.DrawPath($borderPen, $pathBody)
        $borderPen.Dispose()
        $pathBody.Dispose()

        # Center Apple logo / Mac emblem
        $cx = $sz / 2.0
        $cy = $sz / 2.0 - (4.0 * $scale)
        $appW = 100.0 * $scale
        $appH = 110.0 * $scale

        if ($sz -ge 24) {
            # Apple leaf
            $pathLeaf = New-Object System.Drawing.Drawing2D.GraphicsPath
            $leafW = 28.0 * $scale
            $leafH = 34.0 * $scale
            $lx = $cx + (4.0 * $scale)
            $ly = $cy - ($appH / 2.0) - (8.0 * $scale)
            $pathLeaf.AddArc($lx, $ly, $leafW, $leafH, 200, 140)
            $pathLeaf.AddArc($lx, $ly, $leafW, $leafH, 20, 140)
            $pathLeaf.CloseFigure()
            $appleBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 45, 52, 62))
            $g.FillPath($appleBrush, $pathLeaf)
            $pathLeaf.Dispose()

            # Apple body (overlapping circles / curves)
            $pathApple = New-Object System.Drawing.Drawing2D.GraphicsPath
            $bodyW = 76.0 * $scale
            $bodyH = 82.0 * $scale
            $bx = $cx - ($bodyW / 2.0)
            $by = $cy - ($bodyH / 2.0) + (8.0 * $scale)

            # Left lobe + right lobe
            $pathApple.AddEllipse($bx, $by, $bodyW * 0.72, $bodyH)
            $pathApple.AddEllipse($bx + ($bodyW * 0.28), $by, $bodyW * 0.72, $bodyH)
            $g.FillPath($appleBrush, $pathApple)
            $pathApple.Dispose()

            # Bite cutout
            $biteX = $cx + ($bodyW * 0.30)
            $biteY = $by + ($bodyH * 0.22)
            $biteD = 32.0 * $scale
            $biteBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
                [System.Drawing.PointF]::new($biteX, $biteY),
                [System.Drawing.PointF]::new($biteX + $biteD, $biteY + $biteD),
                [System.Drawing.Color]::FromArgb(255, 220, 226, 232),
                [System.Drawing.Color]::FromArgb(255, 185, 195, 204)
            )
            $g.FillEllipse($biteBrush, $biteX, $biteY, $biteD, $biteD)
            $biteBrush.Dispose()
            $appleBrush.Dispose()

            # Top stem notch
            $notchBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 225, 230, 236))
            $g.FillEllipse($notchBrush, $cx - (12.0 * $scale), $by - (4.0 * $scale), 24.0 * $scale, 16.0 * $scale)
            $notchBrush.Dispose()
        } else {
            # 16x16: Crisp simplified logo
            $appleBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 45, 52, 62))
            $g.FillEllipse($appleBrush, 5, 5, 6, 6)
            $g.FillRectangle($appleBrush, 6, 3, 2, 3)
            $appleBrush.Dispose()
        }

        # Activity LED (Green/Cyan glow)
        $ledX = $sz - $margin - [math]::Max(3.0, 14.0 * $scale)
        $ledY = $sz - $margin - [math]::Max(3.0, 14.0 * $scale)
        $ledD = [math]::Max(2.0, 8.0 * $scale)
        $ledGlowBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(180, 0, 230, 255))
        $g.FillEllipse($ledGlowBrush, $ledX - ($scale * 2.0), $ledY - ($scale * 2.0), $ledD + ($scale * 4.0), $ledD + ($scale * 4.0))
        $ledGlowBrush.Dispose()
        $ledBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 0, 255, 180))
        $g.FillEllipse($ledBrush, $ledX, $ledY, $ledD, $ledD)
        $ledBrush.Dispose()

        # APFS text label for 48px and larger
        if ($sz -ge 48) {
            $fontSize = [math]::Max(6.0, 20.0 * $scale)
            $font = New-Object System.Drawing.Font("Segoe UI", $fontSize, [System.Drawing.FontStyle]::Bold)
            $textBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(200, 30, 40, 50))
            $sf = New-Object System.Drawing.StringFormat
            $sf.Alignment = [System.Drawing.StringAlignment]::Center
            $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
            $ty = $sz - $margin - (22.0 * $scale)
            $g.DrawString("APFS", $font, $textBrush, [System.Drawing.PointF]::new($cx, $ty), $sf)
            $sf.Dispose()
            $textBrush.Dispose()
            $font.Dispose()
        }

        $g.Dispose()
        $images += $bmp
    }

    # Assemble into multi-resolution ICO file
    $fs = [System.IO.File]::Create($OutputPath)
    $bw = New-Object System.IO.BinaryWriter($fs)

    # ICONDIR
    $bw.Write([uint16]0) # Reserved
    $bw.Write([uint16]1) # Type (1=Icon)
    $bw.Write([uint16]$images.Count) # Count

    $pngStreams = @()
    foreach ($img in $images) {
        $ms = New-Object System.IO.MemoryStream
        $img.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $pngStreams += $ms
    }

    $offset = 6 + ($images.Count * 16)

    for ($i = 0; $i -lt $images.Count; $i++) {
        $img = $images[$i]
        $ms = $pngStreams[$i]
        $w = if ($img.Width -ge 256) { 0 } else { $img.Width }
        $h = if ($img.Height -ge 256) { 0 } else { $img.Height }

        $bw.Write([byte]$w)
        $bw.Write([byte]$h)
        $bw.Write([byte]0)   # Colors
        $bw.Write([byte]0)   # Reserved
        $bw.Write([uint16]1) # Planes
        $bw.Write([uint16]32)# BitCount
        $bw.Write([uint32]$ms.Length) # BytesInRes
        $bw.Write([uint32]$offset)    # ImageOffset

        $offset += $ms.Length
    }

    # Write Image Data
    for ($i = 0; $i -lt $images.Count; $i++) {
        $bytes = $pngStreams[$i].ToArray()
        $bw.Write($bytes)
        $pngStreams[$i].Dispose()
        $images[$i].Dispose()
    }

    $bw.Close()
    $fs.Close()
    Write-Output "Created $OutputPath successfully."
}

Create-ApfsIcon -OutputPath "res\apfs.ico"
