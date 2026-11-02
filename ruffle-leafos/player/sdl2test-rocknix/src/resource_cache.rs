//! Leaf's optional idle-resource budgets, with a conservative memory fallback.
use std::time::Duration;

const MIB: u64 = 1024 * 1024;
const PRESSURE_THRESHOLD: u64 = 128 * MIB;

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
            Some(bytes) if bytes < PRESSURE_THRESHOLD => self.pressure = true,
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

/// Stop Vulkan only after valid low-memory samples span at least a second.
/// The first sample leaves the existing idle-cache policy time to reclaim.
#[derive(Default)]
pub struct VulkanMemoryGuard {
    low_since: Option<Duration>,
}

impl VulkanMemoryGuard {
    pub fn update(&mut self, vulkan: bool, available: Option<u64>, now: Duration) -> bool {
        if !vulkan || !available.is_some_and(|bytes| bytes < PRESSURE_THRESHOLD) {
            self.low_since = None;
            return false;
        }
        match self.low_since {
            Some(since) if now >= since => now - since >= Duration::from_secs(1),
            _ => {
                self.low_since = Some(now);
                false
            }
        }
    }
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

    #[test]
    fn single_low_sample_reclaims_idle_caches_without_stopping_vulkan() {
        let memory = "MemAvailable: 131071 kB\n";
        let mut policy = CachePolicy::new(8, 16);
        let mut guard = VulkanMemoryGuard::default();
        assert!(policy.update(Some(memory)));
        assert_eq!((policy.pixel_bytes(), policy.shape_bytes()), (0, 0));
        assert!(!guard.update(true, available_bytes(Some(memory)), Duration::ZERO));
    }

    #[test]
    fn sustained_vulkan_low_memory_requires_samples_at_least_a_second_apart() {
        let mut guard = VulkanMemoryGuard::default();
        assert!(!guard.update(true, Some(100 * MIB), Duration::from_secs(2)));
        assert!(!guard.update(true, Some(80 * MIB), Duration::from_millis(2999)));
        assert!(guard.update(true, Some(90 * MIB), Duration::from_secs(3)));
    }

    #[test]
    fn invalid_memory_sample_breaks_the_vulkan_low_memory_streak() {
        let mut guard = VulkanMemoryGuard::default();
        assert!(!guard.update(true, Some(100 * MIB), Duration::ZERO));
        assert!(!guard.update(true, available_bytes(Some("MemAvailable: invalid kB")), Duration::from_secs(1)));
        assert!(!guard.update(true, Some(100 * MIB), Duration::from_secs(2)));
        assert!(guard.update(true, Some(100 * MIB), Duration::from_secs(3)));
    }

    #[test]
    fn recovery_resets_the_guard_even_while_idle_cache_hysteresis_stays_active() {
        let mut policy = CachePolicy::new(8, 16);
        let mut guard = VulkanMemoryGuard::default();
        policy.update(Some("MemAvailable: 100000 kB\n"));
        assert!(!guard.update(true, Some(100 * MIB), Duration::ZERO));
        policy.update(Some("MemAvailable: 131072 kB\n"));
        assert!(policy.under_pressure());
        assert!(!guard.update(true, Some(128 * MIB), Duration::from_secs(1)));
        assert!(!guard.update(true, Some(100 * MIB), Duration::from_secs(2)));
        assert!(guard.update(true, Some(100 * MIB), Duration::from_secs(3)));
    }

    #[test]
    fn gles_never_triggers_and_switching_away_resets_the_vulkan_guard() {
        let mut guard = VulkanMemoryGuard::default();
        for second in 0..3 {
            assert!(!guard.update(false, Some(32 * MIB), Duration::from_secs(second)));
        }
        assert!(!guard.update(true, Some(100 * MIB), Duration::from_secs(3)));
        assert!(!guard.update(false, Some(100 * MIB), Duration::from_secs(4)));
        assert!(!guard.update(true, Some(100 * MIB), Duration::from_secs(5)));
        assert!(guard.update(true, Some(100 * MIB), Duration::from_secs(6)));
    }
}
