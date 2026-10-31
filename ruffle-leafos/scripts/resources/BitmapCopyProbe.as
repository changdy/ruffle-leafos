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
            if (actual != expected) throw new Error("COPY_PIXELS_PROBE fail actual=" + actual.toString(16) + " expected=" + expected.toString(16));
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
            trace("COPY_PIXELS_PROBE PASS cases=8");
            stop();
        }
    }
}
