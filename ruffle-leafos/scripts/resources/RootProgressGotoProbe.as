package {
    import flash.display.MovieClip;
    import flash.events.Event;
    import flash.events.ProgressEvent;

    // A bounded recursion guard keeps a broken completion path testable.
    public class RootProgressGotoProbe extends MovieClip {
        private var progressEvents:int = 0;
        public function RootProgressGotoProbe() {
            trace("constructor");
            loaderInfo.addEventListener(ProgressEvent.PROGRESS, function(e:ProgressEvent):void {
                trace(e.bytesLoaded == e.bytesTotal && e.bytesTotal > 0 ? "progress: complete" : "progress: partial");
                if (++progressEvents > 3) return;
                trace("gotoAndStop(1)");
                gotoAndStop(1);
                trace("gotoAndStop(1) done");
            });
            loaderInfo.addEventListener(Event.INIT, function(e:Event):void { trace("init"); });
            loaderInfo.addEventListener(Event.COMPLETE, function(e:Event):void { trace("complete"); });
            addEventListener(Event.ENTER_FRAME, function(e:Event):void { trace("enterFrame"); });
            addEventListener(Event.EXIT_FRAME, function(e:Event):void { trace("exitFrame"); });
        }
    }
}
