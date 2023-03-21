import pathlib
import cffi
import tasks
# print_banner("Building CFFI Module")
ffi = cffi.FFI()
this_dir = pathlib.Path().absolute()
h_file_name = this_dir / "libchess.ph"
with open(h_file_name) as h_file:
    ffi.cdef(h_file.read())

ffi.set_source(
    "chess",
    '#include "libchess.ph"',
    libraries=["chess", "pthread"],
    library_dirs=[this_dir.as_posix()],
#    extra_link_args=["-Wl,-rpath,-fPIC,."],
    extra_link_args=["-Wl,-rpath,-fPIC,-lpthread"],
)
ffi.compile()

