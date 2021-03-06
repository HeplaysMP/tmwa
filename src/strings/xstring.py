class XString(object):
    __slots__ = ('_value')
    name = 'tmwa::strings::XString'
    enabled = True

    def __init__(self, value):
        self._value = value

    def to_string(self):
        b = self._value['_b']['_ptr']
        e = self._value['_e']['_ptr']
        d = e - b
        return b.lazy_string(length=d)

    def children(self):
        yield 'base', self._value['_base']

    str256 = '0123456789abcdef' * 16

    test_extra = '''
    using tmwa::operator "" _s;
    '''

    tests = [
            ('tmwa::XString(""_s)', '"" = {base = nullptr}'),
            ('tmwa::XString("Hello"_s)', '"Hello" = {base = nullptr}'),
            ('tmwa::XString("' + str256[:-2] + '"_s)', '"' + str256[:-2] + '" = {base = nullptr}'),
            ('tmwa::XString("' + str256[:-1] + '"_s)', '"' + str256[:-1] + '" = {base = nullptr}'),
            ('tmwa::XString("' + str256 + '"_s)', '"' + str256 + '" = {base = nullptr}'),
            ('tmwa::XString("' + str256 + 'x"_s)', '"' + str256 + 'x" = {base = nullptr}'),
    ]
