#!/bin/bash

# OUTDIR=tracing
OUTDIR=../build/tracing
INCLDIR=../build/include/tracing

if [ $# -eq 1 ] ; then
  BACKEND=$1
else
  BACKEND=lttng
fi

echo "Generating for $BACKEND"

for f in `find tracing/tracetool/subsys -type f`; do
  # uncomment to use lttng
  if [ $BACKEND == lttng ] ; then
    python tracing/tracetool/tracetool.py -f ${f} -b lttng -t tp > ${OUTDIR}/`basename ${f}`.tp
    python tracing/tracetool/tracetool.py -f ${f} -b lttng -t h > ${INCLDIR}/`basename ${f}`_impl.h
    python tracing/tracetool/tracetool.py -f ${f} -b lttng -t c > ${OUTDIR}/`basename ${f}`.c
  else
  # uncomment to use dout
    rm -f ${OUTDIR}/`basename ${f}`.tp
    rm -f ${OUTDIR}/`basename ${f}`.c
    python tracing/tracetool/tracetool.py -f ${f} -b dout -t h > ${INCLDIR}/`basename ${f}`_impl.h
  fi
done

