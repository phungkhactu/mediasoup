use hash_hasher::HashedMap;
use nohash_hasher::IntMap;
use parking_lot::Mutex;
use serde::Deserialize;
use std::sync::{Arc, Weak};
use uuid::Uuid;

struct EventHandlersList<F> {
    index: usize,
    callbacks: IntMap<usize, F>,
}

impl<F> Default for EventHandlersList<F> {
    fn default() -> Self {
        Self {
            index: 0,
            callbacks: IntMap::default(),
        }
    }
}

#[derive(Clone)]
pub(super) struct EventHandlers<F> {
    handlers: Arc<Mutex<HashedMap<SubscriptionTarget, EventHandlersList<F>>>>,
}

impl<F: Sized + Send + Sync + 'static> EventHandlers<F> {
    pub(super) fn new() -> Self {
        let handlers = Arc::<Mutex<HashedMap<SubscriptionTarget, EventHandlersList<F>>>>::default();
        Self { handlers }
    }

    pub(super) fn add(&self, target_id: SubscriptionTarget, callback: F) -> SubscriptionHandler {
        let index;
        {
            let mut event_handlers = self.handlers.lock();
            let list = event_handlers
                .entry(target_id)
                .or_insert_with(EventHandlersList::default);
            index = list.index;
            list.index += 1;
            list.callbacks.insert(index, callback);
            drop(event_handlers);
        }

        SubscriptionHandler::new({
            let event_handlers_weak = Arc::downgrade(&self.handlers);

            Box::new(move || {
                if let Some(event_handlers) = event_handlers_weak.upgrade() {
                    // We store removed handler here in order to drop after `event_handlers` lock is
                    // released, otherwise handler will be dropped on removal from callbacks
                    // immediately and if it happens to hold another entity that held subscription
                    // handler, we may arrive here again trying to acquire lock that we didn't
                    // release yet. By dropping callback only when lock is released we avoid
                    // deadlocking.
                    let removed_handler;
                    {
                        let mut handlers = event_handlers.lock();
                        let is_empty = {
                            let list = handlers.get_mut(&target_id).unwrap();
                            removed_handler = list.callbacks.remove(&index);
                            list.callbacks.is_empty()
                        };
                        if is_empty {
                            handlers.remove(&target_id);
                        }
                    }
                    drop(removed_handler);
                }
            })
        })
    }

    pub(super) fn downgrade(&self) -> WeakEventHandlers<F> {
        WeakEventHandlers {
            handlers: Arc::downgrade(&self.handlers),
        }
    }
}

impl<V: ?Sized> EventHandlers<Arc<dyn Fn(&V) + Send + Sync + 'static>> {
    pub(super) fn call_callbacks_with_single_value(
        &self,
        target_id: &SubscriptionTarget,
        value: &V,
    ) {
        let handlers = self.handlers.lock();
        if let Some(list) = handlers.get(target_id) {
            for callback in list.callbacks.values() {
                callback(value);
            }
        }
    }
}

impl<V1: ?Sized, V2: ?Sized> EventHandlers<Arc<dyn Fn(&V1, &V2) + Send + Sync + 'static>> {
    pub(super) fn call_callbacks_with_two_values(
        &self,
        target_id: &SubscriptionTarget,
        value1: &V1,
        value2: &V2,
    ) {
        let handlers = self.handlers.lock();
        if let Some(list) = handlers.get(target_id) {
            for callback in list.callbacks.values() {
                callback(value1, value2);
            }
        }
    }
}

#[derive(Clone)]
pub(super) struct WeakEventHandlers<F> {
    handlers: Weak<Mutex<HashedMap<SubscriptionTarget, EventHandlersList<F>>>>,
}

impl<F> WeakEventHandlers<F> {
    pub(super) fn upgrade(&self) -> Option<EventHandlers<F>> {
        self.handlers
            .upgrade()
            .map(|handlers| EventHandlers { handlers })
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Deserialize)]
#[serde(untagged)]
pub(crate) enum SubscriptionTarget {
    Uuid(Uuid),
    Number(u64),
}

impl From<u32> for SubscriptionTarget {
    fn from(number: u32) -> Self {
        Self::Number(u64::from(number))
    }
}

impl From<u64> for SubscriptionTarget {
    fn from(number: u64) -> Self {
        Self::Number(number)
    }
}

/// Subscription handler, will remove corresponding subscription when dropped
pub(crate) struct SubscriptionHandler {
    remove_callback: Option<Box<dyn FnOnce() + Send + Sync>>,
}

impl SubscriptionHandler {
    fn new(remove_callback: Box<dyn FnOnce() + Send + Sync>) -> Self {
        Self {
            remove_callback: Some(remove_callback),
        }
    }
}

impl Drop for SubscriptionHandler {
    fn drop(&mut self) {
        let remove_callback = self.remove_callback.take().unwrap();
        remove_callback();
    }
}
