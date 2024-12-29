# python3 setup.py build
# python3 setup.py develop --user

from setuptools import setup, Extension

setup(
	name="cbrrr",
	packages=["cbrrr"],
	package_dir={"cbrrr": "src/cbrrr"},
	ext_modules=[
		Extension(
			"cbrrr._cbrrr",
			sources=["src/cbrrr/_cbrrr.c"],
			extra_compile_args=["-O3", "-Wall", "-Wextra", "-Wpedantic", "-std=c99", "-Werror"], # sorry, I hate Werror too, but this code is security-sensive and it's much better to have no build than to have an insecure build. please file a github issue if you're hitting this.
		),
	],
)
