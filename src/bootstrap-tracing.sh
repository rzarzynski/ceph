for f in `find tracing/tracetool/subsys -type f`; do
  # uncomment to use lttng
  python tracing/tracetool/tracetool.py -f ${f} -b lttng -t tp > tracing/`basename ${f}`.tp
  python tracing/tracetool/tracetool.py -f ${f} -b lttng -t h > tracing/`basename ${f}`_impl.h
  python tracing/tracetool/tracetool.py -f ${f} -b lttng -t c > tracing/`basename ${f}`.c

  # uncomment to use dout
  #python tracing/tracetool/tracetool.py -f ${f} -b dout -t h > tracing/`basename ${f}`_impl.h
done

