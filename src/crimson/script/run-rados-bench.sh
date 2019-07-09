#!/usr/bin/env bash
#
# Ceph distributed storage system
#
# Copyright (C) 2014 Red Hat <contact@redhat.com>
#
# Author: Loic Dachary <loic@dachary.org>
#
#  This library is free software; you can redistribute it and/or
#  modify it under the terms of the GNU Lesser General Public
#  License as published by the Free Software Foundation; either
#  version 2.1 of the License, or (at your option) any later version.
#

#
# To just look at what this script will do, run it like this:
#
# $ DRY_RUN=echo ./run-make-check.sh
#

set -ex

function cluster_start() {
    MDS=0 MGR=1 OSD=1 MON=1 ../src/vstart.sh  --crimson \
        -n --without-dashboard --memstore -X --nodaemon --redirect-output &&
    bin/ceph osd pool create test-pool 128 128 &&
    bin/ceph osd pool set test-pool size 1 &&
    bin/ceph osd pool set test-pool min_size 1

    ### oops, that's really it: sleep-driven synchronization
    sleep 5
}

function cluster_stop() {
    # FIXME: crimon-osd teardown in stop.sh
    ../src/stop.sh
    killall crimson-osd
    sleep 5
}

function test_write() {
    for chunk_size in 4096 4194304
    do
        bin/rados bench -p test-pool 10 write -b ${chunk_size}
    done
}

function perfed_rados_bench() {
    perf stat -p `pgrep -u ${UID} crimson-osd` &
    bin/rados bench ${@} ;
    sleep 1; killall -INT perf
}

function test_read() {
    for chunk_size in 4096 4194304
    do
        cluster_start
        bin/rados bench -p test-pool 2 write -b ${chunk_size} --no-cleanup
        perfed_rados_bench -p test-pool 10 rand
        cluster_stop
    done
}

trap cluster_stop EXIT
test_read
