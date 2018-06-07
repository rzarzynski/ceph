#!/bin/bash

# OUTDIR=tracing
OUTDIR=../build/tracing
INCLDIR=../build/include/tracing

for f in `find tracing/tracetool/subsys -type f`; do
  # uncomment to use lttng
  python tracing/tracetool/tracetool.py -f ${f} -b lttng -t tp > ${OUTDIR}/`basename ${f}`.tp
  python tracing/tracetool/tracetool.py -f ${f} -b lttng -t h > ${INCLDIR}/`basename ${f}`_impl.h
  python tracing/tracetool/tracetool.py -f ${f} -b lttng -t c > ${OUTDIR}/`basename ${f}`.c

  # uncomment to use dout
  #python tracing/tracetool/tracetool.py -f ${f} -b dout -t h > ${OUTDIR}/`basename ${f}`_impl.h
done

