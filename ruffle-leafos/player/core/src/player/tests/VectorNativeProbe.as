package {
    import flash.display.MovieClip;

    public class VectorNativeProbe extends MovieClip {
        private function check(label:String, actual:*, expected:*):void {
            if (actual !== expected) {
                throw new Error(label + ": actual=" + actual + ", expected=" + expected);
            }
            trace("VECTOR_PASS:" + label);
        }

        private function typed(v:Vector.<VectorToken>):void {
            v.length = 2;
            check("typed-length", v.length, 2);
            check("typed-default-null", v[0], null);
            v[0] = new VectorToken(7);
            v[1] = new VectorToken(11);
            check("typed-index", v[0].value + v[1].value, 18);
            v.fixed = true;
            check("typed-fixed", v.fixed, true);
            // Adobe's Vector setter checks fixed before comparing lengths.
            // Assigning the current length is still an error.
            try {
                v.length = 2;
                throw new Error("fixed same-length assignment did not fail");
            } catch (e:RangeError) {
                check("fixed-same-length-error", e.errorID, 1126);
            }
            try {
                v.length = 3;
                throw new Error("fixed length change did not fail");
            } catch (e:RangeError) {
                check("fixed-length-error", e.errorID, 1126);
            }
            try {
                v.push(new VectorToken(13));
                throw new Error("fixed push did not fail");
            } catch (e:RangeError) {
                check("fixed-push-error", e.errorID, 1126);
            }
            try {
                var missing:VectorToken = v[3];
                throw new Error("out-of-range index did not fail");
            } catch (e:RangeError) {
                check("index-error", e.errorID, 1125);
            }
            var dynamicVector:* = v;
            try {
                dynamicVector[0] = new MovieClip();
                throw new Error("incompatible element did not fail");
            } catch (e:TypeError) {
                check("element-type-error", e.errorID, 1034);
            }
            check("type-error-keeps-value", v[0].value, 7);
            v.fixed = false;
            check("typed-unfixed", v.fixed, false);
            check("typed-pop", v.pop().value, 11);
            check("typed-after-pop", v.length, 1);
            v.length = 3;
            check("typed-growth-null", v[2], null);
            v.length = 0;
            check("typed-truncate", v.length, 0);
        }

        public function VectorNativeProbe() {
            stop();
            try {
                runProbe();
            } catch (e:Error) {
                trace("VECTOR_PROBE_FAIL:" + e.errorID + ":" + e.message);
                throw e;
            }
        }

        private function runProbe():void {
            typed(new Vector.<VectorToken>());

            var clips:Vector.<MovieClip> = new Vector.<MovieClip>(2);
            clips[1] = new MovieClip();
            check("display-class-length", clips.length, 2);
            check("display-class-index", clips[1] is MovieClip, true);
            clips.fixed = true;
            check("display-class-fixed", clips.fixed, true);

            var objects:Vector.<Object> = new Vector.<Object>(1);
            objects[0] = "object";
            check("object-index", objects[0], "object");
            objects.length = 2;
            check("object-growth", objects[1], null);

            var strings:Vector.<String> = new Vector.<String>(1);
            strings[0] = "text";
            check("string-length", strings.length, 1);
            check("string-index", strings[0], "text");
            strings.fixed = true;
            check("string-fixed", strings.fixed, true);

            var stars:Vector.<*> = new Vector.<*>(2);
            stars[0] = 3;
            stars[1] = "star";
            check("star-length", stars.length, 2);
            check("star-index", stars[1], "star");

            var ints:Vector.<int> = new Vector.<int>(1);
            ints[0] = -9;
            check("int-length", ints.length, 1);
            check("int-index", ints[0], -9);
            var uints:Vector.<uint> = new Vector.<uint>(1);
            uints[0] = 0xffffffff;
            check("uint-index", uints[0], uint(0xffffffff));
            var numbers:Vector.<Number> = new Vector.<Number>(1);
            numbers[0] = 1.25;
            check("number-index", numbers[0], 1.25);
            trace("VECTOR_PROBE_DONE");
        }
    }
}

class VectorToken {
    public var value:int;
    public function VectorToken(value:int) { this.value = value; }
}
