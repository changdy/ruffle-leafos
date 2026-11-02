//! Regression cases for upstream #23960 / #23962 and permissive SWF lengths.
use std::io::{Cursor, Read};
use swf::{error::Error, read::Reader};

const MAX_INITIAL_CAPACITY: usize = 128 * 1024 * 1024;

#[test]
fn lzma_short_lengths_return_invalid_data() {
    for length in 0u32..8 {
        let mut bytes = b"ZWS\x0d".to_vec();
        bytes.extend_from_slice(&length.to_le_bytes());
        assert!(
            matches!(
                swf::decompress_swf(bytes.as_slice()),
                Err(Error::InvalidData(_))
            ),
            "length {length}"
        );
    }
    let bytes = include_bytes!("swfs/lzma-malformed-length.swf");
    assert!(matches!(
        swf::decompress_swf(bytes.as_slice()),
        Err(Error::InvalidData(_))
    ));
}

#[cfg(feature = "lzma")]
#[test]
fn inflated_lzma_lengths_preserve_valid_end_marked_content() {
    // This fixture has a complete 43-byte LZMA body with an end marker; only
    // the SWF's declared length is inflated. Restore its actual 8 + 43 length
    // for the reference, and also exercise an ordinary unmodified LZMA movie.
    let inflated_fixture = include_bytes!("swfs/lzma-length-too-large.swf");
    let mut corrected = inflated_fixture.to_vec();
    corrected[4..8].copy_from_slice(&51u32.to_le_bytes());
    for original in [corrected.as_slice(), include_bytes!("swfs/lzma.swf").as_slice()] {
        let expected = swf::decompress_swf(original).unwrap();
        let mut inflated = original.to_vec();
        inflated[4..8].copy_from_slice(&u32::MAX.to_le_bytes());
        let actual = swf::decompress_swf(inflated.as_slice()).unwrap();
        assert_eq!(actual.data, expected.data);
        assert!(actual.data.capacity() <= MAX_INITIAL_CAPACITY,
            "a declared length must not request a multi-GiB initial allocation");
        assert_eq!(
            swf::parse_swf(&actual).unwrap().tags,
            swf::parse_swf(&expected).unwrap().tags
        );
    }
}

#[cfg(feature = "lzma")]
#[test]
fn invalid_lzma_properties_return_error() {
    let mut bytes = include_bytes!("swfs/lzma-length-too-large.swf").to_vec();
    bytes[4..8].copy_from_slice(&51u32.to_le_bytes());
    bytes[12] = u8::MAX; // Invalid lc/lp/pb property byte, not a length mismatch.
    assert!(matches!(
        swf::decompress_swf(bytes.as_slice()),
        Err(Error::InvalidData(_))
    ));
}

#[test]
fn inflated_fws_and_cws_lengths_preserve_decoded_content() {
    for original in [
        include_bytes!("swfs/uncompressed.swf").as_slice(),
        include_bytes!("swfs/zlib.swf").as_slice(),
    ] {
        let expected = swf::decompress_swf(original).unwrap();
        let mut bytes = original.to_vec();
        bytes[4..8].copy_from_slice(&u32::MAX.to_le_bytes());
        let actual = swf::decompress_swf(bytes.as_slice()).unwrap();
        assert_eq!(actual.data, expected.data);
        assert!(actual.data.capacity() <= MAX_INITIAL_CAPACITY);
        assert_eq!(
            swf::parse_swf(&actual).unwrap().tags,
            swf::parse_swf(&expected).unwrap().tags
        );
    }
}

#[test]
fn decoded_content_can_grow_past_initial_capacity() {
    // One valid DefineBinaryData tag with a streamed payload larger than 128 MiB.
    // RECT(0), 30 fps, one frame; extended tag 87, character id 1, reserved 0.
    let payload_len = MAX_INITIAL_CAPACITY + 1;
    let mut prefix = b"FWS\x0d".to_vec();
    prefix.extend_from_slice(&(29u32 + payload_len as u32).to_le_bytes());
    prefix.extend_from_slice(&[0, 0, 30, 1, 0, 0xff, 0x15]);
    prefix.extend_from_slice(&(6u32 + payload_len as u32).to_le_bytes());
    prefix.extend_from_slice(&[1, 0, 0, 0, 0, 0]);
    let input = Cursor::new(prefix)
        .chain(std::io::repeat(0).take(payload_len as u64))
        .chain(Cursor::new([0x40, 0, 0, 0])); // ShowFrame, End.
    let actual = swf::decompress_swf(input).unwrap();
    assert!(actual.data.len() > MAX_INITIAL_CAPACITY);
    let parsed = swf::parse_swf(&actual).unwrap();
    assert_eq!(parsed.tags.len(), 2);
    let swf::Tag::DefineBinaryData(binary) = &parsed.tags[0] else {
        panic!("Expected DefineBinaryData");
    };
    assert_eq!(binary.id, 1);
    assert_eq!(binary.data.len(), payload_len);
    assert_eq!(parsed.tags[1], swf::Tag::ShowFrame);
}

#[test]
fn huge_scene_and_frame_label_counts_return_parse_errors() {
    for bytes in [
        include_bytes!("swfs/scene-count-too-large.swf").as_slice(),
        include_bytes!("swfs/frame-label-count-too-large.swf").as_slice(),
    ] {
        let movie = swf::decompress_swf(bytes).unwrap();
        assert!(matches!(
            swf::parse_swf(&movie),
            Err(Error::SwfParseError { tag_code: 86, .. })
        ));
    }
}

#[test]
fn scene_and_frame_label_counts_can_grow_past_initial_capacity() {
    // EncodedU32(257), followed by 257 entries: frame 0, empty label.
    let mut bytes = vec![0x81, 0x02];
    bytes.extend_from_slice(&[0; 257 * 2]);
    bytes.extend_from_slice(&[0x81, 0x02]);
    bytes.extend_from_slice(&[0; 257 * 2]);
    let data = Reader::new(&bytes, 13)
        .read_define_scene_and_frame_label_data()
        .unwrap();
    assert_eq!(data.scenes.len(), 257);
    assert_eq!(data.frame_labels.len(), 257);
    for entry in data.scenes.iter().chain(&data.frame_labels) {
        assert_eq!(entry.frame_num, 0);
        assert!(entry.label.is_empty());
    }
}

#[test]
fn bundled_game_movies_still_parse() {
    // Include external resource SWFs: they use the same changed decoder.
    fn check_directory(path: &std::path::Path) -> usize {
        let mut count = 0;
        for entry in std::fs::read_dir(path).unwrap() {
            let path = entry.unwrap().path();
            if path.is_dir() {
                count += check_directory(&path);
            } else if path.extension().is_some_and(|ext| ext == "swf") {
                let bytes = std::fs::read(&path).unwrap();
                let movie = swf::decompress_swf(bytes.as_slice())
                    .unwrap_or_else(|e| panic!("{}: {e}", path.display()));
                swf::parse_swf(&movie).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
                count += 1;
            }
        }
        count
    }
    let games = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../games/flash");
    for entrypoint in [
        "拳皇Wing1.91-定制版.swf",
        "死神VS火影3.3-定制版/死神VS火影3.3-定制版.swf",
    ] {
        assert!(games.join(entrypoint).is_file(), "Missing {entrypoint}");
    }
    assert!(check_directory(&games) >= 2);
}
