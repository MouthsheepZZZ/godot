#pragma once

#include "core/math/vector3.h"
#include "core/os/mutex.h"
#include "core/templates/rid.h"
#include "core/typedefs.h"

class LRTRuntime {
public:
	struct State {
		RID irradiance_texture;
		Vector3 bounds_min;
		Vector3 bounds_inv_size;
		uint64_t owner_id = 0;
		bool enabled = false;
		bool indirect_only = false;
	};

private:
	static Mutex mutex;
	static State state;

public:
	static void publish(const State &p_state);
	static void clear(uint64_t p_owner_id);
	static State get_state();
};
