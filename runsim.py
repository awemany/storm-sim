#!/usr/bin/env python3
# (C)opyright 2019 awemany - See file COPYING for details
# Run delta blocks simulation by filling deltablocks.h.tmpl,
# compiling, running the sim and saving it in a unique output
# directory.
import hashlib
import os
import shutil
import glob

# same as in C++ code:
BYTE=1
KILOBYTE= 1000 * BYTE
KIBIBYTE = 1024 * BYTE
MICROSEC = 1
MILLISEC = 1000 * MICROSEC
SEC = 1000 * MILLISEC

sim_defaults = dict(
    WEAK_BLOCK_TIME=600*MILLISEC,
    TRANSACTION_TIME=100*MILLISEC,
    NODE_SCENARIO="scRandom",
    NUM_NODES=10,
    NUM_NODECONNS=30,
    MEAN_LATENCY=250*MILLISEC,
    LATENCY_SIGMA=100*MILLISEC,
    LATENCY_FLOOR=50*MILLISEC,
    MEAN_BANDWIDTH=200*KILOBYTE/SEC,
    BANDWIDTH_SIGMA=20*KILOBYTE/SEC,
    BANDWIDTH_FLOOR=150*KILOBYTE/SEC,
    TXN_SIZE=223*BYTE,
    TXN_SIZE_BLOCK=100*BYTE,
    ANCESTOR_SIZE=100*BYTE,
    DELTA_BLOCK_EMPTY_SIZE=200*BYTE,
    BLOCK_REREQUEST_PROBABILITY=0.15,
    BLOCK_REQUEST_SIZE=100*BYTE,
    DOUBLESPEND_RATE=0.1,
    MINERS_DROP_KNOWN_DOUBLESPEND="true",
    RELAY_DOUBLESPENDS="true",
    TXN_MEMPOOL_MIN_AGE=0*SEC,
    TXN_MEMPOOL_MAX_AGE=72*SEC,
    USE_DELTABLOCKS_METHOD="true",
    TXN_TRACE_PROBABILITY=1.0,
    TXN_TRACE_NODE=0
)

def S(x):
    print("]", x)
    os.system(x)

class SimRunner:
    def __init__(self,
                 config
                 ):
        header_template = open("deltablocks.h.tmpl", "r").read()
        cfg = sim_defaults.copy()
        cfg.update(config)

        self.deltablocks_header = (header_template % cfg).encode("ascii")
        self.cfg_hash = hashlib.sha256(self.deltablocks_header).hexdigest()

    def run(self, *runopts):
        with open("deltablocks.h", "wb") as outf:
            outf.write(self.deltablocks_header)
        S("make")
        try:
            os.mkdir("runsim/")
        except:
            pass
        try:
            os.mkdir(self.results_dir())
        except:
            pass
        try:
            os.mkdir(os.path.join(
                self.results_dir(),
                "_".join(runopts)))
        except:
            pass

        def j(fn):
            return os.path.join(self.results_dir(), "_".join(runopts), fn)
        shutil.copyfile("Makefile", j("Makefile"))
        shutil.copyfile("deltablocks.h", j("deltablocks.h"))
        shutil.copyfile("simdeltablocks", j("simdeltablocks"))
        S("./simdeltablocks %s|gzip -c -9 > %s" % (
            " ".join(runopts), j("sim.out.gz")))
        S("touch %s" % j("sim.done"))
    def results_dir(self):
        return "runsim/%s/" % self.cfg_hash
