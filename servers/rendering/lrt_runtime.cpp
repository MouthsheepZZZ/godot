#include "lrt_runtime.h"

Mutex LRTRuntime::mutex;
LRTRuntime::State LRTRuntime::state;

void LRTRuntime::publish(const State &p_state) {
	MutexLock lock(mutex);
	state = p_state;
}

void LRTRuntime::clear(uint64_t p_owner_id) {
	MutexLock lock(mutex);
	if (state.owner_id == p_owner_id) {
		state = State();
	}
}

LRTRuntime::State LRTRuntime::get_state() {
	MutexLock lock(mutex);
	return state;
}
