import os
import cbrrr
import json
import base64
import base58
import unittest

class CID:
	def __init__(self, cid_bytes):
		self.cid_bytes = cid_bytes
	
	def __repr__(self) -> str:
		return f"CID({self.cid_bytes.hex()})"

	def __str__(self) -> str:
		#print(self.cid_bytes[:5])
		# XXX: this is a hack! do proper multiformat logic
		if self.cid_bytes.startswith(b'\x01'):
			return "b" + base64.b32encode(self.cid_bytes).decode().lower().rstrip("=")
		else:
			return base58.b58encode(self.cid_bytes).decode()

# XXX: this is not a good DAG-JSON encoder, it's just here because I need it
# to verify the test results
class DagJsonEncoder(json.JSONEncoder):
	def default(self, o):
		if isinstance(o, bytes):
			return {"/": {"bytes": base64.b64encode(o).decode().rstrip("=")}}
		if isinstance(o, CID):
			return {"/": str(o)}
		return json.JSONEncoder.default(self, o)


class TestFixtures(unittest.TestCase):
	FIXTURE_PATH = "codec-fixtures/fixtures/"

	def setUp(self):
		tests = []
		for subdir in os.listdir(self.FIXTURE_PATH):
			if subdir.startswith("."):
				continue

			dirpath = self.FIXTURE_PATH + subdir + "/"
			paths = os.listdir(dirpath)
			with open(next(dirpath + p for p in paths if p.endswith(".dag-cbor")), "rb") as infile:
				dag_cbor = infile.read()
			with open(next(dirpath + p for p in paths if p.endswith(".dag-json")), "rb") as outfile:
				dag_json = outfile.read()
			py_normalised_json = json.dumps(json.loads(dag_json)) # roundtrip thru python, this makes sure floats etc are in python-flavoured encodings
			tests.append((subdir, dag_cbor, py_normalised_json))
		self.tests = tests

	def test_all_fixtures(self):
		for name, cbor_in, expected in self.tests:
			result, parsed_len = cbrrr._cbrrr.decode_dag_cbor(cbor_in, CID, False)
			self.assertEqual(parsed_len, len(cbor_in))
			reserialised = json.dumps(result, cls=DagJsonEncoder, sort_keys=True)
			self.assertEqual(reserialised, expected, name)


if __name__ == '__main__':
	unittest.main(module="tests.test_fixtures")
