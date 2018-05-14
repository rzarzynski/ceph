import sys
import argparse
import re

ALLOWED_TYPES = [
    "int",
    "long",
    "short",
    "char",
    "bool",
    "unsigned",
    "signed",
    "float",
    "double",
    "int8_t",
    "uint8_t",
    "int16_t",
    "uint16_t",
    "int32_t",
    "uint32_t",
    "int64_t",
    "uint64_t",
    "void",
    "size_t",
    "ssize_t",
]

def validate_type(name):
    bits = name.split(" ")
    for bit in bits:
        bit = re.sub("\*", "", bit)
        if bit == "":
            continue
        if bit == "const":
            continue
        if bit not in ALLOWED_TYPES:
            raise ValueError("Argument type '%s' is not in whitelist. "
                             "Only standard C types and fixed size integer "
                             "types should be used. struct, union, and "
                             "other complex pointer types should be "
                             "declared as 'void *'" % name)

class Arguments:
    """Event arguments description."""

    def __init__(self, args):
        """
        Parameters
        ----------
        args :
            List of (type, name) tuples or Arguments objects.
        """
        self._args = []
        for arg in args:
            if isinstance(arg, Arguments):
                self._args.extend(arg._args)
            else:
                self._args.append(arg)

    def copy(self):
        """Create a new copy."""
        return Arguments(list(self._args))

    @staticmethod
    def build(arg_str):
        """Build and Arguments instance from an argument string.

        Parameters
        ----------
        arg_str : str
            String describing the event arguments.
        """
        res = []
        for arg in arg_str.split(","):
            arg = arg.strip()
            if not arg:
                raise ValueError("Empty argument (did you forget to use 'void'?)")
            if arg == 'void':
                continue

            if '*' in arg:
                arg_type, identifier = arg.rsplit('*', 1)
                arg_type += '*'
                identifier = identifier.strip()
            else:
                arg_type, identifier = arg.rsplit(None, 1)

            validate_type(arg_type)
            res.append((arg_type, identifier))
        return Arguments(res)

    def __getitem__(self, index):
        if isinstance(index, slice):
            return Arguments(self._args[index])
        else:
            return self._args[index]

    def __iter__(self):
        """Iterate over the (type, name) pairs."""
        return iter(self._args)

    def __len__(self):
        """Number of arguments."""
        return len(self._args)

    def __str__(self):
        """String suitable for declaring function arguments."""
        if len(self._args) == 0:
            return "void"
        else:
            return ", ".join([ " ".join([t, n]) for t,n in self._args ])

    def __repr__(self):
        """Evaluable string representation for this object."""
        return "Arguments(\"%s\")" % str(self)

    def names(self):
        """List of argument names."""
        return [ name for _, name in self._args ]

    def types(self):
        """List of argument types."""
        return [ type_ for type_, _ in self._args ]

    def casted(self):
        """List of argument names casted to their type."""
        return ["(%s)%s" % (type_, name) for type_, name in self._args]

    def transform(self, *trans):
        """Return a new Arguments instance with transformed types.

        The types in the resulting Arguments instance are transformed according
        to tracetool.transform.transform_type.
        """
        res = []
        for type_, name in self._args:
            res.append((tracetool.transform.transform_type(type_, *trans),
                        name))
        return Arguments(res)


class Event(object):
    _CRE = re.compile("((?P<props>[\w\s]+)\s+)?"
                      "(?P<name>\w+)"
                      "\((?P<args>[^)]*)\)"
                      "\s*"
                      "(?:(?:(?P<fmt_trans>\".+),)?\s*(?P<fmt>\".+))?"
                      "\s*")

    def __init__(self, name, props, fmt, args, orig=None,
                 event_trans=None, event_exec=None):
        """
        Parameters
        ----------
        name : string
            Event name.
        props : list of str
            Property names.
        fmt : str, list of str
            Event printing format string(s).
        args : Arguments
            Event arguments.
        orig : Event or None
            Original Event before transformation/generation.
        event_trans : Event or None
            Generated translation-time event ("tcg" property).
        event_exec : Event or None
            Generated execution-time event ("tcg" property).

        """
        self.name = name
        self.properties = props
        self.fmt = fmt
        self.args = args
        self.event_trans = event_trans
        self.event_exec = event_exec

        if len(args._args) > 10:
            raise ValueError("Event '%s' has more than maximum permitted "
                             "argument count" % name)

    @staticmethod
    def build(line):
        """Build an Event"""
        m = Event._CRE.match(line)
        assert m is not None
        groups = m.groupdict('')

        name = groups["name"]
        props = groups["props"].split()
        fmt = groups["fmt"]
        fmt_trans = groups["fmt_trans"]
        if len(fmt_trans) > 0:
            fmt = [fmt_trans, fmt]
        args = Arguments.build(groups["args"])

        event = Event(name, props, fmt, args)

        return event

    # Star matching on PRI is dangerous as one might have multiple
    # arguments with that format, hence the non-greedy version of it.
    _FMT = re.compile("(%[\d\.]*\w+|%.*?PRI\S+)")

    def formats(self):
        """List conversion specifiers in the argument print format string."""
        assert not isinstance(self.fmt, list)
        return self._FMT.findall(self.fmt)

    QEMU_TRACE               = "trace_%(name)s"
    QEMU_TRACE_NOCHECK       = "_nocheck__" + QEMU_TRACE
    QEMU_TRACE_TCG           = QEMU_TRACE + "_tcg"
    QEMU_DSTATE              = "_TRACE_%(NAME)s_DSTATE"
    QEMU_BACKEND_DSTATE      = "TRACE_%(NAME)s_BACKEND_DSTATE"
    QEMU_EVENT               = "_TRACE_%(NAME)s_EVENT"

    def api(self, fmt=None):
        if fmt is None:
            fmt = Event.QEMU_TRACE
        return fmt % {"name": self.name, "NAME": self.name.upper()}



def out(*lines, **kwargs):
    """Write a set of output lines.

    You can use kwargs as a shorthand for mapping variables when formating all
    the strings in lines.
    """
    lines = [ l % kwargs for l in lines ]
    sys.stdout.writelines("\n".join(lines) + "\n")


class DoutWrapper(object):
    def generate_tp_file(self, events):
        pass


    def generate_header(self, events):
        for event in events:
            _args = []
            fmts = re.split('%s|%d', event.fmt.replace('"', ''))
            i = 0
            dout = 'dout (0) << '

            if "disable" not in event.properties:
                for arg in event.args:
                    dout += '"' + str(fmts[i]) + '" << ' + str(arg[1]) + " << "
                    _args.append(str(arg[1]))
                    i += 1
                dout += 'dendl;'

            out('',
                '#define  %(api)s(%(args)s) ' + dout,
                api=event.api(event.QEMU_TRACE),
                args=', '.join(_args))


class LTTngWrapper(object):
    def generate_tp_file(self, events):
        out('#include "include/int_types.h"\n')
        for e in events:
            if len(e.args) > 0:
                out('TRACEPOINT_EVENT(',
                    '   ceph_logging,',
                    '   %(name)s,',
                    '   TP_ARGS(%(args)s),',
                    '   TP_FIELDS(',
                    name=e.name,
                    args=", ".join(", ".join(i) for i in e.args))

                types = e.args.types()
                names = e.args.names()
                fmts = e.formats()
                for t,n,f in zip(types, names, fmts):
                    if ('char *' in t) or ('char*' in t):
                        out('       ctf_string(' + n + ', ' + n + ')')
                    elif ("%p" in f) or ("x" in f) or ("PRIx" in f):
                        out('       ctf_integer_hex('+ t + ', ' + n + ', ' + n + ')')
                    elif ("ptr" in t) or ("*" in t):
                        out('       ctf_integer_hex('+ t + ', ' + n + ', ' + n + ')')
                    elif ('int' in t) or ('long' in t) or ('unsigned' in t) \
                            or ('size_t' in t) or ('bool' in t):
                        out('       ctf_integer(' + t + ', ' + n + ', ' + n + ')')
                    elif ('double' in t) or ('float' in t):
                        out('       ctf_float(' + t + ', ' + n + ', ' + n + ')')
                    elif ('void *' in t) or ('void*' in t):
                        out('       ctf_integer_hex(unsigned long, ' + n + ', ' + n + ')')

                out('   )',
                    ')',
                    '')

            else:
                out('TRACEPOINT_EVENT(',
                    '   ceph_logging,',
                    '   %(name)s,',
                    '   TP_ARGS(void),',
                    '   TP_FIELDS()',
                    ')',
                    '',
                    name=e.name)


    def generate_header(self, events):
        out('#define TRACEPOINT_DEFINE',
            '#define TRACEPOINT_PROBE_DYNAMIC_LINKAGE',
            '#include "tracing/ceph_logging.h"',
            '#undef TRACEPOINT_PROBE_DYNAMIC_LINKAGE',
            '#undef TRACEPOINT_DEFINE')
        for event in events:
            out('',
                'static inline void %(api)s(%(args)s)',
                '{',
                api=event.api(event.QEMU_TRACE),
                args=event.args)

            if "disable" not in event.properties:
                argnames = ", ".join(event.args.names())
                if len(event.args) > 0:
                    argnames = ", " + argnames

                out('    tracepoint(ceph_logging, %(name)s%(tp_args)s);',
                    name=event.name,
                    tp_args=argnames)

            out('}')


def read_events(fname):
    events = []
    with open(fname, "r") as fobj:
        for lineno, line in enumerate(fobj, 1):
            if not line.strip():
                continue
            if line.lstrip().startswith('#'):
                continue

            try:
                event = Event.build(line)
                events.append(event)
            except ValueError as e:
                raise

    return events


def main(args):
    parser = argparse.ArgumentParser(description='Generate trace infrastructure.')
    parser.add_argument('-f', help='Tracepoints list file', required=True)
    parser.add_argument('-b', help='Available backends: lttng, dout', required=True)
    parser.add_argument('-t', help='Available types: tp, c', required=True)

    parsed_args = parser.parse_args(args[1:])

    events = read_events(parsed_args.f)
    if parsed_args.b == 'lttng':
        wrapper = LTTngWrapper()
    if parsed_args.b == 'dout':
        wrapper = DoutWrapper()
    if parsed_args.t == 'tp':
        wrapper.generate_tp_file(events)
    elif parsed_args.t == 'h':
        wrapper.generate_header(events)
    # elif parsed_args.t == 'c':
    #     lttngwrapper.generate_c(events)
    else:
        print('unknown format')


if __name__ == '__main__':
    main(sys.argv)
