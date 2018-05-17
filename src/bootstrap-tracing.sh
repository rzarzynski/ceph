# uncomment to use lttng
python tracing/tracetool/tracetool.py -f tracing/trace-events.txt -b lttng -t tp > tracing/ceph_logging.tp
python tracing/tracetool/tracetool.py -f tracing/trace-events.txt -b lttng -t h > tracing/ceph_logging_impl.h

# uncomment to use dout
#python tracing/tracetool/tracetool.py -f tracing/trace-events.txt -b dout -t h > tracing/ceph_logging_impl.h

