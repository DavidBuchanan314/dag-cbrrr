import cbrrr
import os
import gc
#import sys

#gc.collect()
#print(len(gc.get_objects()))
gc.collect()

for i in range(9999999999):
	try:
		#print(sys.getrefcount(cbrrr.CID))
		res = cbrrr.decode_dag_cbor(os.urandom(1024))
		print(res)
	except Exception as e:
		#print(e)
		pass
	if i%100000 == 0:
		gc.collect()
		print(len(gc.get_objects()))
