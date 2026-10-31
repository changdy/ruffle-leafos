//! Leaf's optional idle-resource budgets, with a conservative memory fallback.
const MIB: u64 = 1024 * 1024;

pub fn available_bytes(meminfo: Option<&str>) -> Option<u64> {
    meminfo?.lines().find_map(|line| {
        let mut fields = line.split_whitespace();
        if fields.next()? != "MemAvailable:" { return None; }
        let kib = fields.next()?.parse::<u64>().ok()?;
        (fields.next()? == "kB").then_some(kib.saturating_mul(1024))
    })
}

pub struct CachePolicy {
    pixels: u64,
    shapes: u64,
    pressure: bool,
}

impl CachePolicy {
    pub fn new(pixels_mib: u64, shapes_mib: u64) -> Self {
        Self { pixels: pixels_mib.min(16) * MIB, shapes: shapes_mib.min(32) * MIB, pressure: false }
    }

    /// Missing or malformed counters disable optional retention until a valid
    /// sample is available. Hysteresis avoids repeated purge/refill near a limit.
    pub fn update(&mut self, meminfo: Option<&str>) -> bool {
        let available = available_bytes(meminfo);
        let previous = self.pressure;
        match available {
            Some(bytes) if bytes < 128 * MIB => self.pressure = true,
            Some(bytes) if bytes > 160 * MIB => self.pressure = false,
            None => self.pressure = true,
            _ => {}
        }
        previous != self.pressure
    }

    pub fn pixel_bytes(&self) -> usize { if self.pressure { 0 } else { self.pixels as usize } }
    pub fn shape_bytes(&self) -> u64 { if self.pressure { 0 } else { self.shapes } }
    pub fn under_pressure(&self) -> bool { self.pressure }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn memory_pressure_purges_and_recovers_with_hysteresis() {
        let mut policy = CachePolicy::new(8, 16);
        assert_eq!(policy.shape_bytes(), 16 * MIB);
        assert!(policy.update(Some("MemAvailable: 100000 kB\n")));
        assert_eq!((policy.pixel_bytes(), policy.shape_bytes()), (0, 0));
        assert!(!policy.update(Some("MemAvailable: 150000 kB\n")));
        assert!(policy.under_pressure());
        assert!(policy.update(Some("MemAvailable: 200000 kB\n")));
        assert_eq!(policy.pixel_bytes(), 8 * MIB as usize);
    }

    #[test]
    fn invalid_memory_counters_disable_retention_and_budgets_are_capped() {
        let mut policy = CachePolicy::new(u64::MAX, u64::MAX);
        assert_eq!(policy.shape_bytes(), 32 * MIB);
        assert_eq!(policy.pixel_bytes(), 16 * MIB as usize);
        assert!(policy.update(Some("MemAvailable: 999 MB\n")));
        assert_eq!(policy.shape_bytes(), 0);
        assert!(!policy.update(None));
        assert!(policy.update(Some("MemTotal: 100 kB\nMemAvailable: 200000 kB\n")));
        assert_eq!(policy.shape_bytes(), 32 * MIB);
    }
}
