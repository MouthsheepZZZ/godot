// Face order: -X, +X, -Y, +Y, -Z, +Z. Edges/corners retain each outgoing connection.
int boundary_index(ivec3 cell, ivec3 offset, ivec3 size) {
	if (offset.x != 0) {
		return (offset.x > 0 ? size.y * size.z : 0) + cell.y + size.y * cell.z;
	}
	int base = 2 * size.y * size.z;
	if (offset.y != 0) {
		return base + (offset.y > 0 ? size.x * size.z : 0) + cell.x + size.x * cell.z;
	}
	return base + 2 * size.x * size.z + (offset.z > 0 ? size.x * size.y : 0) + cell.x + size.x * cell.y;
}

void boundary_connection(int index, ivec3 size, out ivec3 cell, out ivec3 offset) {
	int face_size = size.y * size.z;
	if (index < 2 * face_size) {
		bool high = index >= face_size;
		int local_index = index % face_size;
		cell = ivec3(high ? size.x - 1 : 0, local_index % size.y, local_index / size.y);
		offset = ivec3(high ? 1 : -1, 0, 0);
		return;
	}
	index -= 2 * face_size;
	face_size = size.x * size.z;
	if (index < 2 * face_size) {
		bool high = index >= face_size;
		int local_index = index % face_size;
		cell = ivec3(local_index % size.x, high ? size.y - 1 : 0, local_index / size.x);
		offset = ivec3(0, high ? 1 : -1, 0);
		return;
	}
	index -= 2 * face_size;
	face_size = size.x * size.y;
	bool high = index >= face_size;
	int local_index = index % face_size;
	cell = ivec3(local_index % size.x, local_index / size.x, high ? size.z - 1 : 0);
	offset = ivec3(0, 0, high ? 1 : -1);
}
