# python3 setup.py build
# python3 setup.py develop --user

from setuptools import setup, Extension

setup(
	name="cbrrr",
	version="0.2",
	packages=["cbrrr"],
	package_dir={"cbrrr": "src/cbrrr"},
	description="TODO: put some words here",
	ext_modules=[
		Extension(
			"_cbrrr",
			sources=["src/cbrrr/_cbrrr.c"],
			extra_compile_args=["-O3", "-Wall", "-Wextra", "-Wpedantic", "-std=c99"]
		),
	]
)
