package {
    import flash.utils.setTimeout;

    // Cache every original effect, in its original enumeration order. A timer
    // separates effects so startup stays within Ruffle's per-update time limit.
    public class RuffleSingleSwfBoot {
        public static var back:Function;

        public static function load(effects:Object):void {
            var queue:Array = [];
            for each (var effect:Object in effects) {
                queue.push(effect);
            }
            var next:Function = function():void {
                if (queue.length > 0) {
                    queue.shift().cacheBitmapData();
                    setTimeout(next, 1);
                    return;
                }
                var complete:Function = back;
                back = null;
                if (complete != null) {
                    complete();
                }
            };
            setTimeout(next, 1);
        }
    }
}
