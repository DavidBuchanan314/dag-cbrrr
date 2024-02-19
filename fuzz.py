import cbrrr
import os
#import gc
#import sys

#gc.collect()
#print(len(gc.get_objects()))

while True:
	try:
		#print(sys.getrefcount(cbrrr.CID))
		res = cbrrr.parse_dag_cbor(os.urandom(1024))
		print(res)
	except Exception as e:
		#print(e)
		pass
	#gc.collect()
	#print(len(gc.get_objects()))
