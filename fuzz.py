import cbrrr
import os

cid_ctor = lambda x: x

def parse_dag_cbor_object(data):
	return cbrrr.parse_dag_cbor(data, cid_ctor)

while True:
	try:
		res = parse_dag_cbor_object(os.urandom(1024))
		print(res)
	except Exception as e:
		#print(e)
		pass
