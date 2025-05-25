import pathlib
import cffi
import tasks
ffi = cffi.FFI()
this_dir = pathlib.Path().absolute()
h_file_name = this_dir / "libchess.ph"
with open(h_file_name) as h_file:
    ffi.cdef(h_file.read())

ffi.set_source(
    "chess",
    '#include "libchess.ph"',
    libraries=["chess"],
    library_dirs=[this_dir.as_posix()],
#    extra_link_args=["-Wl,-lsqlite3"],
)
ffi.compile()

