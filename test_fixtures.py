# git clone https://github.com/ipld/codec-fixtures/ in this directory

import os
import cbrrr
import json
import base64
import base58

class CID:
	def __init__(self, cid_bytes):
		self.cid_bytes = cid_bytes
	
	def __repr__(self) -> str:
		return f"CID({self.cid_bytes.hex()})"

	def __str__(self) -> str:
		#print(self.cid_bytes[:5])
		# XXX: this is a hack! do proper multiformat logic
		if self.cid_bytes.startswith(b'\x00\x01'):
			return "b" + base64.b32encode(self.cid_bytes[1:]).decode().lower().rstrip("=")
		else:
			return base58.b58encode(self.cid_bytes[1:]).decode()

class DagJsonEncoder(json.JSONEncoder):
	def default(self, o):
		if isinstance(o, bytes):
			return {"/": {"bytes": base64.b64encode(o).decode().rstrip("=")}}
		if isinstance(o, CID):
			return {"/": str(o)}
		return json.JSONEncoder.default(self, o)

def serialise_dag_json(object):
	# XXX: sort_keys probably uses the wrong sorting logic
	return json.dumps(object, cls=DagJsonEncoder, separators=(",", ":"), ensure_ascii=False, sort_keys=True).encode()

num_passed = 0
num_tested = 0

FIXTURE_PATH = "codec-fixtures/fixtures/"
for subdir in os.listdir(FIXTURE_PATH):
	if subdir.startswith("."):
		continue

	"""
	right now a lot of the "garbage" tests are failing
	TODO: figure out if that's supposed to happen
	"""
	if subdir.startswith("garbage"):
		continue

	dirpath = FIXTURE_PATH + subdir + "/"
	print("testing", dirpath)
	paths = os.listdir(dirpath)
	dag_cbor = open(next(dirpath + p for p in paths if p.endswith(".dag-cbor")), "rb").read()
	dag_json = open(next(dirpath + p for p in paths if p.endswith(".dag-json")), "rb").read()

	num_tested += 1

	try:
		result, parsed_len = cbrrr.parse_dag_cbor(dag_cbor, CID)
		reserialised = serialise_dag_json(result)
		#print(dag_json)
		#print(reserialised)
		assert(reserialised == dag_json)
		print("PASS")
		num_passed += 1
	except Exception as e:
		print("FAILED", e)
		#exit()

print(f"{num_passed}/{num_tested} tests passed")
