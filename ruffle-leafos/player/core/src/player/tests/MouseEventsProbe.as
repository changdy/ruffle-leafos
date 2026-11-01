package {
    import flash.display.MovieClip;
    import flash.display.Sprite;
    import flash.events.MouseEvent;

    // Both dynamic sprites have the same symbol id/depth. They must remain
    // distinct mouse targets; AVM1 timeline replacement equivalence is unsafe.
    public class MouseEventsProbe extends MovieClip {
        public function MouseEventsProbe() {
            make("A", 100);
            make("B", 300);
            trace("MOUSE_PROBE_READY");
            stop();
        }

        private function make(label:String, x:int):void {
            var sprite:Sprite = new Sprite();
            sprite.x = x;
            sprite.y = 100;
            sprite.graphics.beginFill(0x3388cc);
            sprite.graphics.drawRect(0, 0, 100, 100);
            sprite.graphics.endFill();
            sprite.name = label;
            var events:Array = [MouseEvent.MOUSE_DOWN, MouseEvent.MOUSE_UP,
                MouseEvent.CLICK, MouseEvent.RELEASE_OUTSIDE,
                MouseEvent.ROLL_OVER, MouseEvent.ROLL_OUT,
                MouseEvent.MOUSE_OVER, MouseEvent.MOUSE_OUT];
            for each (var type:String in events) {
                sprite.addEventListener(type, function(e:MouseEvent):void {
                    trace(e.currentTarget.name + " " + e.type);
                });
            }
            addChild(sprite);
        }
    }
}
