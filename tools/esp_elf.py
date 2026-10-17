"""Bounded ELF32 RISC-V object-symbol conversion, using only the standard library."""
import struct


def object_symbols(data, wanted):
    """Read named linked objects; reject truncated tables, ambiguous names and bad ABI."""
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('Expected little-endian ELF32')
    fields = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
    if fields[1] != 243 or fields[2] != 1 or fields[7] != 52:
        raise ValueError('Expected ELF32 RISC-V ABI')
    offset, stride, count = fields[5], fields[10], fields[11]
    if stride != 40 or not count or offset < 52 or offset + count * stride > len(data):
        raise ValueError('Invalid ELF section table')
    sections = [struct.unpack_from('<10I', data, offset + index * stride) for index in range(count)]

    def contents(section):
        start, size = section[4], section[5]
        if start > len(data) or size > len(data) - start:
            raise ValueError('Truncated ELF section')
        return data[start:start + size]

    result = {}
    wanted = set(wanted)
    for section in sections:
        if section[1] != 2:
            continue
        if section[9] != 16 or section[5] % 16 or section[6] >= count:
            raise ValueError('Invalid ELF symbol table')
        strings = sections[section[6]]
        if strings[1] != 3:
            raise ValueError('Missing ELF symbol string table')
        names, entries = contents(strings), contents(section)
        for at in range(0, len(entries), 16):
            name, address, size, info, other, linked = struct.unpack_from('<IIIBBH', entries, at)
            if not linked or info & 15 != 1:
                continue
            if name >= len(names) or names.find(b'\0', name) < 0:
                raise ValueError('Invalid ELF symbol name')
            label = names[name:names.find(b'\0', name)].decode('utf-8', errors='strict')
            if label in wanted:
                if label in result or not address or not size or linked >= count:
                    raise ValueError('Ambiguous or invalid ELF object: ' + label)
                result[label] = {'address': address, 'size': size}
    if set(result) != wanted:
        raise ValueError('Missing linked ELF objects: ' + ', '.join(sorted(wanted - set(result))))
    return result
