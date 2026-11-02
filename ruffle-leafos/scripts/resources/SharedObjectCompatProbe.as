package {
    import flash.display.MovieClip;
    import flash.net.SharedObject;

    // Original migration fixture. Persistence is committed by the frontend's
    // checked flush, not by a helper or by a scripted SharedObject.flush call.
    public class SharedObjectCompatProbe extends MovieClip {
        private var progress:SharedObject;

        public function SharedObjectCompatProbe() {
            stop();
            progress = SharedObject.getLocal("leaf-shared-compat", "/");
            var data:Object = progress.data;
            trace("SHARED_COMPAT " + data.marker + "|" + data.text + "|" + data.counter
                + "|" + data.ratio + "|" + data.enabled + "|" + data.items.join(",")
                + "|" + data.profile.level + "," + data.profile.note);
            data.marker = "migrated";
            data.counter = int(data.counter) + 1;
            data.items.push("new");
            data.profile.level += 1;
            data.profile.note = "persisted";
            trace("SHARED_COMPAT DIRTY");
        }
    }
}
