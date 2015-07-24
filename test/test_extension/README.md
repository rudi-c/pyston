This folder contains C extension modules.

`make check` will not run anything in this folder. Write tests in `test/test/` that import the modules instead.

Usually, when creating a new C extension test, you will create a new `.c` file, and add it to `CMakeLists.txt` and `setup.py` following the same format as other tests in those files.
