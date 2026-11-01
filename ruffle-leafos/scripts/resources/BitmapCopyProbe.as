package {
    import flash.display.BitmapData;
    import flash.display.MovieClip;
    import flash.display.Shape;
    import flash.geom.Point;
    import flash.geom.Rectangle;

    // Exercise real offscreen drawing and pending readbacks through the public API.
    public class BitmapCopyProbe extends MovieClip {
        private function drawn(width:int = 2, height:int = 2, color:uint = 0xff0000):BitmapData {
            var bitmap:BitmapData = new BitmapData(width, height, true, 0);
            var shape:Shape = new Shape();
            shape.graphics.beginFill(color);
            shape.graphics.drawRect(0, 0, width, height);
            shape.graphics.endFill();
            bitmap.draw(shape);
            return bitmap;
        }
        private function pixel(bitmap:BitmapData, x:int, y:int, expected:uint):void {
            var actual:uint = bitmap.getPixel32(x, y);
            if (actual != expected) throw new Error("COPY_PIXELS_PROBE fail x=" + x + " y=" + y + " actual=" + actual.toString(16) + " expected=" + expected.toString(16));
        }
        private function drawnGrid(transparent:Boolean = false):BitmapData {
            var bitmap:BitmapData = new BitmapData(8, 8, true, 0);
            var shape:Shape = new Shape();
            for (var y:int = 0; y < 8; y++) {
                for (var x:int = 0; x < 8; x++) {
                    shape.graphics.beginFill(transparent ? 0 : (y + 1) * 16 + x + 1,
                        transparent ? 128 / 255 : 1);
                    shape.graphics.drawRect(x, y, 1, 1);
                    shape.graphics.endFill();
                }
            }
            // Do not read pixels before copyPixels: its source and destination
            // must still be GPU-dirty, and copyPixels must perform the sync.
            bitmap.draw(shape);
            return bitmap;
        }
        private function overlap(label:String, dx:int, dy:int, rows:String,
                                 transparent:Boolean = false):void {
            trace("COPY_PIXELS_CASE " + label);
            var bitmap:BitmapData = drawnGrid(transparent);
            bitmap.copyPixels(bitmap, new Rectangle(2, 2, 4, 4),
                new Point(dx, dy), null, null, transparent);
            var expectedRows:Array = rows.split(";");
            for (var y:int = 0; y < 8; y++) {
                var expectedRow:Array = expectedRows[y].split(" ");
                for (var x:int = 0; x < 8; x++) {
                    var value:uint = parseInt(expectedRow[x], 16);
                    pixel(bitmap, x, y, transparent ? value << 24 : 0xff000000 | value);
                }
            }
            bitmap.dispose();
        }
        public function BitmapCopyProbe() {
            var blank:BitmapData = new BitmapData(2, 2, true, 0);
            var target:BitmapData = drawn();
            target.copyPixels(blank, blank.rect, new Point());
            pixel(target, 0, 0, 0); pixel(target, 1, 1, 0); target.dispose();

            target = drawn();
            target.copyPixels(blank, new Rectangle(0, 0, 1, 1), new Point());
            pixel(target, 0, 0, 0); pixel(target, 1, 1, 0xffff0000); target.dispose();

            var blue:BitmapData = new BitmapData(2, 2, true, 0x800000ff);
            target = drawn();
            target.copyPixels(blue, blue.rect, new Point(), null, null, true);
            pixel(target, 1, 1, 0xff7f0080); target.dispose(); blue.dispose();

            target = drawn();
            target.copyPixels(target, target.rect, new Point());
            pixel(target, 1, 1, 0xffff0000); target.dispose();

            var source:BitmapData = drawn(2, 2, 0x0000ff);
            target = drawn();
            target.copyPixels(source, source.rect, new Point());
            pixel(target, 1, 1, 0xff0000ff); target.dispose(); source.dispose();

            source = new BitmapData(3, 3, true, 0xff112233);
            source.setPixel32(2, 2, 0xff445566);
            target = drawn();
            target.copyPixels(source, new Rectangle(1, 1, 2, 2), new Point());
            pixel(target, 0, 0, 0xff112233); pixel(target, 1, 1, 0xff445566);
            target.dispose(); source.dispose();

            source = new BitmapData(2, 2, false, 0x123456);
            target = drawn();
            target.copyPixels(source, source.rect, new Point(), null, null, true);
            pixel(target, 1, 1, 0xff123456); target.dispose(); source.dispose();

            target = drawn();
            target.copyPixels(blank, blank.rect, new Point(-1, 0));
            pixel(target, 0, 1, 0); pixel(target, 1, 1, 0xffff0000);
            target.dispose(); blank.dispose();
            // Eight independent Flash Player reference matrices from upstream
            // #24676's bitmapdata_copypixels_self/output.txt. These intentionally
            // preserve Flash's overwritten-row behavior for dx<0, dy>0.
            overlap("gpu-self-right", 3, 2,
                "11 12 13 14 15 16 17 18;21 22 23 24 25 26 27 28;31 32 33 33 34 35 36 38;41 42 43 43 44 45 46 48;51 52 53 53 54 55 56 58;61 62 63 63 64 65 66 68;71 72 73 74 75 76 77 78;81 82 83 84 85 86 87 88");
            overlap("gpu-self-left", 1, 2,
                "11 12 13 14 15 16 17 18;21 22 23 24 25 26 27 28;31 33 34 35 36 36 37 38;41 43 44 45 46 46 47 48;51 53 54 55 56 56 57 58;61 63 64 65 66 66 67 68;71 72 73 74 75 76 77 78;81 82 83 84 85 86 87 88");
            overlap("gpu-self-up", 2, 1,
                "11 12 13 14 15 16 17 18;21 22 33 34 35 36 27 28;31 32 43 44 45 46 37 38;41 42 53 54 55 56 47 48;51 52 63 64 65 66 57 58;61 62 63 64 65 66 67 68;71 72 73 74 75 76 77 78;81 82 83 84 85 86 87 88");
            overlap("gpu-self-down", 2, 3,
                "11 12 13 14 15 16 17 18;21 22 23 24 25 26 27 28;31 32 33 34 35 36 37 38;41 42 33 34 35 36 47 48;51 52 43 44 45 46 57 58;61 62 53 54 55 56 67 68;71 72 63 64 65 66 77 78;81 82 83 84 85 86 87 88");
            overlap("gpu-self-down-right", 3, 3,
                "11 12 13 14 15 16 17 18;21 22 23 24 25 26 27 28;31 32 33 34 35 36 37 38;41 42 43 33 34 35 36 48;51 52 53 43 44 45 46 58;61 62 63 53 54 55 56 68;71 72 73 63 64 65 66 78;81 82 83 84 85 86 87 88");
            overlap("gpu-self-up-left", 1, 1,
                "11 12 13 14 15 16 17 18;21 33 34 35 36 26 27 28;31 43 44 45 46 36 37 38;41 53 54 55 56 46 47 48;51 63 64 65 66 56 57 58;61 62 63 64 65 66 67 68;71 72 73 74 75 76 77 78;81 82 83 84 85 86 87 88");
            overlap("gpu-self-up-right", 3, 1,
                "11 12 13 14 15 16 17 18;21 22 23 33 34 35 36 28;31 32 33 43 44 45 46 38;41 42 43 53 54 55 56 48;51 52 53 63 64 65 66 58;61 62 63 64 65 66 67 68;71 72 73 74 75 76 77 78;81 82 83 84 85 86 87 88");
            overlap("gpu-self-down-left-flash-overwrite", 1, 3,
                "11 12 13 14 15 16 17 18;21 22 23 24 25 26 27 28;31 32 33 34 35 36 37 38;41 33 34 35 36 46 47 48;51 34 35 36 46 56 57 58;61 35 36 46 56 66 67 68;71 36 46 56 66 76 77 78;81 82 83 84 85 86 87 88");

            // Black avoids RGB unpremultiplication ambiguity. Alpha 128 over
            // 128 is exactly 192 in Flash's /256 blend (the old /255 gave 191).
            // The second case reuses overwritten source rows, yielding the
            // independently calculated alpha series 192, 224, 240, 248.
            overlap("gpu-self-transparent-merge-right", 3, 2,
                "80 80 80 80 80 80 80 80;80 80 80 80 80 80 80 80;80 80 80 c0 c0 c0 c0 80;80 80 80 c0 c0 c0 c0 80;80 80 80 c0 c0 c0 c0 80;80 80 80 c0 c0 c0 c0 80;80 80 80 80 80 80 80 80;80 80 80 80 80 80 80 80", true);
            overlap("gpu-self-transparent-merge-down-left", 1, 3,
                "80 80 80 80 80 80 80 80;80 80 80 80 80 80 80 80;80 80 80 80 80 80 80 80;80 c0 c0 c0 c0 80 80 80;80 e0 e0 e0 c0 80 80 80;80 f0 f0 e0 c0 80 80 80;80 f8 f0 e0 c0 80 80 80;80 80 80 80 80 80 80 80", true);
            trace("COPY_PIXELS_PROBE PASS cases=18");
            stop();
        }
    }
}
