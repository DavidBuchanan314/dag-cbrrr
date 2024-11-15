import cbrrr
import os
import gc
# import sys

# gc.collect()
# print(len(gc.get_objects()))
gc.collect()
prev_heap = {}

for i in range(9999999999):
	try:
		# print(sys.getrefcount(cbrrr.CID))
		res = cbrrr.decode_dag_cbor(os.urandom(1024), atjson_mode=i & 1)
		# print(res)
	except Exception:# as e:
		# print(e)
		pass
	if i % 100000 == 0:
		gc.collect()
		print(len(gc.get_objects()))
		if 1:
			print("=" * 128)
			this_heap = {id(x): x for x in gc.get_objects()}
			if id(prev_heap) in this_heap:
				del this_heap[id(prev_heap)]
			if prev_heap:
				for obj in set(this_heap.keys()) - set(prev_heap.keys()):
					if obj == id(this_heap) or obj == id(prev_heap):
						continue
					print(this_heap[obj])
			prev_heap = this_heap
			print("=" * 128)
