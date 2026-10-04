"""Small x64 PE reader used to audit portable DLLs, including export forwarders."""
import struct


class PE:
    def __init__(self, path):
        self.data = path.read_bytes()
        pe = struct.unpack_from('<I', self.data, 0x3c)[0]
        if self.data[pe:pe + 4] != b'PE\0\0':
            raise ValueError(f'Not a PE file: {path}')
        machine, self.count = struct.unpack_from('<HH', self.data, pe + 4)
        self.optional = pe + 24
        if machine != 0x8664 or self.u16(self.optional) != 0x20b:
            raise ValueError(f'Expected a Windows x64 binary: {path}')
        self.sections = self.optional + self.u16(pe + 20)
        self.image_base = struct.unpack_from('<Q', self.data, self.optional + 24)[0]

    def u16(self, at):
        return struct.unpack_from('<H', self.data, at)[0]

    def u32(self, at):
        return struct.unpack_from('<I', self.data, at)[0]

    def offset(self, rva):
        for index in range(self.count):
            size, address, raw_size, raw = struct.unpack_from('<IIII', self.data, self.sections + index * 40 + 8)
            if address <= rva < address + max(size, raw_size):
                return raw + rva - address
        raise ValueError(f'Invalid PE address: {rva:x}')

    def text(self, rva):
        start = self.offset(rva)
        return self.data[start:self.data.index(b'\0', start)].decode('ascii')

    def directory(self, index):
        return struct.unpack_from('<II', self.data, self.optional + 112 + index * 8)

    def imports(self):
        result = set()
        for index, stride, name_index in ((1, 20, 3), (13, 32, 1)):
            rva, size = self.directory(index)
            if not rva:
                continue
            start = self.offset(rva)
            for at in range(start, start + size, stride):
                fields = struct.unpack_from('<' + 'I' * (stride // 4), self.data, at)
                if not any(fields):
                    break
                address = fields[name_index]
                if index == 13 and not fields[0] & 1:
                    address -= self.image_base
                result.add(self.text(address).lower())
        return result

    def exports(self):
        rva, size = self.directory(0)
        if not rva:
            return set(), {}
        at = self.offset(rva)
        _, count, funcs, names, ordinals = struct.unpack_from('<IIIII', self.data, at + 20)
        exports, forwarders = set(), {}
        for index in range(count):
            name = self.text(self.u32(self.offset(names) + index * 4))
            ordinal = self.u16(self.offset(ordinals) + index * 2)
            target = self.u32(self.offset(funcs) + ordinal * 4)
            exports.add(name)
            if rva <= target < rva + size:
                forwarders[name] = self.text(target)
        return exports, forwarders
