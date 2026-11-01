package {
    import flash.display.MovieClip;
    import flash.events.Event;
    import flash.events.ProgressEvent;

    // Based on the Flash Player 32 reference tests in upstream #25051.
    public class RootProgressProbe extends MovieClip {
        public function RootProgressProbe() {
            trace("constructor");
            stop();
            loaderInfo.addEventListener(ProgressEvent.PROGRESS, function(e:ProgressEvent):void {
                trace(e.bytesLoaded == e.bytesTotal && e.bytesTotal > 0 ? "progress: complete" : "progress: partial");
                play();
            });
            loaderInfo.addEventListener(Event.INIT, function(e:Event):void { trace("init"); });
            loaderInfo.addEventListener(Event.COMPLETE, function(e:Event):void { trace("complete"); });
            addEventListener(Event.ENTER_FRAME, function(e:Event):void { trace("enterFrame"); });
            addEventListener(Event.EXIT_FRAME, function(e:Event):void { trace("exitFrame"); });
        }
    }
}
