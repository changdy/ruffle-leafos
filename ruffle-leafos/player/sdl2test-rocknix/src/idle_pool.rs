//! Bounded ownership of idle renderer objects, ordered by their last return.
//!
//! Evicted objects are returned to the caller so destruction can use the GL
//! context that owns them. Taking an object for active use is not an eviction.
use std::collections::VecDeque;

struct Entry<T> {
    item: T,
    bytes: u64,
}

pub struct IdlePool<T> {
    entries: VecDeque<Entry<T>>,
    limit: u64,
    max_entries: usize,
    bytes: u64,
    evictions: u64,
}

impl<T> IdlePool<T> {
    pub fn new(limit: u64, max_entries: usize) -> Self {
        Self {
            entries: VecDeque::new(),
            limit,
            max_entries,
            bytes: 0,
            evictions: 0,
        }
    }

    /// Return an object after its active use. The newest return is most recent.
    /// A disabled pool or an oversized object is rejected without disturbing
    /// useful retained objects. All returned objects require caller disposal.
    pub fn push(&mut self, item: T, bytes: u64) -> Vec<T> {
        if self.limit == 0 || self.max_entries == 0 || bytes > self.limit {
            self.evictions = self.evictions.saturating_add(1);
            return vec![item];
        }
        let mut evicted = Vec::new();
        // Subtract from the budget before adding sizes, including u64::MAX.
        while self.entries.len() >= self.max_entries || self.bytes > self.limit - bytes {
            self.evict_oldest(&mut evicted);
        }
        self.bytes += bytes;
        self.entries.push_back(Entry { item, bytes });
        evicted
    }

    /// Apply a smaller idle budget immediately, returning old objects first.
    /// Zero clears even zero-byte objects and disables further retention.
    pub fn set_limit(&mut self, limit: u64) -> Vec<T> {
        self.limit = limit;
        let mut evicted = Vec::new();
        while !self.entries.is_empty()
            && (limit == 0 || self.bytes > limit || self.entries.len() > self.max_entries)
        {
            self.evict_oldest(&mut evicted);
        }
        evicted
    }

    /// Take the eligible object with the smallest score. Ties prefer the most
    /// recently returned object. The score is evaluated once for each object.
    pub fn take_best<K: Ord>(&mut self, mut score: impl FnMut(&T) -> Option<K>) -> Option<T> {
        let mut best: Option<(usize, K)> = None;
        for (index, entry) in self.entries.iter().enumerate() {
            if let Some(value) = score(&entry.item) {
                if best.as_ref().is_none_or(|(_, previous)| value <= *previous) {
                    best = Some((index, value));
                }
            }
        }
        let (index, _) = best?;
        let entry = self.entries.remove(index)?;
        self.bytes -= entry.bytes;
        Some(entry.item)
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn bytes(&self) -> u64 {
        self.bytes
    }

    pub fn evictions(&self) -> u64 {
        self.evictions
    }

    /// Visit objects from least to most recently returned.
    pub fn iter(&self) -> impl Iterator<Item = &T> {
        self.entries.iter().map(|entry| &entry.item)
    }

    fn evict_oldest(&mut self, evicted: &mut Vec<T>) {
        // Only called when the requested insertion or shrink requires removal.
        if let Some(entry) = self.entries.pop_front() {
            self.bytes -= entry.bytes;
            self.evictions = self.evictions.saturating_add(1);
            evicted.push(entry.item);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn byte_budget_evicts_oldest_and_take_releases_the_charge() {
        let mut pool = IdlePool::new(10, 8);
        assert!(pool.push('a', 4).is_empty());
        assert!(pool.push('b', 4).is_empty());
        assert_eq!(pool.push('c', 5), vec!['a']);
        assert_eq!(pool.iter().copied().collect::<Vec<_>>(), vec!['b', 'c']);
        assert_eq!((pool.len(), pool.bytes(), pool.evictions()), (2, 9, 1));
        assert_eq!(pool.set_limit(5), vec!['b']);
        assert_eq!(pool.take_best(|_| Some(0)), Some('c'));
        assert_eq!((pool.len(), pool.bytes(), pool.evictions()), (0, 0, 2));
    }

    #[test]
    fn entry_cap_bounds_zero_byte_objects_and_zero_limits_disable_retention() {
        let mut pool = IdlePool::new(4, 2);
        pool.push('a', 0);
        pool.push('b', 0);
        assert_eq!(pool.push('c', 0), vec!['a']);
        assert_eq!((pool.len(), pool.bytes()), (2, 0));
        assert_eq!(pool.set_limit(0), vec!['b', 'c']);
        assert_eq!(pool.push('d', 0), vec!['d']);
        assert_eq!((pool.len(), pool.bytes(), pool.evictions()), (0, 0, 4));
        let mut no_entries = IdlePool::new(100, 0);
        assert_eq!(no_entries.push('e', 1), vec!['e']);
        assert_eq!((no_entries.len(), no_entries.bytes()), (0, 0));
    }

    #[test]
    fn best_fit_skips_ineligible_objects_and_prefers_recent_ties() {
        let mut pool = IdlePool::new(16, 8);
        pool.push(("old", Some(0)), 3);
        pool.push(("new", Some(0)), 4);
        pool.push(("ineligible", None), 1);
        pool.push(("worse", Some(1)), 2);
        let mut calls = 0;
        assert_eq!(pool.take_best(|item| { calls += 1; item.1 }), Some(("new", Some(0))));
        assert_eq!(calls, 4);
        assert_eq!(pool.bytes(), 6);
        assert_eq!(pool.take_best(|item| item.1), Some(("old", Some(0))));
        assert_eq!(pool.take_best(|item| item.1), Some(("worse", Some(1))));
        assert_eq!(pool.take_best(|item| item.1), None);
        assert_eq!((pool.len(), pool.bytes(), pool.evictions()), (1, 1, 0));
    }

    #[test]
    fn returning_a_taken_object_updates_its_recency() {
        let mut pool = IdlePool::new(8, 2);
        pool.push('a', 4);
        pool.push('b', 4);
        let item = pool.take_best(|item| (*item == 'a').then_some(0)).unwrap();
        assert!(pool.push(item, 4).is_empty());
        assert_eq!(pool.push('c', 4), vec!['b']);
        assert_eq!(pool.iter().copied().collect::<Vec<_>>(), vec!['a', 'c']);
        assert_eq!(pool.evictions(), 1);
    }

    #[test]
    fn pressure_clear_and_recovery_preserve_explicit_disposal() {
        let mut pool = IdlePool::new(12, 4);
        pool.push(String::from("a"), 5);
        pool.push(String::from("b"), 7);
        assert_eq!(pool.set_limit(0), vec![String::from("a"), String::from("b")]);
        assert_eq!(pool.push(String::from("disabled"), 1), vec![String::from("disabled")]);
        assert!(pool.set_limit(12).is_empty());
        assert!(pool.push(String::from("recovered"), 12).is_empty());
        assert_eq!(pool.take_best(|_| Some(0)), Some(String::from("recovered")));
        assert_eq!((pool.bytes(), pool.evictions()), (0, 3));
    }

    #[test]
    fn oversized_returns_do_not_purge_useful_objects_and_sizes_do_not_overflow() {
        let mut pool = IdlePool::new(10, 2);
        pool.push('a', 3);
        assert_eq!(pool.push('x', 11), vec!['x']);
        assert_eq!((pool.len(), pool.bytes(), pool.evictions()), (1, 3, 1));
        assert_eq!(pool.take_best(|_| Some(0)), Some('a'));

        let mut huge = IdlePool::new(u64::MAX, 4);
        assert!(huge.push(1, u64::MAX - 2).is_empty());
        assert!(huge.push(2, 2).is_empty());
        assert_eq!(huge.bytes(), u64::MAX);
        assert_eq!(huge.push(3, 1), vec![1]);
        assert_eq!((huge.len(), huge.bytes()), (2, 3));
        assert_eq!(huge.push(4, u64::MAX), vec![2, 3]);
        assert_eq!(huge.bytes(), u64::MAX);
        assert_eq!(huge.set_limit(u64::MAX - 1), vec![4]);
        assert_eq!((huge.len(), huge.bytes(), huge.evictions()), (0, 0, 4));
    }
}
