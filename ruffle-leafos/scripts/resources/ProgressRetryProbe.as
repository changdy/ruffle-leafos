package {
    import flash.display.MovieClip;
    import flash.events.Event;
    import flash.events.KeyboardEvent;
    import flash.net.SharedObject;

    // Original fixture for isolated write-failure and successful retry checks.
    // Default gamepad A maps to J; the player commits the dirty native data.
    public class ProgressRetryProbe extends MovieClip {
        { trace("PROGRESS_RETRY_PROBE CLASS_INIT"); }
        private var progress:SharedObject;
        public function ProgressRetryProbe() {
            trace("PROGRESS_RETRY_PROBE BEGIN");
            try {
                initialize();
            } catch (error:*) {
                trace("PROGRESS_RETRY_PROBE ERROR:" + error);
                throw error;
            }
        }

        private function initialize():void {
            stop();
            trace("PROGRESS_RETRY_PROBE STOPPED");
            // Isolated tests live under .userdata; the default movie path is
            // rejected by SharedObject's dot-segment guard. The frontend still
            // isolates this root-relative SO by content identity and test data.
            progress = SharedObject.getLocal("ruffle-progress-retry", "/");
            trace("PROGRESS_RETRY_PROBE OBJECT:" + progress);
            progress.data.marker = "initial";
            trace("PROGRESS_RETRY_PROBE DATA_SET");
            progress.flush();
            trace("PROGRESS_RETRY_PROBE FLUSHED");
            var keyDown:Function = function(e:KeyboardEvent):void {
                if (e.keyCode == 74) {
                    progress.data.marker = "changed-after-input";
                    trace("PROGRESS_RETRY_PROBE DIRTY");
                }
            };
            if (stage) stage.addEventListener(KeyboardEvent.KEY_DOWN, keyDown);
            else addEventListener(Event.ADDED_TO_STAGE, function(e:Event):void {
                stage.addEventListener(KeyboardEvent.KEY_DOWN, keyDown);
            });
            trace("PROGRESS_RETRY_PROBE READY");
        }
    }
}
