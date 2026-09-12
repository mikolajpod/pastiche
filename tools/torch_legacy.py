"""Read PyTorch checkpoint files without PyTorch.

Supports both serialisation formats produced by torch.save():

* the legacy format (pickle stream with a magic number, then the pickled
  object, then raw storages), used by older checkpoints such as the AdaIN
  weights from naoto0804/pytorch-AdaIN;
* the zip format (data.pkl + data/<key> entries), used since PyTorch 1.6.

Only tensors, dicts/OrderedDicts, lists/tuples and primitive values are
reconstructed; tensors become numpy arrays. Anything else (modules,
optimizers) is returned as an opaque placeholder.

Usage as a script: python tools/torch_legacy.py file.pth  -> prints the keys.
"""
import pickle
import struct
import sys
import zipfile
from collections import OrderedDict

import numpy as np

MAGIC_NUMBER = 0x1950A86A20F9469CFC6C

DTYPES = {
    "FloatStorage": np.float32,
    "DoubleStorage": np.float64,
    "HalfStorage": np.float16,
    "LongStorage": np.int64,
    "IntStorage": np.int32,
    "ShortStorage": np.int16,
    "CharStorage": np.int8,
    "ByteStorage": np.uint8,
    "BoolStorage": np.bool_,
    "BFloat16Storage": np.uint16,  # raw bits
}


class StorageType:
    def __init__(self, name):
        self.name = name
        self.dtype = np.dtype(DTYPES[name])


class Storage:
    def __init__(self, dtype, numel):
        self.dtype = dtype
        self.numel = numel
        self.data = None  # numpy 1-D array once filled


class Opaque:
    def __init__(self, module, name):
        self.module = module
        self.name = name

    def __call__(self, *args, **kwargs):
        return self

    def __setstate__(self, state):
        self.state = state

    def __repr__(self):
        return f"<{self.module}.{self.name}>"


def _rebuild_tensor(storage, storage_offset, size, stride, *rest):
    del rest
    if storage.data is None:
        raise ValueError("storage not loaded yet")
    size = tuple(int(s) for s in size)
    stride = tuple(int(s) for s in stride)
    itemsize = storage.dtype.itemsize
    base = storage.data[storage_offset:]
    if len(size) == 0:
        return np.array(base[0])
    view = np.lib.stride_tricks.as_strided(base, shape=size, strides=tuple(s * itemsize for s in stride))
    return np.ascontiguousarray(view)


class _LazyTensor:
    """Tensor whose storage is filled after the main pickle is read."""

    def __init__(self, storage, offset, size, stride):
        self.storage, self.offset, self.size, self.stride = storage, offset, size, stride

    def materialize(self):
        return _rebuild_tensor(self.storage, self.offset, self.size, self.stride)


def _lazy_rebuild(storage, storage_offset, size, stride, *rest):
    del rest
    return _LazyTensor(storage, storage_offset, size, stride)


def _make_unpickler(file, storages, loader):
    class Unpickler(pickle.Unpickler):
        def find_class(self, module, name):
            if module == "torch._utils" and name in ("_rebuild_tensor_v2", "_rebuild_tensor"):
                return _lazy_rebuild
            if module == "torch" and name in DTYPES:
                return StorageType(name)
            if module == "collections" and name == "OrderedDict":
                return OrderedDict
            if module == "torch._utils" and name == "_rebuild_parameter":
                return lambda data, requires_grad, backward_hooks: data
            if module == "torch" and name == "Size":
                return tuple
            return Opaque(module, name)

        def persistent_load(self, pid):
            return loader(pid, storages)

    return Unpickler(file, encoding="latin1")


def _materialize(obj):
    if isinstance(obj, _LazyTensor):
        return obj.materialize()
    if isinstance(obj, OrderedDict):
        return OrderedDict((k, _materialize(v)) for k, v in obj.items())
    if isinstance(obj, dict):
        return {k: _materialize(v) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_materialize(v) for v in obj]
    if isinstance(obj, tuple):
        return tuple(_materialize(v) for v in obj)
    return obj


def _load_legacy(f):
    magic = pickle.load(f)
    if magic != MAGIC_NUMBER:
        raise ValueError("not a legacy torch file (bad magic)")
    pickle.load(f)  # protocol version
    pickle.load(f)  # sys info
    storages = {}

    def loader(pid, storages):
        typename = pid[0]
        if typename != "storage":
            raise ValueError(f"unknown persistent id {typename}")
        storage_type, root_key, _location, numel = pid[1], pid[2], pid[3], pid[4]
        view_metadata = pid[5] if len(pid) > 5 else None
        if root_key not in storages:
            storages[root_key] = Storage(storage_type.dtype, int(numel))
        root = storages[root_key]
        if view_metadata is not None:
            view_key, offset, view_size = view_metadata
            view = Storage(root.dtype, int(view_size))
            view.parent = (root, int(offset))
            storages.setdefault(view_key, view)
            return view
        return root

    result = _make_unpickler(f, storages, loader).load()
    keys = pickle.load(f)
    for key in keys:
        st = storages[key]
        (numel,) = struct.unpack("<q", f.read(8))
        if numel != st.numel:
            raise ValueError(f"storage {key}: size {numel} != {st.numel}")
        raw = f.read(numel * st.dtype.itemsize)
        st.data = np.frombuffer(raw, dtype=st.dtype)
    for st in storages.values():
        if st.data is None and hasattr(st, "parent"):
            root, offset = st.parent
            st.data = root.data[offset:offset + st.numel]
    return _materialize(result)


def _load_zip(path):
    z = zipfile.ZipFile(path)
    names = z.namelist()
    pkl = [n for n in names if n.endswith("data.pkl")]
    if not pkl:
        raise ValueError("zip has no data.pkl")
    prefix = pkl[0][: -len("data.pkl")]
    storages = {}

    def loader(pid, storages):
        typename, storage_type, key, _location, numel = pid[:5]
        if typename != "storage":
            raise ValueError(f"unknown persistent id {typename}")
        if key not in storages:
            st = Storage(storage_type.dtype, int(numel))
            st.data = np.frombuffer(z.read(f"{prefix}data/{key}"), dtype=st.dtype)
            storages[key] = st
        return storages[key]

    with z.open(pkl[0]) as f:
        result = _make_unpickler(f, storages, loader).load()
    return _materialize(result)


def load(path):
    """Returns the checkpoint object with tensors as numpy arrays."""
    if zipfile.is_zipfile(path):
        return _load_zip(path)
    with open(path, "rb") as f:
        return _load_legacy(f)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    obj = load(argv[1])
    if isinstance(obj, dict):
        total = 0
        for k, v in obj.items():
            if isinstance(v, np.ndarray):
                total += v.size
                print(f"{k:40s} {str(v.dtype):8s} {tuple(v.shape)}")
            else:
                print(f"{k:40s} {type(v).__name__}")
        print(f"{len(obj)} entries, {total / 1e6:.2f} M parameters")
    else:
        print(type(obj), obj)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
