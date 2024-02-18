import cbrrr
import os

while True:
	try:
		res = cbrrr.parse_dag_cbor(os.urandom(1024))
		print(res)
	except Exception as e:
		#print(e)
		pass
